#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <karshipta/v1/envelope.pb.h>

#include "fleet_manager.h"
#include "gt06_tcp_server.h"
#include "herald_http_server.h"
#include "herald_ward_manager.h"
#include "transport.h"
#include "websocket_transport.h"

#ifdef KARSHIPTA_GATEWAY_ENABLE_RELAY
#include "relay_transport.h"
#endif

// karshipta-gateway (drones) vs karshipta-herald (generic tags/GPS devices)
// are two separate, fixed release artifacts built from this one main.cpp
// (see root CMakeLists.txt's KARSHIPTA_GATEWAY_ENABLE_MAVLINK). ward_manager.h
// is only included, and WardManager only constructed, when this flag is on.
#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
#include "ward_manager.h"
#endif

namespace {
// Both relative to the repo root, matching how the binary is documented to
// be run (docs/quickstart-windows.md, gateway/CLAUDE.local.md).
// gateway/config/ is a tracked directory inside a checkout (see .gitkeep),
// but a released binary run standalone (a Tauri sidecar, a downloaded
// release artifact, anything outside a git checkout of this repo) has no
// .gitkeep to rely on - see ensure_config_dir_exists() below.
constexpr auto kConfigDir = "gateway/config";
constexpr auto kNetworkConfigPath = "gateway/config/gateway.yaml";
#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
// Generated state, not operator-managed; gitignored (gateway/.gitignore).
// Only WardManager persists to this path; unused (and would otherwise be an
// unused-const-variable warning under -Werror) in a MAVLink-disabled build.
constexpr auto kPersistencePath = "gateway/config/fleet_state.yaml";
#endif
// Same treatment for Fleet/Zone's SQLite store (gateway/.gitignore).
constexpr auto kFleetZoneDbPath = "gateway/config/fleet_zones.db";
// Same, for FleetMission's own SQLite store - a sibling file, not a table
// added to kFleetZoneDbPath's database (see FleetMissionStore's own header).
constexpr auto kFleetMissionDbPath = "gateway/config/fleet_missions.db";
// Default when gateway.yaml's relay.credentials_path is unset; secrets, so
// gitignored, unlike gateway.yaml itself (see relay_credentials.yaml.example).
constexpr auto kDefaultRelayCredentialsPath = "gateway/config/relay_credentials.yaml";

// Reads gateway.yaml's top-level `transport` field to decide which Transport
// implementation to construct: "websocket" (default, and the fallback for
// anything unrecognized) or "relay" (gateway/docs/relay-transport.md). Only
// ever returns a RelayTransport when this binary was actually built with
// KARSHIPTA_GATEWAY_ENABLE_RELAY=ON; logs and falls back to websocket
// otherwise, matching every other config-parsing failure mode in this file
// (observable, never a hard crash over a config problem).
std::unique_ptr<Transport> build_transport(const std::string& config_path) {
    std::string mode = "websocket";
    std::string relay_url;
    std::string relay_credentials_path = kDefaultRelayCredentialsPath;

    std::error_code exists_error;
    if (std::filesystem::exists(config_path, exists_error) && !exists_error) {
        try {
            const YAML::Node root = YAML::LoadFile(config_path);
            if (root["transport"]) mode = root["transport"].as<std::string>();
            if (const YAML::Node relay = root["relay"]; relay) {
                if (relay["url"]) relay_url = relay["url"].as<std::string>();
                if (relay["credentials_path"])
                    relay_credentials_path = relay["credentials_path"].as<std::string>();
            }
        } catch (const YAML::Exception& parse_error) {
            spdlog::error(
                "failed to parse gateway config '{}': {}; defaulting to websocket transport",
                config_path, parse_error.what());
        }
    }

    if (mode == "relay") {
#ifdef KARSHIPTA_GATEWAY_ENABLE_RELAY
        if (relay_url.empty()) {
            spdlog::error(
                "transport: relay but gateway.yaml has no relay.url set; falling back to "
                "websocket");
        } else {
            spdlog::info("transport: relay ({}), credentials from '{}'", relay_url,
                         relay_credentials_path);
            return RelayTransport::from_config(relay_url, relay_credentials_path);
        }
#else
        spdlog::error(
            "transport: relay requested but this binary was not built with "
            "-DKARSHIPTA_GATEWAY_ENABLE_RELAY=ON (see gateway/docs/relay-transport.md); "
            "falling back to websocket");
#endif
    } else if (mode != "websocket") {
        spdlog::warn("unknown transport '{}' in gateway config; defaulting to websocket", mode);
    }

    return WebsocketTransport::from_config(config_path);
}

// Upper bound on one inbound frame, checked before ParseFromArray(). Found
// during the gateway concurrency audit: IXWebSocket (v11.4.5, as vendored)
// has no maxMessageSize concept at all, so an unbounded client could send one
// arbitrarily large frame straight into the protobuf parser. No Envelope
// payload this milestone sends legitimately approaches 1MB (a mission file
// upload is the largest, and QGC/WPL/plan files for a single ward's mission
// are orders of magnitude smaller); generous enough not to reject real
// traffic, small enough to bound the parser's worst case.
constexpr std::size_t kMaxEnvelopeBytes = 1024 * 1024;

// Which manager owns rejecting a given upstream payload kind while a client
// is a read-only viewer (gateway issue #20). Kept as one small router here
// rather than teaching either manager about the other's payload kinds.
bool is_fleet_payload(const karshipta::v1::Envelope::PayloadCase payload_case) {
    switch (payload_case) {
        case karshipta::v1::Envelope::kCreateFleet:
        case karshipta::v1::Envelope::kRenameFleet:
        case karshipta::v1::Envelope::kDeleteFleet:
        case karshipta::v1::Envelope::kAddWardToFleet:
        case karshipta::v1::Envelope::kRemoveWardFromFleet:
        case karshipta::v1::Envelope::kCreateZone:
        case karshipta::v1::Envelope::kUpdateZone:
        case karshipta::v1::Envelope::kDeleteZone:
        case karshipta::v1::Envelope::kCreateFleetMission:
        case karshipta::v1::Envelope::kStopFleetMission:
        case karshipta::v1::Envelope::kRemoveFleetMission:
        case karshipta::v1::Envelope::kUpdateFleetMissionRoutes:
            return true;
        default:
            return false;
    }
}

std::vector<uint8_t> serialize_envelope(const karshipta::v1::Envelope& envelope) {
    std::vector<uint8_t> bytes(envelope.ByteSizeLong());
    if (!envelope.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()))) {
        spdlog::error("failed to serialize an Envelope of {} bytes", bytes.size());
        bytes.clear();
    }
    return bytes;
}

// Every path above assumes kConfigDir exists - inside a checkout of this
// repo it always does (tracked via .gitkeep), but nothing enforced that for
// a binary run outside one: found by actually running a release build in an
// empty directory, where FleetManager's FleetZoneStore constructor throws
// std::runtime_error opening kFleetZoneDbPath (SQLite cannot create a file
// in a directory that does not exist), uncaught here, aborting the whole
// process before it ever logs a usable reason. create_directories() is a
// no-op if the directory (or its .gitkeep-created equivalent) already
// exists, so this is safe to call unconditionally on every startup.
void ensure_config_dir_exists() {
    std::error_code error;
    std::filesystem::create_directories(kConfigDir, error);
    if (error) {
        spdlog::error("failed to create '{}': {}; startup will likely fail shortly", kConfigDir,
                      error.message());
    }
}
}  // namespace

int main() {
    ensure_config_dir_exists();
    std::unique_ptr<Transport> transport = build_transport(kNetworkConfigPath);
#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
    auto ward_manager = WardManager::create(*transport, kPersistencePath);
#endif
    auto fleet_manager =
        std::make_unique<FleetManager>(*transport, kFleetZoneDbPath, kFleetMissionDbPath);
#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
    // Depends on ward_manager (has_ward() rejects an entity_id collision
    // with a MAVLink ward), so it's constructed after it.
    auto herald_manager = std::make_unique<HeraldWardManager>(*transport, *ward_manager);

    // Lets a fleet mission's Stop/Create dispatch (see
    // FleetManager::handle_stop_fleet_mission/handle_create_fleet_mission)
    // learn its per-ward outcome without WardManager needing to know
    // FleetMission exists (see both setters' own doc comments on ward_manager.h).
    // Registered here, before start_publishing()/restore_and_start() below
    // ever let a command or upload actually run: set-once, never reassigned
    // afterward, matching persistence_path_'s own contract.
    ward_manager->set_command_outcome_observer(
        [&fleet_manager](const karshipta::v1::CommandAck& ack) {
            fleet_manager->handle_command_outcome(ack);
        });
    ward_manager->set_mission_upload_outcome_observer(
        [&fleet_manager](const std::string& ward_id, const std::string& mission_id, bool success,
                          const std::string& message) {
            fleet_manager->handle_mission_upload_outcome(ward_id, mission_id, success, message);
        });
#else
    // No WardManager exists in this build to collide-check against - there
    // are no MAVLink ward_ids at all, so nothing for a Herald entity_id to
    // collide with.
    auto herald_manager = std::make_unique<HeraldWardManager>(*transport);
#endif
    auto herald_http = HeraldHttpServer::from_config(kNetworkConfigPath, *herald_manager);
    auto gt06_tcp = Gt06TcpServer::from_config(kNetworkConfigPath, *herald_manager);

    transport->on_connect(
        [
#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
            &ward_manager,
#endif
            &fleet_manager, &herald_manager](const Transport::ClientId client) {
#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
            ward_manager->send_ward_info(client);
#endif
            fleet_manager->send_fleet_zone_snapshot(client);
            fleet_manager->send_fleet_mission_snapshot(client);
            herald_manager->send_known_wards(client);
        });

    transport->on_receive([
#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
                               &ward_manager,
#endif
                               &fleet_manager, &transport](
                               const Transport::ClientId client, const std::vector<uint8_t>& bytes) {
        if (bytes.size() > kMaxEnvelopeBytes) {
            spdlog::warn("client {} sent an oversized frame ({} bytes > {} limit), disconnecting",
                         client, bytes.size(), kMaxEnvelopeBytes);
            transport->disconnect(client);
            return;
        }
        karshipta::v1::Envelope envelope;
        if (!envelope.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
            spdlog::warn("undecodable {} byte frame from client {}", bytes.size(), client);
            return;
        }
        // Read-only viewer mode (gateway issue #20): a viewer's envelope
        // never reaches dispatch_command/handle_*, only the reasoned
        // rejection path, routed to whichever manager owns that payload kind.
        if (transport->role(client) == Transport::ClientRole::kViewer) {
            if (is_fleet_payload(envelope.payload_case())) {
                fleet_manager->reject_viewer_envelope(client, envelope);
#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
            } else {
                ward_manager->reject_viewer_envelope(client, envelope);
#else
            } else {
                spdlog::warn("viewer client {} sent unexpected payload kind {}, ignoring", client,
                             static_cast<int>(envelope.payload_case()));
#endif
            }
            return;
        }
        switch (envelope.payload_case()) {
#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
            case karshipta::v1::Envelope::kCommand:
                ward_manager->dispatch_command(envelope.command());
                break;
            case karshipta::v1::Envelope::kAddWard: {
                const auto ack = ward_manager->handle_add_ward(envelope.add_ward());
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_ward_config_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
                break;
            }
            case karshipta::v1::Envelope::kRemoveWard: {
                const auto ack = ward_manager->handle_remove_ward(envelope.remove_ward());
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_ward_config_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
                break;
            }
            case karshipta::v1::Envelope::kMissionUpload:
                ward_manager->handle_mission_upload(envelope.mission_upload());
                break;
            case karshipta::v1::Envelope::kMissionFileUpload:
                ward_manager->handle_mission_file_upload(envelope.mission_file_upload());
                break;
            case karshipta::v1::Envelope::kMissionDownloadRequest:
                ward_manager->handle_mission_download_request(envelope.mission_download_request());
                break;
#endif  // KARSHIPTA_GATEWAY_ENABLE_MAVLINK
            case karshipta::v1::Envelope::kCreateFleet: {
                const auto ack = fleet_manager->handle_create_fleet(envelope.create_fleet());
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_fleet_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
                break;
            }
            case karshipta::v1::Envelope::kRenameFleet: {
                const auto ack = fleet_manager->handle_rename_fleet(envelope.rename_fleet());
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_fleet_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
                break;
            }
            case karshipta::v1::Envelope::kDeleteFleet: {
                const auto ack = fleet_manager->handle_delete_fleet(envelope.delete_fleet());
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_fleet_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
                break;
            }
            case karshipta::v1::Envelope::kAddWardToFleet: {
                const auto ack = fleet_manager->handle_add_ward_to_fleet(envelope.add_ward_to_fleet());
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_fleet_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
                break;
            }
            case karshipta::v1::Envelope::kRemoveWardFromFleet: {
                const auto ack =
                    fleet_manager->handle_remove_ward_from_fleet(envelope.remove_ward_from_fleet());
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_fleet_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
                break;
            }
            case karshipta::v1::Envelope::kCreateZone: {
                const auto ack = fleet_manager->handle_create_zone(envelope.create_zone());
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_zone_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
                break;
            }
            case karshipta::v1::Envelope::kUpdateZone: {
                const auto ack = fleet_manager->handle_update_zone(envelope.update_zone());
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_zone_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
                break;
            }
            case karshipta::v1::Envelope::kDeleteZone: {
                const auto ack = fleet_manager->handle_delete_zone(envelope.delete_zone());
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_zone_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
                break;
            }
            case karshipta::v1::Envelope::kCreateFleetMission: {
#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
                const auto ack =
                    fleet_manager->handle_create_fleet_mission(envelope.create_fleet_mission(),
                                                                *ward_manager);
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_fleet_mission_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
#else
                spdlog::warn(
                    "client {} sent create_fleet_mission but this gateway was not built with "
                    "KARSHIPTA_GATEWAY_ENABLE_MAVLINK=ON (a fleet mission inherently needs flight "
                    "control); ignoring",
                    client);
#endif
                break;
            }
            case karshipta::v1::Envelope::kStopFleetMission: {
#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
                const auto ack =
                    fleet_manager->handle_stop_fleet_mission(envelope.stop_fleet_mission(),
                                                              *ward_manager);
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_fleet_mission_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
#else
                spdlog::warn(
                    "client {} sent stop_fleet_mission but this gateway was not built with "
                    "KARSHIPTA_GATEWAY_ENABLE_MAVLINK=ON; ignoring",
                    client);
#endif
                break;
            }
            case karshipta::v1::Envelope::kRemoveFleetMission: {
                const auto ack =
                    fleet_manager->handle_remove_fleet_mission(envelope.remove_fleet_mission());
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_fleet_mission_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
                break;
            }
            case karshipta::v1::Envelope::kUpdateFleetMissionRoutes: {
#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
                const auto ack = fleet_manager->handle_update_fleet_mission_routes(
                    envelope.update_fleet_mission_routes(), *ward_manager);
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_fleet_mission_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
#else
                spdlog::warn(
                    "client {} sent update_fleet_mission_routes but this gateway was not built "
                    "with KARSHIPTA_GATEWAY_ENABLE_MAVLINK=ON; ignoring",
                    client);
#endif
                break;
            }
            default:
                spdlog::warn("unexpected downstream payload kind {} from client {}",
                             static_cast<int>(envelope.payload_case()), client);
                break;
        }
    });
    // Only the relay transport can actually throw here (websocket's start()
    // just begins listening; relay's blocks on a real network handshake and
    // surfaces a relayly::Error on auth/connect failure, see
    // gateway/docs/relay-transport.md's "Threading and blocking") - caught
    // broadly since RelayTransport is conditionally compiled and this path
    // must build cleanly either way.
    try {
        transport->start();
    } catch (const std::exception& start_error) {
        spdlog::error("transport failed to start: {}", start_error.what());
        return 1;
    }

#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
    if (ward_manager->restore_and_start() == 0) {
        // First run, nothing persisted yet: seed the same three SITL wards
        // deploy/docker-compose.yml brings up, so `docker compose up` and a
        // bare `cmake --build && run` against a manually started SITL trio
        // both work out of the box with no console/test-client needed.
        //
        // mavlink_system_id must be set explicitly (not left at the 0
        // "bind to the first autopilot" default): all three connection
        // URLs share one Mavsdk core, so a second/third ward left at 0
        // would resolve to whichever system connected first instead of its
        // own instance. These ids (1, 2, 3) must match docker-compose.yml's
        // PX4_INSTANCE values, and the ports (24540-24542) must match where
        // each instance's offboard link sends per that file's broadcast-fix
        // wrapper - not MAVLink's conventional 14540, see that file for why.
        struct SeedWard {
            std::string ward_id;
            std::string connection_url;
            uint32_t system_id;
        };
        const SeedWard seeds[] = {
            {"sitl-1", "udp://:24540", 1},
            {"sitl-2", "udp://:24541", 2},
            {"sitl-3", "udp://:24542", 3},
        };
        for (const auto& seed_ward : seeds) {
            karshipta::v1::AddWard seed;
            seed.set_ward_id(seed_ward.ward_id);
            seed.set_connection_url(seed_ward.connection_url);
            seed.set_ward_class(karshipta::v1::WARD_CLASS_MULTIROTOR);
            seed.set_mavlink_system_id(seed_ward.system_id);
            ward_manager->handle_add_ward(seed);
        }
    }
    ward_manager->start_publishing();
#endif
    herald_http->start();
    gt06_tcp->start();

    // Runs until externally killed: with a dynamic fleet there's no longer
    // one hardcoded ward whose disconnect should end the process. No
    // signal handling (Ctrl+C/SIGTERM) exists yet to force_stop_all() a
    // still-flying fleet first before exiting; a pre-existing gap (see
    // WardManager's destructor comment), not something this loop adds.
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
