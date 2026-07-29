//
// Created by amir abkhoshk on 13/07/2026.
//

#include "ward_manager.h"

#include <google/protobuf/arena.h>
#include <karshipta/v1/envelope.pb.h>
#include <mavsdk/plugins/info/info.h>
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace {

// How often the reconnect loop polls is_connected() while waiting for a
// known-connected link to drop, and how often force_stop() polls for landing.
constexpr std::chrono::milliseconds kReconnectPollInterval{1000};

// How long force_stop() supervises an RTL before giving up and leaving the
// reconnect worker running. Generous: RTL duration scales with distance and
// altitude. Named so a config value can override it later (BRIEF.md M4).
constexpr std::chrono::seconds kForceStopLandingTimeout{120};

// BRIEF.md M2's ~5Hz per-ward telemetry target: requested from the
// autopilot on every (re)connect, and matches
// WardManager::kDefaultPublishInterval (1000ms / kTelemetryRateHz).
constexpr float kTelemetryRateHz = 5.0f;

// Only autopilot this milestone connects to (SITL); AddWard carries no
// autopilot-family field to derive this from.
constexpr auto kAutopilotName = "PX4";

// Rejection reason for every upstream envelope from a viewer connection
// (gateway issue #20). Fixed wording, not a formatted message: a viewer's
// attempt is a role check, not something whose detail varies per ward.
constexpr auto kReadOnlySessionMessage = "read-only session";

uint64_t unix_epoch_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

// Sizes come from ByteSizeLong() just above the failing call, so SerializeToArray
// cannot fail in practice; the empty-vector-and-log path exists anyway per
// gateway rule 5 (every failure observable, never silently swallowed).
std::vector<uint8_t> serialize_envelope(const karshipta::v1::Envelope& envelope) {
    std::vector<uint8_t> bytes(envelope.ByteSizeLong());
    if (!envelope.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()))) {
        spdlog::error("failed to serialize an Envelope of {} bytes", bytes.size());
        bytes.clear();
    }
    return bytes;
}

// mavsdk::Telemetry::FixType has more granularity than karshipta.v1.GpsFixType; collapse
// the extra values (NoGps, FixDgps, RtkFloat) onto their nearest proto equivalent.
karshipta::v1::GpsFixType to_proto_fix_type(const mavsdk::Telemetry::FixType fix_type) {
    switch (fix_type) {
        case mavsdk::Telemetry::FixType::NoGps:
        case mavsdk::Telemetry::FixType::NoFix:
            return karshipta::v1::GPS_FIX_TYPE_NO_FIX;
        case mavsdk::Telemetry::FixType::Fix2D:
            return karshipta::v1::GPS_FIX_TYPE_FIX_2D;
        case mavsdk::Telemetry::FixType::Fix3D:
        case mavsdk::Telemetry::FixType::FixDgps:
            return karshipta::v1::GPS_FIX_TYPE_FIX_3D;
        case mavsdk::Telemetry::FixType::RtkFloat:
        case mavsdk::Telemetry::FixType::RtkFixed:
            return karshipta::v1::GPS_FIX_TYPE_RTK;
        default:
            return karshipta::v1::GPS_FIX_TYPE_UNSPECIFIED;
    }
}

karshipta::v1::WardState build_ward_state(const std::string& ward_id,
                                          const WardConnection& connection,
                                          const TelemetryInfo& telemetry) {
    karshipta::v1::WardState state;
    state.set_ward_id(ward_id);
    state.set_timestamp_ms(unix_epoch_ms());

    const auto position = telemetry.get_position();
    auto* proto_position = state.mutable_position();
    proto_position->set_latitude_deg(position.latitude_deg);
    proto_position->set_longitude_deg(position.longitude_deg);
    proto_position->set_altitude_msl_m(position.absolute_altitude_m);
    proto_position->set_altitude_rel_m(position.relative_altitude_m);

    const auto velocity = telemetry.get_velocity_ned();
    auto* proto_velocity = state.mutable_velocity();
    proto_velocity->set_north_m_s(velocity.north_m_s);
    proto_velocity->set_east_m_s(velocity.east_m_s);
    proto_velocity->set_down_m_s(velocity.down_m_s);

    state.set_heading_deg(telemetry.get_heading_deg());

    const auto battery = telemetry.get_battery();
    auto* proto_battery = state.mutable_battery();
    proto_battery->set_voltage_v(battery.voltage_v);
    proto_battery->set_remaining_pct(battery.remaining_percent);

    const auto gps = telemetry.get_gps_info();
    auto* proto_gps = state.mutable_gps();
    proto_gps->set_fix_type(to_proto_fix_type(gps.fix_type));
    proto_gps->set_num_satellites(static_cast<uint32_t>(gps.num_satellites));
    // Gps.hdop stays unset: MAVSDK's GpsInfo does not carry it (RawGps does;
    // schema gap tracked for a later milestone).
    proto_gps->set_hdop(telemetry.get_raw_gps().hdop);

    // The gateway only ever manages MAVLink-connected wards (BRIEF.md scope;
    // non-MAVLink ward ingestion is explicitly out of scope for now), so an
    // autopilot connection - and therefore real flight_mode/armed/in_air
    // telemetry - always exists for whatever this gateway is managing,
    // regardless of the operator-assigned ward_class label. flight is always
    // populated here; a future non-MAVLink ingestion path would be the one
    // to leave it unset, not this function.
    auto* flight = state.mutable_flight();
    flight->set_flight_mode(
        ward_manager_internal::to_proto_flight_mode(telemetry.get_flight_mode()));
    flight->set_armed(telemetry.is_armed());
    flight->set_in_air(telemetry.is_in_air());
    state.set_health_ok(telemetry.is_health_ok());
    state.set_connected(connection.is_connected());

    return state;
}

// Blocking query against the Info plugin; only called once per client connect
// per ward, so a fresh plugin instance per call is cheap and needs no
// lifecycle management (unlike TelemetryInfo's persistent subscriptions).
std::string query_firmware_version(const std::shared_ptr<mavsdk::System>& system) {
    const mavsdk::Info info(system);
    const auto [result, version] = info.get_version();
    if (result != mavsdk::Info::Result::Success) {
        spdlog::warn("could not query firmware version: result={}", static_cast<int>(result));
        return {};
    }
    return std::to_string(version.flight_sw_major) + "." + std::to_string(version.flight_sw_minor) +
           "." + std::to_string(version.flight_sw_patch) +
           (version.flight_sw_git_hash.empty() ? "" : " (" + version.flight_sw_git_hash + ")");
}

// Runs fn at scope exit, success or failure, so busy-flag cleanup cannot be
// skipped by an early return or an exception thrown in an unlocked window.
template <typename F>
struct ScopeExit {
    F fn;
    ~ScopeExit() { fn(); }
};
// Explicit deduction guide: relying on C++20 aggregate CTAD alone (letting F
// deduce from the braced-init lambda with no guide) builds clean on MSVC and
// gcc but fails on clang ("no viable constructor or deduction guide") - CI's
// clang job is what actually caught this. An explicit guide sidesteps
// aggregate-CTAD support differences entirely.
template <typename F>
ScopeExit(F) -> ScopeExit<F>;

karshipta::v1::CommandAck make_rejected_ack(const karshipta::v1::Command& command,
                                            const std::string& reason) {
    karshipta::v1::CommandAck ack;
    ack.set_command_id(command.command_id());
    ack.set_ward_id(command.ward_id());
    ack.set_status(karshipta::v1::COMMAND_STATUS_REJECTED);
    ack.set_message(reason);
    return ack;
}

// Free-function counterparts of WardManager::broadcast_command_ack()/
// broadcast_rejection_event(), taking Transport& explicitly instead of
// reading transport_ off `this`. make_executor()'s ack callback calls these
// directly (never the WardManager member functions of the same name)
// specifically so that callback never captures `this` at all; the member
// functions below just delegate to these for every other caller.
void emit_command_ack(Transport& transport, const karshipta::v1::CommandAck& ack) {
    karshipta::v1::Envelope envelope;
    *envelope.mutable_command_ack() = ack;
    transport.broadcast(serialize_envelope(envelope));
}

void emit_rejection_event(Transport& transport, const karshipta::v1::CommandAck& ack) {
    karshipta::v1::Envelope envelope;
    auto* event = envelope.mutable_event();
    event->set_ward_id(ack.ward_id());
    event->set_timestamp_ms(unix_epoch_ms());
    event->set_severity(karshipta::v1::SEVERITY_WARNING);
    event->set_code("COMMAND_REJECTED");
    event->set_message(ack.message());
    transport.broadcast(serialize_envelope(envelope));
}

}  // namespace

namespace ward_manager_internal {

// mavsdk::Telemetry::FlightMode has more granularity than karshipta.v1.FlightMode; modes
// with no proto equivalent (Ready, FollowMe, Altctl, Acro, Stabilized, Rattitude) map to
// FLIGHT_MODE_UNKNOWN rather than UNSPECIFIED, since a mode IS active, it just isn't one
// the schema names yet.
karshipta::v1::FlightMode to_proto_flight_mode(const mavsdk::Telemetry::FlightMode flight_mode) {
    switch (flight_mode) {
        case mavsdk::Telemetry::FlightMode::Manual:
            return karshipta::v1::FLIGHT_MODE_MANUAL;
        case mavsdk::Telemetry::FlightMode::Hold:
            return karshipta::v1::FLIGHT_MODE_HOLD;
        case mavsdk::Telemetry::FlightMode::Mission:
            return karshipta::v1::FLIGHT_MODE_MISSION;
        case mavsdk::Telemetry::FlightMode::ReturnToLaunch:
            return karshipta::v1::FLIGHT_MODE_RETURN;
        case mavsdk::Telemetry::FlightMode::Takeoff:
            return karshipta::v1::FLIGHT_MODE_TAKEOFF;
        case mavsdk::Telemetry::FlightMode::Land:
            return karshipta::v1::FLIGHT_MODE_LAND;
        case mavsdk::Telemetry::FlightMode::Offboard:
            return karshipta::v1::FLIGHT_MODE_OFFBOARD;
        case mavsdk::Telemetry::FlightMode::Posctl:
            return karshipta::v1::FLIGHT_MODE_POSITION;
        default:
            return karshipta::v1::FLIGHT_MODE_UNKNOWN;
    }
}

}  // namespace ward_manager_internal

WardManager::WardManager(std::shared_ptr<mavsdk::Mavsdk> mavsdk, Transport& tp,
                         std::filesystem::path persistence_path)
    : mavsdk_(std::move(mavsdk)), transport_(tp), persistence_path_(std::move(persistence_path)) {}

std::unique_ptr<WardManager> WardManager::create(Transport& tp,
                                                 std::filesystem::path persistence_path) {
    return std::make_unique<WardManager>(WardConnection::create_shared_core(), tp,
                                         std::move(persistence_path));
}

WardManager::~WardManager() {
    // See the header comment: force_stop() in particular runs for up to
    // kForceStopLandingTimeoutS with no lock held, holding a
    // shared_ptr<ManagedWard>. Snapshotting every ward once (structural
    // lock) and then waiting for each one's own busy flag to clear
    // guarantees managed_wards_ never gets torn down out from under one of
    // those unlocked windows. No shared condition_variable spans this: busy
    // now lives in N per-ward mutexes, so each is polled on its own mutex_
    // instead, acceptable since this runs once, at shutdown, never on a hot
    // path.
    std::vector<std::shared_ptr<ManagedWard>> wards;
    {
        std::lock_guard lock(wards_mutex_);
        wards.reserve(managed_wards_.size());
        for (const auto& [id, ward] : managed_wards_) {
            wards.push_back(ward);
        }
    }
    for (const auto& ward : wards) {
        std::unique_lock lock(ward->mutex_);
        while (ward->busy) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            lock.lock();
        }
    }
}

std::shared_ptr<ManagedWard> WardManager::find_shared_locked(const std::string& ward_id) const {
    std::lock_guard lock(wards_mutex_);
    const auto it = managed_wards_.find(ward_id);
    return it == managed_wards_.end() ? nullptr : it->second;
}

std::unique_ptr<CommandExecutor> WardManager::make_executor(ManagedWard& ward) {
    // Captures &transport_, never `this` (see the header comment on this
    // method): keeps this callback safe to invoke even if a CommandExecutor
    // outlives WardManager itself. Calls the free functions above (not the
    // WardManager member functions of the same name) precisely because it
    // must not touch `this`. command_outcome_observer_ is copied by value for
    // the same reason: the copy's own bound target (FleetManager) is this
    // observer's problem to keep alive, not something this lambda re-reads
    // off a possibly-gone WardManager.
    Transport* transport = &transport_;
    CommandOutcomeObserver observer = command_outcome_observer_;
    return std::make_unique<CommandExecutor>(
        *ward.actions, *ward.telemetry, *ward.mission,
        [transport, observer](const karshipta::v1::CommandAck& ack) {
            emit_command_ack(*transport, ack);
            // rejected commands are events a human should see (gateway rule 5)
            if (ack.status() == karshipta::v1::COMMAND_STATUS_REJECTED) {
                emit_rejection_event(*transport, ack);
            }
            if (observer) observer(ack);
        });
}

std::optional<std::string> WardManager::add_ward_impl(const WardConfig& cfg, bool should_persist) {
    if (cfg.ward_id.empty()) {
        return "ward_id is empty";
    }

    spdlog::debug("add_ward: building object graph for '{}' (system_id={}, url={})", cfg.ward_id,
                  cfg.system_id, cfg.connection_url);

    // Built entirely without wards_mutex_: constructing this object graph
    // touches nothing shared (each object only binds to the previous one by
    // reference; WardConnection's constructor only stores state, it does
    // not connect), so there is nothing here for another thread to observe
    // or race against. The duplicate-ward_id/duplicate-system_id checks
    // that used to run before this build now run AFTER it, under the lock,
    // right next to the insert itself (see below) - checking before an
    // unlocked build would leave a window for two concurrent add_ward_impl
    // calls to both pass the check and then both try to insert the same
    // ward_id.
    const std::optional<uint32_t> expected_system_id =
        cfg.system_id == 0 ? std::nullopt : std::optional<uint32_t>{cfg.system_id};
    auto connection =
        std::make_unique<WardConnection>(mavsdk_, cfg.connection_url, expected_system_id);
    auto telemetry = std::make_unique<TelemetryInfo>(*connection);
    auto actions = std::make_unique<WardActions>(*connection);
    auto mission = std::make_unique<WardMission>(*connection);
    auto mission_importer = std::make_unique<MissionImporter>(*connection);

    // make_shared<ManagedWard>() default-constructs the aggregate (mutex_
    // default-constructs, unique_ptrs to nullptr, busy to false,
    // reconnect_worker not-joinable), then fields are filled in below;
    // ManagedWard contains a std::mutex, which is neither copyable nor
    // movable, so it cannot be built as a local variable and moved/emplaced
    // the way an earlier version of this function did.
    auto managed = std::make_shared<ManagedWard>();
    managed->config = cfg;
    managed->connection = std::move(connection);
    managed->telemetry = std::move(telemetry);
    managed->actions = std::move(actions);
    managed->mission = std::move(mission);
    managed->mission_importer = std::move(mission_importer);
    managed->executor = make_executor(*managed);

    std::lock_guard lock(wards_mutex_);
    if (managed_wards_.contains(cfg.ward_id)) {
        return "ward_id '" + cfg.ward_id + "' already registered";
    }
    // system_id 0 means "first autopilot" (fleet.proto), so zeros are not
    // identities and must not collide with each other.
    if (cfg.system_id != 0) {
        for (const auto& [id, ward] : managed_wards_) {
            if (ward->config.system_id == cfg.system_id) {
                return "system_id " + std::to_string(cfg.system_id) +
                       " already bound to ward_id '" + id + "'";
            }
        }
    }

    managed_wards_.emplace(cfg.ward_id, std::move(managed));
    spdlog::info("ward '{}' registered (system_id={}, total={})", cfg.ward_id, cfg.system_id,
                 managed_wards_.size());

    if (should_persist) {
        persist_locked();
    }
    return std::nullopt;
}

bool WardManager::add_ward(const WardConfig& cfg) {
    const auto error = add_ward_impl(cfg);
    if (error) {
        spdlog::warn("add_ward rejected: {}", *error);
        return false;
    }
    return true;
}

std::vector<std::string> WardManager::list_ward_ids() const {
    std::lock_guard lock(wards_mutex_);
    std::vector<std::string> ids;
    ids.reserve(managed_wards_.size());
    for (const auto& entry : managed_wards_) {
        ids.push_back(entry.first);
    }
    return ids;
}

bool WardManager::has_ward(const std::string& ward_id) const {
    return find_shared_locked(ward_id) != nullptr;
}

void WardManager::dispatch_command(const karshipta::v1::Command& command) {
    // The rejection ack is built under ward->mutex_ but broadcast after
    // releasing it: Transport::broadcast is a blocking socket write and must
    // not stall every other manager call behind a slow client. The accept
    // path below gets the same treatment: enqueue() synchronously broadcasts
    // its own ACCEPTED ack, so it must not run while any lock is held either.
    // find_shared_locked() only takes the structural lock (briefly); every
    // read/write below is under this one ward's own mutex_, so a slow
    // command on ward A never blocks a lookup or command for ward B.
    auto ward = find_shared_locked(command.ward_id());
    std::optional<karshipta::v1::CommandAck> rejection;
    CommandExecutor* executor = nullptr;
    if (ward == nullptr) {
        spdlog::warn("dispatch_command rejected: unknown ward_id '{}'", command.ward_id());
        rejection = make_rejected_ack(command, "unknown ward_id: " + command.ward_id());
    } else {
        std::lock_guard lock(ward->mutex_);
        if (ward->executor == nullptr) {
            spdlog::warn("dispatch_command rejected: ward_id '{}' is stopped", command.ward_id());
            rejection = make_rejected_ack(command, "ward is stopped");
        } else if (ward->busy) {
            // busy here means either a stop/force_stop/remove transition is
            // in flight, or another dispatch_command for this same ward
            // is mid-enqueue (see the comment on the executor-capture branch
            // below); either way the command is safe to retry shortly, so
            // the message must not claim the ward is stopped when it may
            // simply be a rare double-dispatch collision.
            spdlog::warn("dispatch_command rejected: ward_id '{}' is busy, retry shortly",
                         command.ward_id());
            rejection = make_rejected_ack(command, "ward is busy, retry shortly");
        } else {
            // busy blocks every quiescing path (stop/force_stop/remove) from
            // retiring this executor while it's set, so the raw pointer below
            // stays valid for the unlocked enqueue() call.
            ward->busy = true;
            executor = ward->executor.get();
        }
    }
    if (rejection) {
        broadcast_command_ack(*rejection);
        return;
    }
    executor->enqueue(command);
    clear_busy(ward);
}

void WardManager::set_command_outcome_observer(CommandOutcomeObserver observer) {
    command_outcome_observer_ = std::move(observer);
}

void WardManager::set_mission_upload_outcome_observer(MissionUploadOutcomeObserver observer) {
    mission_upload_outcome_observer_ = std::move(observer);
}

void WardManager::handle_mission_upload(const karshipta::v1::Mission& mission) {
    auto ward = find_shared_locked(mission.ward_id());
    std::optional<std::string> rejection_reason;
    WardMission* target = nullptr;
    if (ward == nullptr) {
        rejection_reason = "unknown ward_id: " + mission.ward_id();
    } else {
        std::lock_guard lock(ward->mutex_);
        if (ward->executor == nullptr) {
            rejection_reason = "ward is stopped";
        } else if (ward->busy) {
            rejection_reason = "ward is busy, retry shortly";
        } else {
            ward->busy = true;
            target = ward->mission.get();
        }
    }
    if (rejection_reason) {
        spdlog::warn("mission upload rejected: {}", *rejection_reason);
        broadcast_mission_event(mission.ward_id(), "MISSION_UPLOAD_REJECTED", *rejection_reason);
        return;
    }
    target->enqueue_upload(mission);
    clear_busy(ward);
}

std::optional<std::string> WardManager::dispatch_mission_upload_and_start(
    const karshipta::v1::Mission& mission) {
    auto ward = find_shared_locked(mission.ward_id());
    std::optional<std::string> rejection_reason;
    WardMission* target = nullptr;
    if (ward == nullptr) {
        rejection_reason = "unknown ward_id: " + mission.ward_id();
    } else {
        std::lock_guard lock(ward->mutex_);
        if (ward->executor == nullptr) {
            rejection_reason = "ward is stopped";
        } else if (ward->busy) {
            rejection_reason = "ward is busy, retry shortly";
        } else {
            ward->busy = true;
            ward->pending_auto_start_mission_id = mission.mission_id();
            target = ward->mission.get();
        }
    }
    if (rejection_reason) {
        spdlog::warn("fleet mission assignment upload rejected for ward '{}': {}",
                     mission.ward_id(), *rejection_reason);
        broadcast_mission_event(mission.ward_id(), "MISSION_UPLOAD_REJECTED", *rejection_reason);
        return rejection_reason;
    }
    target->enqueue_upload(mission);
    clear_busy(ward);
    return std::nullopt;
}

void WardManager::handle_mission_file_upload(const karshipta::v1::MissionFileUpload& upload) {
    auto ward = find_shared_locked(upload.ward_id());
    std::optional<std::string> rejection_reason;
    MissionImporter* importer = nullptr;
    WardMission* target = nullptr;
    if (ward == nullptr) {
        rejection_reason = "unknown ward_id: " + upload.ward_id();
    } else {
        std::lock_guard lock(ward->mutex_);
        if (ward->executor == nullptr) {
            rejection_reason = "ward is stopped";
        } else if (ward->busy) {
            rejection_reason = "ward is busy, retry shortly";
        } else {
            ward->busy = true;
            importer = ward->mission_importer.get();
            target = ward->mission.get();
        }
    }
    if (rejection_reason) {
        spdlog::warn("mission file upload rejected: {}", *rejection_reason);
        broadcast_mission_event(upload.ward_id(), "MISSION_UPLOAD_REJECTED", *rejection_reason);
        return;
    }

    auto [mission, reason] = importer->import(upload);
    if (!mission) {
        spdlog::warn("mission file import rejected for {}: {}", upload.ward_id(), reason);
        broadcast_mission_event(upload.ward_id(), "MISSION_IMPORT_REJECTED", reason);
        clear_busy(ward);
        return;
    }
    target->enqueue_upload(std::move(*mission));
    clear_busy(ward);
}

void WardManager::handle_mission_download_request(
    const karshipta::v1::MissionDownloadRequest& request) {
    auto ward = find_shared_locked(request.ward_id());
    std::optional<std::string> rejection_reason;
    WardMission* target = nullptr;
    if (ward == nullptr) {
        rejection_reason = "unknown ward_id: " + request.ward_id();
    } else {
        std::lock_guard lock(ward->mutex_);
        if (ward->executor == nullptr) {
            rejection_reason = "ward is stopped";
        } else if (ward->busy) {
            rejection_reason = "ward is busy, retry shortly";
        } else {
            ward->busy = true;
            target = ward->mission.get();
        }
    }
    if (rejection_reason) {
        spdlog::warn("mission download rejected: {}", *rejection_reason);
        broadcast_mission_event(request.ward_id(), "MISSION_DOWNLOAD_REJECTED", *rejection_reason);
        return;
    }
    target->enqueue_download();
    clear_busy(ward);
}

void WardManager::reject_viewer_envelope(const Transport::ClientId client,
                                         const karshipta::v1::Envelope& envelope) {
    switch (envelope.payload_case()) {
        case karshipta::v1::Envelope::kCommand: {
            const auto& command = envelope.command();
            spdlog::warn("viewer client {} attempted command '{}' on ward_id '{}', rejecting",
                         client, command.command_id(), command.ward_id());
            karshipta::v1::CommandAck ack;
            ack.set_command_id(command.command_id());
            ack.set_ward_id(command.ward_id());
            ack.set_status(karshipta::v1::COMMAND_STATUS_REJECTED);
            ack.set_message(kReadOnlySessionMessage);
            broadcast_command_ack(ack);
            // gateway rule 5: a viewer overstepping its role is an event a
            // human should see, same as any other rejected command.
            broadcast_rejection_event(ack);
            break;
        }
        case karshipta::v1::Envelope::kAddWard: {
            const auto& request = envelope.add_ward();
            spdlog::warn("viewer client {} attempted add_ward '{}', rejecting", client,
                         request.ward_id());
            karshipta::v1::Envelope ack_envelope;
            auto* ack = ack_envelope.mutable_ward_config_ack();
            ack->set_request_id(request.request_id());
            ack->set_ward_id(request.ward_id());
            ack->set_status(karshipta::v1::WARD_CONFIG_STATUS_REJECTED);
            ack->set_message(kReadOnlySessionMessage);
            transport_.broadcast(serialize_envelope(ack_envelope));
            break;
        }
        case karshipta::v1::Envelope::kRemoveWard: {
            const auto& request = envelope.remove_ward();
            spdlog::warn("viewer client {} attempted remove_ward '{}', rejecting", client,
                         request.ward_id());
            karshipta::v1::Envelope ack_envelope;
            auto* ack = ack_envelope.mutable_ward_config_ack();
            ack->set_request_id(request.request_id());
            ack->set_ward_id(request.ward_id());
            ack->set_status(karshipta::v1::WARD_CONFIG_STATUS_REJECTED);
            ack->set_message(kReadOnlySessionMessage);
            transport_.broadcast(serialize_envelope(ack_envelope));
            break;
        }
        case karshipta::v1::Envelope::kMissionUpload: {
            const auto& mission = envelope.mission_upload();
            spdlog::warn("viewer client {} attempted mission_upload on ward_id '{}', rejecting",
                         client, mission.ward_id());
            broadcast_mission_event(mission.ward_id(), "MISSION_UPLOAD_REJECTED",
                                    kReadOnlySessionMessage);
            break;
        }
        case karshipta::v1::Envelope::kMissionFileUpload: {
            const auto& upload = envelope.mission_file_upload();
            spdlog::warn(
                "viewer client {} attempted mission_file_upload on ward_id '{}', rejecting", client,
                upload.ward_id());
            broadcast_mission_event(upload.ward_id(), "MISSION_UPLOAD_REJECTED",
                                    kReadOnlySessionMessage);
            break;
        }
        case karshipta::v1::Envelope::kMissionDownloadRequest: {
            const auto& request = envelope.mission_download_request();
            spdlog::warn(
                "viewer client {} attempted mission_download_request on ward_id '{}', "
                "rejecting",
                client, request.ward_id());
            broadcast_mission_event(request.ward_id(), "MISSION_DOWNLOAD_REJECTED",
                                    kReadOnlySessionMessage);
            break;
        }
        default:
            spdlog::warn("viewer client {} sent unexpected upstream payload kind {}, ignoring",
                         client, static_cast<int>(envelope.payload_case()));
            break;
    }
}

void WardManager::broadcast_command_ack(const karshipta::v1::CommandAck& ack) const {
    emit_command_ack(transport_, ack);
    if (command_outcome_observer_) command_outcome_observer_(ack);
}

void WardManager::broadcast_rejection_event(const karshipta::v1::CommandAck& ack) const {
    emit_rejection_event(transport_, ack);
}

void WardManager::broadcast_link_event(const std::string& ward_id, bool connected) const {
    karshipta::v1::Envelope envelope;
    auto* event = envelope.mutable_event();
    event->set_ward_id(ward_id);
    event->set_timestamp_ms(unix_epoch_ms());
    if (connected) {
        event->set_severity(karshipta::v1::SEVERITY_INFO);
        event->set_code("LINK_CONNECTED");
        event->set_message("ward connected");
    } else {
        event->set_severity(karshipta::v1::SEVERITY_WARNING);
        event->set_code("LINK_LOST");
        event->set_message("ward link lost, reconnecting");
    }
    transport_.broadcast(serialize_envelope(envelope));
}

void WardManager::broadcast_mission_event(const std::string& ward_id, const std::string& code,
                                          const std::string& message) const {
    karshipta::v1::Envelope envelope;
    auto* event = envelope.mutable_event();
    event->set_ward_id(ward_id);
    event->set_timestamp_ms(unix_epoch_ms());
    event->set_severity(karshipta::v1::SEVERITY_WARNING);
    event->set_code(code);
    event->set_message(message);
    transport_.broadcast(serialize_envelope(envelope));
}

void WardManager::broadcast_mission_download(const karshipta::v1::Mission& mission) const {
    karshipta::v1::Envelope envelope;
    *envelope.mutable_mission_download() = mission;
    transport_.broadcast(serialize_envelope(envelope));
}

void WardManager::broadcast_fleet_event(const std::string& ward_id, bool added) const {
    karshipta::v1::Envelope envelope;
    auto* event = envelope.mutable_event();
    event->set_ward_id(ward_id);
    event->set_timestamp_ms(unix_epoch_ms());
    event->set_severity(karshipta::v1::SEVERITY_INFO);
    if (added) {
        event->set_code("WARD_ADDED");
        event->set_message("ward added to fleet");
    } else {
        event->set_code("WARD_REMOVED");
        event->set_message("ward removed from fleet");
    }
    transport_.broadcast(serialize_envelope(envelope));
}

bool WardManager::start(const std::string& ward_id) {
    auto ward = find_shared_locked(ward_id);
    if (ward == nullptr) {
        spdlog::warn("start rejected: unknown ward_id '{}'", ward_id);
        return false;
    }
    std::lock_guard lock(ward->mutex_);
    if (ward->busy) {
        spdlog::warn("start rejected: ward_id '{}' is mid-transition", ward_id);
        return false;
    }
    if (ward->reconnect_worker.joinable()) {
        spdlog::warn("start rejected: ward_id '{}' already running", ward_id);
        return false;
    }

    // A previous stop() retired the executor; restore the command path before
    // the ward comes back online.
    if (ward->executor == nullptr) {
        ward->executor = make_executor(*ward);
    }
    // Captures a raw pointer, not the shared_ptr itself: reconnect_worker is
    // one of ManagedWard's own members, so a shared_ptr captured here would
    // keep the object alive as long as the thread runs, which is exactly
    // what has to stop and join for the object to ever be destroyed in the
    // first place, i.e. a self-referential leak/deadlock. The raw pointer is
    // safe because reconnect_worker's own lifetime is always bounded by
    // ManagedWard's lifetime (it's a member, declared last so it stops and
    // joins before anything else in the struct is torn down).
    ManagedWard* raw_ward = ward.get();
    ward->reconnect_worker = std::jthread([this, raw_ward](std::stop_token stop_token) {
        run_reconnect_loop(*raw_ward, stop_token);
    });
    return true;
}

void WardManager::start_all() {
    // Snapshot ids first: start() takes the lock itself, and iterating the
    // map while calling a locking method would self-deadlock.
    for (const auto& ward_id : list_ward_ids()) {
        start(ward_id);
    }
}

void WardManager::persist_locked() const {
    if (persistence_path_.empty()) {
        return;
    }
    YAML::Node root;
    YAML::Node wards(YAML::NodeType::Sequence);
    for (const auto& [id, ward] : managed_wards_) {
        YAML::Node entry;
        entry["ward_id"] = ward->config.ward_id;
        entry["connection_url"] = ward->config.connection_url;
        entry["mavlink_system_id"] = ward->config.system_id;
        entry["name"] = ward->config.name;
        entry["ward_class"] = karshipta::v1::WardClass_Name(ward->config.ward_class);
        wards.push_back(entry);
    }
    root["wards"] = wards;

    // gateway/config/ is a tracked directory (see .gitkeep); this is internal
    // machinery, not an operator-managed path, so no runtime mkdir here.
    //
    // Written to a temp file in the same directory first, then renamed over
    // the real path: rename() is atomic on both POSIX and NTFS, so a crash
    // mid-write can never corrupt or empty the only crash-recovery record
    // for the fleet the way a direct truncate-write could.
    const auto temp_path = persistence_path_.string() + ".tmp";
    {
        std::ofstream out(temp_path, std::ios::trunc);
        if (!out) {
            spdlog::error("failed to open '{}' for writing persisted fleet state", temp_path);
            return;
        }
        out << root;
        if (!out) {
            spdlog::error("failed to write persisted fleet state to '{}'", temp_path);
            return;
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temp_path, persistence_path_, rename_error);
    if (rename_error) {
        spdlog::error("failed to replace '{}' with the freshly written '{}': {}",
                      persistence_path_.string(), temp_path, rename_error.message());
    }
}

std::size_t WardManager::load_persisted() {
    if (persistence_path_.empty()) {
        return 0;
    }
    std::error_code exists_error;
    if (!std::filesystem::exists(persistence_path_, exists_error) || exists_error) {
        return 0;
    }

    YAML::Node root;
    try {
        root = YAML::LoadFile(persistence_path_.string());
    } catch (const YAML::Exception& parse_error) {
        spdlog::error("failed to parse persisted fleet state '{}': {}", persistence_path_.string(),
                      parse_error.what());
        return 0;
    }

    const YAML::Node wards = root["wards"];
    if (!wards || !wards.IsSequence()) {
        return 0;
    }

    std::size_t loaded = 0;
    for (const auto& entry : wards) {
        try {
            WardConfig cfg;
            cfg.ward_id = entry["ward_id"].as<std::string>();
            cfg.connection_url = entry["connection_url"].as<std::string>();
            cfg.system_id = entry["mavlink_system_id"].as<unsigned int>();
            cfg.name = entry["name"] ? entry["name"].as<std::string>() : std::string{};
            const std::string ward_class_name =
                entry["ward_class"] ? entry["ward_class"].as<std::string>() : std::string{};
            if (!ward_class_name.empty() &&
                !karshipta::v1::WardClass_Parse(ward_class_name, &cfg.ward_class)) {
                spdlog::warn(
                    "persisted ward '{}' has unknown class '{}', defaulting to unspecified",
                    cfg.ward_id, ward_class_name);
            }
            // should_persist=false: reconstructing entries already on disk
            // must not rewrite the file once per entry while loading it.
            if (const auto error = add_ward_impl(cfg, /*should_persist=*/false)) {
                spdlog::warn("skipping persisted ward '{}': {}", cfg.ward_id, *error);
                continue;
            }
            ++loaded;
        } catch (const YAML::Exception& entry_error) {
            spdlog::warn("skipping malformed persisted ward entry: {}", entry_error.what());
        }
    }
    spdlog::info("loaded {} ward(s) from '{}'", loaded, persistence_path_.string());
    return loaded;
}

std::size_t WardManager::restore_and_start() {
    const std::size_t loaded = load_persisted();
    start_all();
    return loaded;
}

void WardManager::start_publishing(std::chrono::milliseconds interval) {
    publish_worker_ = std::jthread(
        [this, interval](std::stop_token stop_token) { run_publish_loop(interval, stop_token); });
}

void WardManager::run_publish_loop(std::chrono::milliseconds interval, std::stop_token stop_token) {
    auto next_tick = std::chrono::steady_clock::now() + interval;
    while (!stop_token.stop_requested()) {
        // One Arena per tick: an Envelope's mutable_*() otherwise heap-new's
        // its submessage individually, up to twice per ward per tick (state
        // + mission progress) forever. Arena-allocating the Envelope makes
        // every submessage set on it arena-allocated too, so a tick's worth
        // of messages is one bump-pointer region freed in one shot when
        // `arena` goes out of scope, not N/2N individual new/delete pairs.
        google::protobuf::Arena arena;
        std::vector<std::vector<uint8_t>> frames;
        // Deferred so broadcast_mission_event()/broadcast_mission_download()
        // (blocking socket writes) never run while any lock is held, same
        // rule as everywhere else in this file. mission set means "broadcast
        // this download"; otherwise code/message carry an event.
        struct DeferredMissionBroadcast {
            std::string ward_id;
            std::optional<karshipta::v1::Mission> mission;
            std::string code;
            std::string message;
        };
        std::vector<DeferredMissionBroadcast> deferred_mission_broadcasts;
        // ward + executor to enqueue a synthetic RTL command on, once
        // unlocked; mirrors dispatch_command's busy-then-unlocked-enqueue
        // pattern so a concurrent remove_ward() can't destroy the executor
        // out from under this deferred call. Keeps the ward's shared_ptr
        // (not just its id) so clear_busy() can clear busy directly on it
        // afterward, without a second lookup.
        std::vector<std::pair<std::shared_ptr<ManagedWard>, CommandExecutor*>> pending_rtl;
        // Same deferred-unlocked-enqueue shape as pending_rtl, but for a
        // fleet mission assignment's auto-start (see
        // ManagedWard::pending_auto_start_mission_id): populated below when
        // a ward's own just-drained upload result matches the mission_id it
        // was armed with.
        std::vector<std::pair<std::shared_ptr<ManagedWard>, CommandExecutor*>> pending_start;

        std::vector<std::shared_ptr<ManagedWard>> wards;
        {
            // Structural lock only, held briefly: copies every ward's
            // shared_ptr, which is what keeps each one alive for the rest of
            // this tick independent of a concurrent remove_ward() erasing it
            // from the map mid-tick. TelemetryInfo's/WardMission's getters
            // below are cached reads (not MAVSDK round-trips) that guard
            // their own internal state already, so none of this needs
            // wards_mutex_ held any longer than this copy.
            std::lock_guard lock(wards_mutex_);
            wards.reserve(managed_wards_.size());
            for (const auto& [id, ward] : managed_wards_) {
                wards.push_back(ward);
            }
        }
        frames.reserve(wards.size());
        for (const auto& ward : wards) {
            const std::string& id = ward->config.ward_id;
            auto* state_envelope = google::protobuf::Arena::Create<karshipta::v1::Envelope>(&arena);
            *state_envelope->mutable_ward_state() =
                build_ward_state(id, *ward->connection, *ward->telemetry);
            frames.push_back(serialize_envelope(*state_envelope));

            if (!ward->mission) continue;  // always built by add_ward_impl; defensive only

            // Only once something has actually been uploaded (get_progress()'s
            // own documented default), so wards with no mission don't
            // spam empty progress frames every tick.
            const auto progress = ward->mission->get_progress();
            if (!progress.mission_id().empty()) {
                auto* progress_envelope =
                    google::protobuf::Arena::Create<karshipta::v1::Envelope>(&arena);
                *progress_envelope->mutable_mission_progress() = progress;
                frames.push_back(serialize_envelope(*progress_envelope));
            }

            if (ward->mission->take_pending_return_to_launch()) {
                // Only the busy check-and-set below needs this ward's own
                // mutex_; everything else touched on this ward in this loop
                // is a cached, self-synchronized read that needs no lock
                // (see ManagedWard's header comment).
                std::lock_guard vlock(ward->mutex_);
                if (ward->executor != nullptr && !ward->busy) {
                    ward->busy = true;
                    pending_rtl.emplace_back(ward, ward->executor.get());
                } else {
                    // Narrow race (ward mid-transition exactly when its
                    // final pass completed): the flag is already consumed
                    // by take_pending_return_to_launch() and cannot be
                    // retried. Logged so it's at least observable.
                    spdlog::warn(
                        "ward '{}' mission finished pending return-to-launch, but the "
                        "ward is busy or stopped; return-to-launch not sent this cycle",
                        id);
                }
            }

            if (const auto upload_result = ward->mission->take_upload_result()) {
                // A fleet mission assignment arms pending_auto_start_mission_id
                // before enqueuing its upload; consume it here regardless of
                // outcome so a failed upload never leaves a stale armed
                // mission_id behind for some later, unrelated upload to
                // accidentally match. pending_auto_start_mission_id and the
                // busy check-and-set below both live under ward->mutex_, so
                // both are folded into one locked scope rather than two
                // separate acquisitions.
                bool auto_start = false;
                {
                    std::lock_guard vlock(ward->mutex_);
                    auto_start = ward->pending_auto_start_mission_id &&
                                 *ward->pending_auto_start_mission_id == upload_result->mission_id;
                    if (auto_start) ward->pending_auto_start_mission_id.reset();

                    if (upload_result->result == mavsdk::Mission::Result::Success && auto_start) {
                        if (ward->executor != nullptr && !ward->busy) {
                            ward->busy = true;
                            pending_start.emplace_back(ward, ward->executor.get());
                        } else {
                            // Narrow race (ward stopped/mid-transition in the
                            // instant between the upload landing and this
                            // tick): logged so it's at least observable,
                            // matching the pending_rtl race note just above.
                            spdlog::warn(
                                "ward '{}' fleet mission assignment uploaded but the ward is "
                                "busy or stopped; mission not started this cycle",
                                id);
                        }
                    }
                }
                if (upload_result->result != mavsdk::Mission::Result::Success) {
                    deferred_mission_broadcasts.push_back(
                        {id, std::nullopt, "MISSION_UPLOAD_REJECTED",
                         WardMission::result_name(upload_result->result)});
                }
                // Every upload result, not just fleet-mission-armed ones -
                // mission_upload_outcome_observer_ is keyed by mission_id, so
                // an observer with no interest in this one simply ignores it.
                // Safe to call from this thread: see the setter's own doc
                // comment (set-once before start_publishing()).
                if (mission_upload_outcome_observer_) {
                    const bool success = upload_result->result == mavsdk::Mission::Result::Success;
                    mission_upload_outcome_observer_(
                        id, upload_result->mission_id, success,
                        success ? std::string() : WardMission::result_name(upload_result->result));
                }
            }

            if (auto download_result = ward->mission->take_download_result()) {
                if (download_result->mission) {
                    deferred_mission_broadcasts.push_back(
                        {id, std::move(download_result->mission), "", ""});
                } else {
                    deferred_mission_broadcasts.push_back(
                        {id, std::nullopt, "MISSION_DOWNLOAD_REJECTED", download_result->message});
                }
            }
        }
        for (const auto& frame : frames) {
            transport_.broadcast(frame);
        }
        for (auto& [ward, executor] : pending_rtl) {
            karshipta::v1::Command rtl;
            rtl.set_command_id("gateway-mission-rtl-" + ward->config.ward_id + "-" +
                               std::to_string(unix_epoch_ms()));
            rtl.set_ward_id(ward->config.ward_id);
            rtl.set_timestamp_ms(unix_epoch_ms());
            rtl.mutable_rtl();
            executor->enqueue(rtl);
            clear_busy(ward);
        }
        for (auto& [ward, executor] : pending_start) {
            karshipta::v1::Command start;
            start.set_command_id("gateway-fleet-mission-start-" + ward->config.ward_id + "-" +
                                 std::to_string(unix_epoch_ms()));
            start.set_ward_id(ward->config.ward_id);
            start.set_timestamp_ms(unix_epoch_ms());
            start.mutable_start_mission();
            executor->enqueue(start);
            clear_busy(ward);
        }
        for (auto& broadcast : deferred_mission_broadcasts) {
            if (broadcast.mission) {
                broadcast_mission_download(*broadcast.mission);
            } else {
                broadcast_mission_event(broadcast.ward_id, broadcast.code, broadcast.message);
            }
        }
        // steady_clock deadline, not sleep_for(interval): sleeping for a
        // fixed interval after doing this tick's work means the actual
        // period is interval + tick_work_time, which grows silently as the
        // fleet grows. A deadline anchored before the tick's own work keeps
        // the average rate correct; if a tick still overran (a stall, not
        // just steady growth), skip ahead to the next real deadline rather
        // than firing a burst of zero-sleep ticks to catch up on whatever
        // was missed.
        const auto now = std::chrono::steady_clock::now();
        if (now < next_tick) {
            std::this_thread::sleep_until(next_tick);
            next_tick += interval;
        } else {
            next_tick = now + interval;
        }
    }
}

void WardManager::send_ward_info(Transport::ClientId client) const {
    struct Snapshot {
        std::string ward_id;
        std::shared_ptr<mavsdk::System> system;
        karshipta::v1::WardClass ward_class;
    };
    // Snapshot cheap values under the structural lock, then release it
    // before the per-ward work below: query_firmware_version() is a
    // blocking MAVSDK round-trip, not cached state, and transport_.send()
    // is a blocking socket write - neither may run while any lock is held,
    // or every other WardManager call stalls behind one client connect.
    // connection->get_system() guards its own internal state (see
    // WardConnection), so no ward->mutex_ is needed here at all: only
    // reconnect_worker/executor/busy/pending_auto_start_mission_id live
    // behind that.
    std::vector<Snapshot> snapshots;
    {
        std::lock_guard lock(wards_mutex_);
        snapshots.reserve(managed_wards_.size());
        for (const auto& [id, ward] : managed_wards_) {
            snapshots.push_back({id, ward->connection->get_system(), ward->config.ward_class});
        }
    }
    for (const auto& snapshot : snapshots) {
        // A ward with no discovered system yet (still reconnecting) still
        // belongs to the fleet, so it gets a WardInfo too, with
        // mavlink_system_id left at 0 - already documented in
        // telemetry.proto as "not connected via MAVLink" - rather than
        // being omitted entirely.
        karshipta::v1::WardInfo info;
        info.set_ward_id(snapshot.ward_id);
        info.set_ward_class(snapshot.ward_class);
        if (snapshot.system) {
            info.set_autopilot(kAutopilotName);
            info.set_mavlink_system_id(snapshot.system->get_system_id());
            info.set_firmware_version(query_firmware_version(snapshot.system));
        }

        karshipta::v1::Envelope envelope;
        *envelope.mutable_ward_info() = info;
        transport_.send(client, serialize_envelope(envelope));
    }
}

bool WardManager::disarm_if_armed(const std::string& ward_id, ManagedWard& ward) {
    if (!ward.telemetry->is_armed()) {
        return true;
    }
    const mavsdk::Action::Result result = ward.actions->disarm();
    if (result == mavsdk::Action::Result::Success) {
        spdlog::info("ward_id '{}' disarmed", ward_id);
        return true;
    }
    spdlog::warn("disarm failed for ward_id '{}': {}", ward_id, WardActions::result_name(result));
    return false;
}

void WardManager::stop_worker(ManagedWard& ward) {
    if (!ward.reconnect_worker.joinable()) {
        return;
    }
    // Joining under ward.mutex_ is deliberate: other threads read joinable()
    // on this same ward under that same mutex, so the join must not race
    // them (std::jthread::joinable()/join() are not safe to call
    // concurrently on the same object from different threads). Worst case
    // this blocks ~3s (a discovery attempt is not stop-token-interruptible)
    // - but now only for THIS ward's own mutex_, not the fleet-wide
    // wards_mutex_, so no other ward's calls are affected.
    ward.reconnect_worker.request_stop();
    ward.reconnect_worker.join();
}

void WardManager::clear_busy(const std::shared_ptr<ManagedWard>& ward) {
    std::lock_guard lock(ward->mutex_);
    ward->busy = false;
}

std::optional<std::string> WardManager::verify_grounded_and_disarm(const std::string& ward_id,
                                                                   ManagedWard& ward) {
    // Unlocked: is_in_air()/is_armed()/disarm() are blocking MAVSDK calls.
    // Safe because ward.busy == true, set by the caller before its own
    // executor-retiring unlock window began.
    //
    // link_state() is checked explicitly here, not inferred from
    // is_in_air()/is_armed() returning false: TelemetryInfo only defaults to
    // false before the plugin is ever created (never connected). Once
    // created, it keeps returning MAVSDK's last-cached value even after the
    // link drops, which could be stale-true for a ward that was armed or
    // airborne right before the drop. Every ground-safety guard in this class
    // must gate on link_state() itself, never trust is_in_air()/is_armed()
    // alone to mean "unavailable therefore safe."
    const bool link_ok = ward.connection->link_state() != WardConnection::LinkState::kLinkDown;
    const bool grounded = link_ok && !ward.telemetry->is_in_air();
    const bool disarmed = grounded && disarm_if_armed(ward_id, ward);
    if (link_ok && grounded && disarmed) {
        return std::nullopt;
    }

    std::lock_guard lock(ward.mutex_);
    ward.executor = make_executor(ward);
    if (!link_ok) return "link dropped during transition";
    if (!grounded) return "ward took off during transition";
    return "failed to disarm";
}

bool WardManager::stop(const std::string& ward_id) {
    auto ward = find_shared_locked(ward_id);
    if (ward == nullptr) {
        spdlog::warn("stop rejected: unknown ward_id '{}'", ward_id);
        return false;
    }
    std::unique_ptr<CommandExecutor> retired_executor;
    {
        std::lock_guard lock(ward->mutex_);
        if (ward->busy) {
            spdlog::warn("stop rejected: ward_id '{}' is mid-transition", ward_id);
            return false;
        }
        // Before any side effect: a stop() that rejects must not have already
        // disarmed the ward.
        if (!ward->reconnect_worker.joinable()) {
            spdlog::warn("stop rejected: ward_id '{}' not running", ward_id);
            return false;
        }
        // Link down after discovery means telemetry cannot be trusted (see
        // verify_grounded_and_disarm()'s comment on why is_in_air()/
        // is_armed() alone can't stand in for this check). Never-discovered
        // passes; nothing we ever saw can be airborne because of us.
        if (ward->connection->link_state() == WardConnection::LinkState::kLinkDown) {
            spdlog::warn(
                "stop rejected: ward_id '{}' link is down, state unknown; "
                "use force_stop",
                ward_id);
            return false;
        }
        if (ward->telemetry->is_in_air()) {
            spdlog::warn("stop rejected: ward_id '{}' is in the air", ward_id);
            return false;
        }
        ward->busy = true;
        retired_executor = std::move(ward->executor);
    }
    // busy is set: no other transition on this ward can start, and
    // remove_ward cannot erase this entry, so `ward` (kept alive by our own
    // shared_ptr copy regardless) stays usable across the unlocked phases
    // below.
    ScopeExit busy_guard{[this, &ward] { clear_busy(ward); }};

    // Unlocked: joins the executor worker (may be mid-MAVSDK-call for
    // seconds) and broadcasts rejection acks for whatever was still queued.
    retired_executor.reset();

    // Re-check now that no command path exists that could have armed or
    // launched the ward between the guard above and the quiesce just now.
    if (const auto error = verify_grounded_and_disarm(ward_id, *ward)) {
        spdlog::warn("stop rejected: ward_id '{}' {}", ward_id, *error);
        return false;
    }

    std::lock_guard lock(ward->mutex_);
    stop_worker(*ward);
    spdlog::info("ward_id '{}' stopped", ward_id);
    return true;
}

void WardManager::stop_all() {
    for (const auto& ward_id : list_ward_ids()) {
        stop(ward_id);
    }
}

bool WardManager::force_stop(const std::string& ward_id) {
    auto ward = find_shared_locked(ward_id);
    if (ward == nullptr) {
        spdlog::warn("force_stop rejected: unknown ward_id '{}'", ward_id);
        return false;
    }
    std::unique_ptr<CommandExecutor> retired_executor;
    {
        std::lock_guard lock(ward->mutex_);
        if (ward->busy) {
            spdlog::warn("force_stop rejected: ward_id '{}' is mid-transition", ward_id);
            return false;
        }
        ward->busy = true;
        retired_executor = std::move(ward->executor);
    }
    ScopeExit busy_guard{[this, &ward] { clear_busy(ward); }};

    retired_executor.reset();  // unlocked; see stop()

    const auto link = ward->connection->link_state();
    const bool airborne = ward->telemetry->is_in_air();
    if (airborne || link == WardConnection::LinkState::kLinkDown) {
        // Direct call is safe: the executor is retired, so this is the only
        // command path to the ward. Best-effort by design; if the link is
        // down the autopilot may still be flying its own failsafe RTL.
        const mavsdk::Action::Result result = ward->actions->return_to_launch();
        if (result == mavsdk::Action::Result::Success) {
            spdlog::warn("force_stop: rtl commanded for ward_id '{}'", ward_id);
        } else {
            spdlog::warn("force_stop: rtl failed for ward_id '{}': {}", ward_id,
                         WardActions::result_name(result));
        }

        // Supervise the flight home instead of abandoning it: the reconnect
        // worker keeps running, and only a ward that is provably connected,
        // landed, and disarmed lets us proceed. `connected` is required in
        // the condition below specifically because is_in_air()/is_armed()
        // do NOT reliably read false on link-down once the plugin has been
        // created once (see verify_grounded_and_disarm()); without the
        // explicit connected check, a link drop mid-RTL could be misread as
        // "landed and disarmed" purely because the cached values happened to
        // predate the ward taking off, silently reintroducing an
        // unsupervised flying ward.
        const auto deadline = std::chrono::steady_clock::now() + kForceStopLandingTimeout;
        bool safe_on_ground = false;
        while (std::chrono::steady_clock::now() < deadline) {
            const bool connected =
                ward->connection->link_state() == WardConnection::LinkState::kConnected;
            if (connected && !ward->telemetry->is_in_air() && !ward->telemetry->is_armed()) {
                safe_on_ground = true;
                break;
            }
            std::this_thread::sleep_for(kReconnectPollInterval);
        }
        if (!safe_on_ground) {
            std::lock_guard lock(ward->mutex_);
            ward->executor = make_executor(*ward);
            spdlog::error(
                "force_stop timed out for ward_id '{}': not confirmed landed and "
                "disarmed within {}s; monitoring stays active",
                ward_id, kForceStopLandingTimeout.count());
            return false;
        }
    }

    // Grounded (or confirmed landed): disarm is best-effort here, unlike
    // stop(); taking the ward offline is the whole point of force_stop.
    (void)disarm_if_armed(ward_id, *ward);

    std::lock_guard lock(ward->mutex_);
    stop_worker(*ward);
    spdlog::warn("ward_id '{}' force-stopped", ward_id);
    return true;
}

void WardManager::force_stop_all() {
    for (const auto& ward_id : list_ward_ids()) {
        force_stop(ward_id);
    }
}

bool WardManager::is_started(const std::string& ward_id) const {
    auto ward = find_shared_locked(ward_id);
    if (ward == nullptr) {
        return false;
    }
    std::lock_guard lock(ward->mutex_);
    return ward->reconnect_worker.joinable();
}

bool WardManager::is_connected(const std::string& ward_id) const {
    // connection->is_connected() guards its own internal state (see
    // WardConnection), so no ward->mutex_ is needed here at all: only
    // reconnect_worker/executor/busy/pending_auto_start_mission_id live
    // behind that.
    auto ward = find_shared_locked(ward_id);
    return ward != nullptr && ward->connection->is_connected();
}

bool WardManager::is_in_air(const std::string& ward_id) const {
    // Same reasoning as is_connected() above: telemetry->is_in_air() guards
    // its own internal state, not one of the fields behind ward->mutex_.
    auto ward = find_shared_locked(ward_id);
    return ward != nullptr && ward->telemetry->is_in_air();
}

std::vector<WardStatus> WardManager::list_status() const {
    std::vector<std::shared_ptr<ManagedWard>> wards;
    {
        std::lock_guard lock(wards_mutex_);
        wards.reserve(managed_wards_.size());
        for (const auto& [id, ward] : managed_wards_) {
            wards.push_back(ward);
        }
    }
    std::vector<WardStatus> statuses;
    statuses.reserve(wards.size());
    for (const auto& ward : wards) {
        bool started = false;
        {
            std::lock_guard lock(ward->mutex_);
            started = ward->reconnect_worker.joinable();
        }
        statuses.push_back(WardStatus{
            .ward_id = ward->config.ward_id,
            .started = started,
            .connected = ward->connection->is_connected(),
        });
    }
    return statuses;
}

std::optional<std::string> WardManager::remove_ward_impl(const std::string& ward_id) {
    auto ward = find_shared_locked(ward_id);
    if (ward == nullptr) {
        return "unknown ward_id: " + ward_id;
    }
    std::unique_ptr<CommandExecutor> retired_executor;
    {
        std::lock_guard lock(ward->mutex_);
        if (ward->busy) {
            return "ward is mid-transition";
        }
        // Same trust rule as stop() (see verify_grounded_and_disarm()):
        // erasing the connection to a possibly-flying ward removes the
        // only way to ever command it again.
        if (ward->connection->link_state() == WardConnection::LinkState::kLinkDown) {
            return "link is down, ward state unknown";
        }
        if (ward->telemetry->is_in_air()) {
            return "ward is in the air";
        }
        ward->busy = true;
        retired_executor = std::move(ward->executor);
    }
    ScopeExit busy_guard{[this, &ward] { clear_busy(ward); }};

    retired_executor.reset();  // unlocked; see stop()

    if (const auto error = verify_grounded_and_disarm(ward_id, *ward)) {
        return *error;
    }

    // stop_worker() only needs this ward's own mutex_, never wards_mutex_
    // (see stop_worker()'s own doc comment: its reconnect_worker join is
    // worst-case ~3s, and must not stall the fleet-wide structural lock
    // every other ward's dispatch_command/publish-tick/add-or-remove also
    // needs). Kept in its own unlocked-from-wards_mutex_ scope, the same
    // shape stop() itself already uses; ward->busy stayed true since it was
    // set above, so no concurrent transition on this same ward can start
    // regardless of which lock is or isn't held here.
    {
        std::lock_guard vlock(ward->mutex_);
        stop_worker(*ward);
    }

    // Structural lock, now covering only the fast, bounded part: the map
    // shape change and the persisted-state write.
    //
    // A plain erase() here is correct, not a regression of an earlier fix
    // that used std::map::extract() instead: that version's map held
    // ManagedWard BY VALUE, so erase() would destroy the object (running
    // WardConnection::~WardConnection() -> mavsdk's remove_connection(),
    // TelemetryInfo::~TelemetryInfo()'s unsubscribe, WardMission::
    // ~WardMission()'s own worker join, all unbounded) right here, under
    // the lock, stalling the whole fleet. Now the map holds
    // shared_ptr<ManagedWard>: erase() only drops the map's reference. The
    // local `ward` variable above still holds its own shared_ptr copy, so
    // the object is not destroyed by this erase() call - it is destroyed
    // later, when `ward` goes out of scope at the end of this function,
    // which happens after every lock here has already released.
    {
        std::lock_guard lock(wards_mutex_);
        managed_wards_.erase(ward_id);
        persist_locked();
        spdlog::info("ward_id '{}' removed (total={})", ward_id, managed_wards_.size());
    }
    return std::nullopt;
}

bool WardManager::remove_ward(const std::string& ward_id) {
    const auto error = remove_ward_impl(ward_id);
    if (error) {
        spdlog::warn("remove_ward rejected: ward_id '{}': {}", ward_id, *error);
        return false;
    }
    return true;
}

void WardManager::remove_all() {
    for (const auto& ward_id : list_ward_ids()) {
        remove_ward(ward_id);
    }
}

karshipta::v1::WardConfigAck WardManager::handle_add_ward(const karshipta::v1::AddWard& request) {
    karshipta::v1::WardConfigAck ack;
    ack.set_request_id(request.request_id());
    ack.set_ward_id(request.ward_id());

    const WardConfig cfg{
        .ward_id = request.ward_id(),
        .connection_url = request.connection_url(),
        .system_id = request.mavlink_system_id(),
        .ward_class = request.ward_class(),
        .name = request.name(),
    };

    std::optional<std::string> error;
    try {
        error = add_ward_impl(cfg);
    } catch (const std::invalid_argument& bad_url) {
        // WardConnection rejects an empty/invalid URL by throwing; a bad
        // wire request must become a REJECTED ack, not a gateway crash.
        error = bad_url.what();
    }
    if (!error && !start(cfg.ward_id)) {
        error = "ward registered but failed to start";
        // Roll back the registration: leaving a registered-but-never-started
        // ward behind would strand it permanently, since add_ward_impl
        // rejects a retry with the same ward_id as "already registered."
        // Best-effort and results ignored: a concurrent remove racing in
        // during this window is exactly why start() could have failed on a
        // freshly-added ward in the first place, and in that case this
        // call is a harmless no-op (already gone).
        (void)remove_ward_impl(cfg.ward_id);
    }

    if (error) {
        spdlog::warn("add_ward request '{}' rejected: {}", request.request_id(), *error);
        ack.set_status(karshipta::v1::WARD_CONFIG_STATUS_REJECTED);
        ack.set_message(*error);
    } else {
        ack.set_status(karshipta::v1::WARD_CONFIG_STATUS_ACCEPTED);
        ack.set_message("connection attempt underway");
        broadcast_fleet_event(cfg.ward_id, /*added=*/true);
    }
    return ack;
}

karshipta::v1::WardConfigAck WardManager::handle_remove_ward(
    const karshipta::v1::RemoveWard& request) {
    karshipta::v1::WardConfigAck ack;
    ack.set_request_id(request.request_id());
    ack.set_ward_id(request.ward_id());

    const auto error = remove_ward_impl(request.ward_id());
    if (error) {
        spdlog::warn("remove_ward request '{}' rejected: {}", request.request_id(), *error);
        ack.set_status(karshipta::v1::WARD_CONFIG_STATUS_REJECTED);
        ack.set_message(*error);
    } else {
        ack.set_status(karshipta::v1::WARD_CONFIG_STATUS_ACCEPTED);
        ack.set_message("ward removed");
        broadcast_fleet_event(request.ward_id(), /*added=*/false);
    }
    return ack;
}

void WardManager::run_reconnect_loop(ManagedWard& ward, std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        if (!ward.connection->connect_with_retry(stop_token)) {
            // Only false if stop_token was cancelled during the retry itself.
            break;
        }
        spdlog::info("ward connected (system_id={})", ward.config.system_id);
        broadcast_link_event(ward.config.ward_id, /*connected=*/true);
        // PX4 forgets requested stream rates across a link drop, so this is
        // re-requested on every reconnect, not just the first.
        ward.telemetry->set_telemetry_rate(kTelemetryRateHz);

        while (ward.connection->is_connected() && !stop_token.stop_requested()) {
            std::this_thread::sleep_for(kReconnectPollInterval);
        }

        if (stop_token.stop_requested()) {
            break;
        }
        spdlog::warn("ward link lost (system_id={}), reconnecting", ward.config.system_id);
        broadcast_link_event(ward.config.ward_id, /*connected=*/false);
    }
}
