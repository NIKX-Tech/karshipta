//
// Created by amir abkhoshk on 13/07/2026.
//

#include "ward_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <mavsdk/plugins/info/info.h>
#include <yaml-cpp/yaml.h>

#include <karshipta/v1/envelope.pb.h>
#include <spdlog/spdlog.h>

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
    flight->set_flight_mode(to_proto_flight_mode(telemetry.get_flight_mode()));
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

}  // namespace

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
    // kForceStopLandingTimeoutS with wards_mutex_ released, holding a raw
    // ManagedWard*. Waiting here for every busy flag to clear guarantees
    // managed_wards_ never gets torn down out from under one of those
    // unlocked windows.
    std::unique_lock lock(wards_mutex_);
    busy_cv_.wait(lock, [this] {
        return std::none_of(managed_wards_.begin(), managed_wards_.end(),
                             [](const auto& entry) { return entry.second.busy; });
    });
}

ManagedWard* WardManager::find_locked(const std::string& ward_id) {
    const auto it = managed_wards_.find(ward_id);
    return it == managed_wards_.end() ? nullptr : &it->second;
}

const ManagedWard* WardManager::find_locked(const std::string& ward_id) const {
    const auto it = managed_wards_.find(ward_id);
    return it == managed_wards_.end() ? nullptr : &it->second;
}

std::unique_ptr<CommandExecutor> WardManager::make_executor(ManagedWard& ward) {
    return std::make_unique<CommandExecutor>(
        *ward.actions, *ward.telemetry, *ward.mission,
        [this](const karshipta::v1::CommandAck& ack) {
            broadcast_command_ack(ack);
            // rejected commands are events a human should see (gateway rule 5)
            if (ack.status() == karshipta::v1::COMMAND_STATUS_REJECTED) {
                broadcast_rejection_event(ack);
            }
        });
}

std::optional<std::string> WardManager::add_ward_impl(const WardConfig& cfg,
                                                              bool should_persist) {
    if (cfg.ward_id.empty()) {
        return "ward_id is empty";
    }

    std::lock_guard lock(wards_mutex_);
    if (managed_wards_.contains(cfg.ward_id)) {
        return "ward_id '" + cfg.ward_id + "' already registered";
    }
    // system_id 0 means "first autopilot" (fleet.proto), so zeros are not
    // identities and must not collide with each other.
    if (cfg.system_id != 0) {
        for (const auto& [id, ward] : managed_wards_) {
            if (ward.config.system_id == cfg.system_id) {
                return "system_id " + std::to_string(cfg.system_id) +
                       " already bound to ward_id '" + id + "'";
            }
        }
    }

    spdlog::debug("add_ward: building object graph for '{}' (system_id={}, url={})",
                  cfg.ward_id, cfg.system_id, cfg.connection_url);

    // Each object below binds to the previous one by reference, so they must be
    // built in this order: connection first, then telemetry/actions/mission off
    // of it, then the executor off of those. None of this connects to the
    // ward yet (WardConnection's constructor only stores state); that
    // happens when start() launches the reconnect worker.
    const std::optional<uint32_t> expected_system_id =
        cfg.system_id == 0 ? std::nullopt : std::optional<uint32_t>{cfg.system_id};
    auto connection =
        std::make_unique<WardConnection>(mavsdk_, cfg.connection_url, expected_system_id);
    auto telemetry = std::make_unique<TelemetryInfo>(*connection);
    auto actions = std::make_unique<WardActions>(*connection);
    auto mission = std::make_unique<WardMission>(*connection);
    auto mission_importer = std::make_unique<MissionImporter>(*connection);

    ManagedWard managed{
        .config = cfg,
        .connection = std::move(connection),
        .telemetry = std::move(telemetry),
        .actions = std::move(actions),
        .mission = std::move(mission),
        .mission_importer = std::move(mission_importer),
        // Explicit even though these match ManagedWard's own default
        // member initializers (nullptr, false, not-joinable): gcc/clang's
        // -Wmissing-field-initializers (part of -Wextra) flags a designated
        // initializer that doesn't reach the struct's last member, even when
        // the skipped ones have in-class defaults. executor is filled in
        // below since it needs a reference to `managed` itself.
        .executor = nullptr,
        .busy = false,
        .reconnect_worker = {},
    };
    managed.executor = make_executor(managed);

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

void WardManager::dispatch_command(const karshipta::v1::Command& command) {
    // The rejection ack is built under the lock but broadcast after releasing
    // it: Transport::broadcast is a blocking socket write and must not stall
    // every other manager call behind a slow client. The accept path below
    // gets the same treatment: enqueue() synchronously broadcasts its own
    // ACCEPTED ack, so it must not run while wards_mutex_ is held either.
    std::optional<karshipta::v1::CommandAck> rejection;
    CommandExecutor* executor = nullptr;
    {
        std::lock_guard lock(wards_mutex_);
        auto* ward = find_locked(command.ward_id());
        if (ward == nullptr) {
            spdlog::warn("dispatch_command rejected: unknown ward_id '{}'",
                         command.ward_id());
            rejection = make_rejected_ack(command, "unknown ward_id: " + command.ward_id());
        } else if (ward->executor == nullptr) {
            spdlog::warn("dispatch_command rejected: ward_id '{}' is stopped",
                         command.ward_id());
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
    clear_busy_and_notify(command.ward_id());
}

void WardManager::handle_mission_upload(const karshipta::v1::Mission& mission) {
    std::optional<std::string> rejection_reason;
    WardMission* target = nullptr;
    {
        std::lock_guard lock(wards_mutex_);
        auto* ward = find_locked(mission.ward_id());
        if (ward == nullptr) {
            rejection_reason = "unknown ward_id: " + mission.ward_id();
        } else if (ward->executor == nullptr) {
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
    clear_busy_and_notify(mission.ward_id());
}

void WardManager::handle_mission_file_upload(const karshipta::v1::MissionFileUpload& upload) {
    std::optional<std::string> rejection_reason;
    MissionImporter* importer = nullptr;
    WardMission* target = nullptr;
    {
        std::lock_guard lock(wards_mutex_);
        auto* ward = find_locked(upload.ward_id());
        if (ward == nullptr) {
            rejection_reason = "unknown ward_id: " + upload.ward_id();
        } else if (ward->executor == nullptr) {
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
        clear_busy_and_notify(upload.ward_id());
        return;
    }
    target->enqueue_upload(std::move(*mission));
    clear_busy_and_notify(upload.ward_id());
}

void WardManager::handle_mission_download_request(
    const karshipta::v1::MissionDownloadRequest& request) {
    std::optional<std::string> rejection_reason;
    WardMission* target = nullptr;
    {
        std::lock_guard lock(wards_mutex_);
        auto* ward = find_locked(request.ward_id());
        if (ward == nullptr) {
            rejection_reason = "unknown ward_id: " + request.ward_id();
        } else if (ward->executor == nullptr) {
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
    clear_busy_and_notify(request.ward_id());
}

void WardManager::broadcast_command_ack(const karshipta::v1::CommandAck& ack) const {
    karshipta::v1::Envelope envelope;
    *envelope.mutable_command_ack() = ack;
    transport_.broadcast(serialize_envelope(envelope));
}

void WardManager::broadcast_rejection_event(const karshipta::v1::CommandAck& ack) const {
    karshipta::v1::Envelope envelope;
    auto* event = envelope.mutable_event();
    event->set_ward_id(ack.ward_id());
    event->set_timestamp_ms(unix_epoch_ms());
    event->set_severity(karshipta::v1::SEVERITY_WARNING);
    event->set_code("COMMAND_REJECTED");
    event->set_message(ack.message());
    transport_.broadcast(serialize_envelope(envelope));
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
    std::lock_guard lock(wards_mutex_);
    auto* ward = find_locked(ward_id);
    if (ward == nullptr) {
        spdlog::warn("start rejected: unknown ward_id '{}'", ward_id);
        return false;
    }
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
    ward->reconnect_worker = std::jthread(
        [this, ward](std::stop_token stop_token) { run_reconnect_loop(*ward, stop_token); });
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
        entry["ward_id"] = ward.config.ward_id;
        entry["connection_url"] = ward.config.connection_url;
        entry["mavlink_system_id"] = ward.config.system_id;
        entry["name"] = ward.config.name;
        entry["ward_class"] = karshipta::v1::WardClass_Name(ward.config.ward_class);
        wards.push_back(entry);
    }
    root["wards"] = wards;

    // gateway/config/ is a tracked directory (see .gitkeep); this is internal
    // machinery, not an operator-managed path, so no runtime mkdir here.
    std::ofstream out(persistence_path_, std::ios::trunc);
    if (!out) {
        spdlog::error("failed to open '{}' for writing persisted fleet state",
                      persistence_path_.string());
        return;
    }
    out << root;
    if (!out) {
        spdlog::error("failed to write persisted fleet state to '{}'", persistence_path_.string());
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
                spdlog::warn("persisted ward '{}' has unknown class '{}', defaulting to unspecified",
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
    while (!stop_token.stop_requested()) {
        std::vector<std::vector<uint8_t>> frames;
        // Deferred so broadcast_mission_event()/broadcast_mission_download()
        // (blocking socket writes) never run while wards_mutex_ is held,
        // same rule as everywhere else in this file. mission set means
        // "broadcast this download"; otherwise code/message carry an event.
        struct DeferredMissionBroadcast {
            std::string ward_id;
            std::optional<karshipta::v1::Mission> mission;
            std::string code;
            std::string message;
        };
        std::vector<DeferredMissionBroadcast> deferred_mission_broadcasts;
        // ward_id + executor to enqueue a synthetic RTL command on, once
        // unlocked; mirrors dispatch_command's busy-then-unlocked-enqueue
        // pattern so a concurrent remove_ward() can't destroy the executor
        // out from under this deferred call.
        std::vector<std::pair<std::string, CommandExecutor*>> pending_rtl;
        {
            // Cheap in-memory work only (TelemetryInfo's/WardMission's
            // getters are cached reads, not MAVSDK round-trips): safe to
            // build every frame while holding the lock. The broadcast itself
            // is not, so it happens after releasing it below.
            std::lock_guard lock(wards_mutex_);
            frames.reserve(managed_wards_.size());
            for (auto& [id, ward] : managed_wards_) {
                karshipta::v1::Envelope state_envelope;
                *state_envelope.mutable_ward_state() =
                    build_ward_state(id, *ward.connection, *ward.telemetry);
                frames.push_back(serialize_envelope(state_envelope));

                if (!ward.mission) continue;  // always built by add_ward_impl; defensive only

                // Only once something has actually been uploaded (get_progress()'s
                // own documented default), so wards with no mission don't
                // spam empty progress frames every tick.
                const auto progress = ward.mission->get_progress();
                if (!progress.mission_id().empty()) {
                    karshipta::v1::Envelope progress_envelope;
                    *progress_envelope.mutable_mission_progress() = progress;
                    frames.push_back(serialize_envelope(progress_envelope));
                }

                if (ward.mission->take_pending_return_to_launch()) {
                    if (ward.executor != nullptr && !ward.busy) {
                        ward.busy = true;
                        pending_rtl.emplace_back(id, ward.executor.get());
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

                if (const auto upload_result = ward.mission->take_upload_result()) {
                    if (upload_result->result != mavsdk::Mission::Result::Success) {
                        deferred_mission_broadcasts.push_back(
                            {id, std::nullopt, "MISSION_UPLOAD_REJECTED",
                             WardMission::result_name(upload_result->result)});
                    }
                }

                if (auto download_result = ward.mission->take_download_result()) {
                    if (download_result->mission) {
                        deferred_mission_broadcasts.push_back(
                            {id, std::move(download_result->mission), "", ""});
                    } else {
                        deferred_mission_broadcasts.push_back(
                            {id, std::nullopt, "MISSION_DOWNLOAD_REJECTED", download_result->message});
                    }
                }
            }
        }
        for (const auto& frame : frames) {
            transport_.broadcast(frame);
        }
        for (auto& [ward_id, executor] : pending_rtl) {
            karshipta::v1::Command rtl;
            rtl.set_command_id("gateway-mission-rtl-" + ward_id + "-" +
                                std::to_string(unix_epoch_ms()));
            rtl.set_ward_id(ward_id);
            rtl.set_timestamp_ms(unix_epoch_ms());
            rtl.mutable_rtl();
            executor->enqueue(rtl);
            clear_busy_and_notify(ward_id);
        }
        for (auto& broadcast : deferred_mission_broadcasts) {
            if (broadcast.mission) {
                broadcast_mission_download(*broadcast.mission);
            } else {
                broadcast_mission_event(broadcast.ward_id, broadcast.code, broadcast.message);
            }
        }
        std::this_thread::sleep_for(interval);
    }
}

void WardManager::send_ward_info(Transport::ClientId client) const {
    struct Snapshot {
        std::string ward_id;
        std::shared_ptr<mavsdk::System> system;
        karshipta::v1::WardClass ward_class;
    };
    // Snapshot cheap values under the lock, then release it before the
    // per-ward work below: query_firmware_version() is a blocking MAVSDK
    // round-trip, not cached state, and transport_.send() is a blocking
    // socket write - neither may run while wards_mutex_ is held, or every
    // other WardManager call stalls behind one client connect.
    std::vector<Snapshot> snapshots;
    {
        std::lock_guard lock(wards_mutex_);
        snapshots.reserve(managed_wards_.size());
        for (const auto& [id, ward] : managed_wards_) {
            snapshots.push_back({id, ward.connection->get_system(), ward.config.ward_class});
        }
    }
    for (const auto& snapshot : snapshots) {
        if (!snapshot.system) {
            spdlog::warn(
                "client {} connected but ward '{}' has no discovered system yet, skipping "
                "WardInfo",
                client, snapshot.ward_id);
            continue;
        }
        karshipta::v1::WardInfo info;
        info.set_ward_id(snapshot.ward_id);
        info.set_ward_class(snapshot.ward_class);
        info.set_autopilot(kAutopilotName);
        info.set_mavlink_system_id(snapshot.system->get_system_id());
        info.set_firmware_version(query_firmware_version(snapshot.system));

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
    spdlog::warn("disarm failed for ward_id '{}': {}", ward_id,
                 WardActions::result_name(result));
    return false;
}

void WardManager::stop_worker(ManagedWard& ward) {
    if (!ward.reconnect_worker.joinable()) {
        return;
    }
    // Joining under wards_mutex_ is deliberate: other threads read
    // joinable() under the lock, so the join must not race them. Worst case
    // it blocks ~3s (a discovery attempt is not stop-token-interruptible).
    // Not moved unlocked: std::jthread::joinable()/join() are not safe to
    // call concurrently on the same object from different threads, so an
    // unlocked join here would race is_started()/list_status()'s locked
    // joinable() reads on this exact object. A real fix needs a per-ward
    // mutex (finer than wards_mutex_), which is a larger change than this
    // pass's scope; the ~3s worst case is accepted for now.
    ward.reconnect_worker.request_stop();
    ward.reconnect_worker.join();
}

void WardManager::clear_busy_and_notify(const std::string& ward_id) {
    std::lock_guard lock(wards_mutex_);
    if (auto* v = find_locked(ward_id)) {
        v->busy = false;
    }
    // Unconditional: ~WardManager()'s wait must re-evaluate even when the
    // ward that just finished was erased by this same transition (a
    // successful remove_ward_impl()), not just when a flag actually flips.
    busy_cv_.notify_all();
}

std::optional<std::string> WardManager::verify_grounded_and_disarm(
    const std::string& ward_id, ManagedWard& ward) {
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

    std::lock_guard lock(wards_mutex_);
    ward.executor = make_executor(ward);
    if (!link_ok) return "link dropped during transition";
    if (!grounded) return "ward took off during transition";
    return "failed to disarm";
}

bool WardManager::stop(const std::string& ward_id) {
    ManagedWard* ward = nullptr;
    std::unique_ptr<CommandExecutor> retired_executor;
    {
        std::lock_guard lock(wards_mutex_);
        ward = find_locked(ward_id);
        if (ward == nullptr) {
            spdlog::warn("stop rejected: unknown ward_id '{}'", ward_id);
            return false;
        }
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
            spdlog::warn("stop rejected: ward_id '{}' link is down, state unknown; "
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
    // busy is set: no other transition can start, remove_ward cannot erase
    // this entry, so `ward` stays valid across the unlocked phases below.
    ScopeExit clear_busy{[this, &ward_id] { clear_busy_and_notify(ward_id); }};

    // Unlocked: joins the executor worker (may be mid-MAVSDK-call for
    // seconds) and broadcasts rejection acks for whatever was still queued.
    retired_executor.reset();

    // Re-check now that no command path exists that could have armed or
    // launched the ward between the guard above and the quiesce just now.
    if (const auto error = verify_grounded_and_disarm(ward_id, *ward)) {
        spdlog::warn("stop rejected: ward_id '{}' {}", ward_id, *error);
        return false;
    }

    std::lock_guard lock(wards_mutex_);
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
    ManagedWard* ward = nullptr;
    std::unique_ptr<CommandExecutor> retired_executor;
    {
        std::lock_guard lock(wards_mutex_);
        ward = find_locked(ward_id);
        if (ward == nullptr) {
            spdlog::warn("force_stop rejected: unknown ward_id '{}'", ward_id);
            return false;
        }
        if (ward->busy) {
            spdlog::warn("force_stop rejected: ward_id '{}' is mid-transition", ward_id);
            return false;
        }
        ward->busy = true;
        retired_executor = std::move(ward->executor);
    }
    ScopeExit clear_busy{[this, &ward_id] { clear_busy_and_notify(ward_id); }};

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
            if (connected && !ward->telemetry->is_in_air() &&
                !ward->telemetry->is_armed()) {
                safe_on_ground = true;
                break;
            }
            std::this_thread::sleep_for(kReconnectPollInterval);
        }
        if (!safe_on_ground) {
            std::lock_guard lock(wards_mutex_);
            ward->executor = make_executor(*ward);
            spdlog::error("force_stop timed out for ward_id '{}': not confirmed landed and "
                          "disarmed within {}s; monitoring stays active",
                          ward_id, kForceStopLandingTimeout.count());
            return false;
        }
    }

    // Grounded (or confirmed landed): disarm is best-effort here, unlike
    // stop(); taking the ward offline is the whole point of force_stop.
    (void)disarm_if_armed(ward_id, *ward);

    std::lock_guard lock(wards_mutex_);
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
    std::lock_guard lock(wards_mutex_);
    const auto* ward = find_locked(ward_id);
    return ward != nullptr && ward->reconnect_worker.joinable();
}

bool WardManager::is_connected(const std::string& ward_id) const {
    std::lock_guard lock(wards_mutex_);
    const auto* ward = find_locked(ward_id);
    return ward != nullptr && ward->connection->is_connected();
}

std::vector<WardStatus> WardManager::list_status() const {
    std::lock_guard lock(wards_mutex_);
    std::vector<WardStatus> statuses;
    statuses.reserve(managed_wards_.size());
    for (const auto& [id, ward] : managed_wards_) {
        statuses.push_back(WardStatus{
            .ward_id = id,
            .started = ward.reconnect_worker.joinable(),
            .connected = ward.connection->is_connected(),
        });
    }
    return statuses;
}

std::optional<std::string> WardManager::remove_ward_impl(const std::string& ward_id) {
    ManagedWard* ward = nullptr;
    std::unique_ptr<CommandExecutor> retired_executor;
    {
        std::lock_guard lock(wards_mutex_);
        ward = find_locked(ward_id);
        if (ward == nullptr) {
            return "unknown ward_id: " + ward_id;
        }
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
    ScopeExit clear_busy{[this, &ward_id] { clear_busy_and_notify(ward_id); }};

    retired_executor.reset();  // unlocked; see stop()

    if (const auto error = verify_grounded_and_disarm(ward_id, *ward)) {
        return *error;
    }

    // extract(), not erase(): erase() would destroy the ManagedWard (and
    // therefore run WardConnection::~WardConnection() -> mavsdk's
    // remove_connection(), TelemetryInfo::~TelemetryInfo()'s unsubscribe, and
    // WardMission::~WardMission()'s own worker join) right here, under
    // wards_mutex_. None of those calls are bounded or interruptible, so
    // one slow teardown would stall every other ward's dispatch_command,
    // the publish tick, and add/remove requests behind this lock. extract()
    // only relocates the node out of the map (same out-of-lock-destruction
    // idea as retired_executor above); `node` actually destructs once it goes
    // out of scope below, after the lock has already been released.
    std::map<std::string, ManagedWard>::node_type node;
    {
        std::lock_guard lock(wards_mutex_);
        stop_worker(*ward);
        node = managed_wards_.extract(ward_id);
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

karshipta::v1::WardConfigAck WardManager::handle_add_ward(
    const karshipta::v1::AddWard& request) {
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
