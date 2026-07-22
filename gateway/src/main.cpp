#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include <karshipta/v1/envelope.pb.h>

#include "fleet_manager.h"
#include "herald_http_server.h"
#include "herald_ward_manager.h"
#include "transport.h"
#include "ward_manager.h"
#include "websocket_transport.h"

namespace {
// Both relative to the repo root, matching how the binary is documented to
// be run (docs/quickstart-windows.md, gateway/CLAUDE.local.md).
// gateway/config/ is a tracked directory (see .gitkeep).
constexpr auto kNetworkConfigPath = "gateway/config/gateway.yaml";
// Generated state, not operator-managed; gitignored (gateway/.gitignore).
constexpr auto kPersistencePath = "gateway/config/fleet_state.yaml";
// Same treatment for Fleet/Zone's SQLite store (gateway/.gitignore).
constexpr auto kFleetZoneDbPath = "gateway/config/fleet_zones.db";

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
        case karshipta::v1::Envelope::kFleetMissionAssignment:
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
}  // namespace

int main() {
    auto transport = WebsocketTransport::from_config(kNetworkConfigPath);
    auto ward_manager = WardManager::create(*transport, kPersistencePath);
    auto fleet_manager = std::make_unique<FleetManager>(*transport, kFleetZoneDbPath);
    // Depends on ward_manager (has_ward() rejects an entity_id collision
    // with a MAVLink ward), so it's constructed after it.
    auto herald_manager = std::make_unique<HeraldWardManager>(*transport, *ward_manager);
    auto herald_http = HeraldHttpServer::from_config(kNetworkConfigPath, *herald_manager);

    transport->on_connect(
        [&ward_manager, &fleet_manager, &herald_manager](const Transport::ClientId client) {
            ward_manager->send_ward_info(client);
            fleet_manager->send_fleet_zone_snapshot(client);
            herald_manager->send_known_wards(client);
        });

    transport->on_receive([&ward_manager, &fleet_manager, &transport](
                               const Transport::ClientId client, const std::vector<uint8_t>& bytes) {
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
            } else {
                ward_manager->reject_viewer_envelope(client, envelope);
            }
            return;
        }
        switch (envelope.payload_case()) {
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
            case karshipta::v1::Envelope::kFleetMissionAssignment:
                fleet_manager->handle_fleet_mission_assignment(envelope.fleet_mission_assignment(),
                                                                 *ward_manager);
                break;
            default:
                spdlog::warn("unexpected downstream payload kind {} from client {}",
                             static_cast<int>(envelope.payload_case()), client);
                break;
        }
    });
    transport->start();

    if (ward_manager->restore_and_start() == 0) {
        // First run, nothing persisted yet: seed the same default SITL
        // ward earlier milestones connected to, so `cmake --build && run`
        // still works out of the box with no console/test-client needed.
        karshipta::v1::AddWard seed;
        seed.set_ward_id("sitl-1");
        seed.set_connection_url("udp://:14540");
        seed.set_ward_class(karshipta::v1::WARD_CLASS_MULTIROTOR);
        ward_manager->handle_add_ward(seed);
    }
    ward_manager->start_publishing();
    herald_http->start();

    // Runs until externally killed: with a dynamic fleet there's no longer
    // one hardcoded ward whose disconnect should end the process. No
    // signal handling (Ctrl+C/SIGTERM) exists yet to force_stop_all() a
    // still-flying fleet first before exiting; a pre-existing gap (see
    // WardManager's destructor comment), not something this loop adds.
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
