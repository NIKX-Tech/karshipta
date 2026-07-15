//
// Created by amir abkhoshk on 13/07/2026.
//

#include "vehicle_manager.h"

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

// BRIEF.md M2's ~5Hz per-vehicle telemetry target: requested from the
// autopilot on every (re)connect, and matches
// VehicleManager::kDefaultPublishInterval (1000ms / kTelemetryRateHz).
constexpr float kTelemetryRateHz = 5.0f;

// Only autopilot this milestone connects to (SITL); AddVehicle carries no
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

karshipta::v1::VehicleState build_vehicle_state(const std::string& vehicle_id,
                                                 const VehicleConnection& connection,
                                                 const TelemetryInfo& telemetry) {
    karshipta::v1::VehicleState state;
    state.set_vehicle_id(vehicle_id);
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

    state.set_flight_mode(to_proto_flight_mode(telemetry.get_flight_mode()));
    state.set_armed(telemetry.is_armed());
    state.set_in_air(telemetry.is_in_air());
    state.set_health_ok(telemetry.is_health_ok());
    state.set_connected(connection.is_connected());

    return state;
}

// Blocking query against the Info plugin; only called once per client connect
// per vehicle, so a fresh plugin instance per call is cheap and needs no
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

karshipta::v1::CommandAck make_rejected_ack(const karshipta::v1::Command& command,
                                            const std::string& reason) {
    karshipta::v1::CommandAck ack;
    ack.set_command_id(command.command_id());
    ack.set_vehicle_id(command.vehicle_id());
    ack.set_status(karshipta::v1::COMMAND_STATUS_REJECTED);
    ack.set_message(reason);
    return ack;
}

}  // namespace

VehicleManager::VehicleManager(std::shared_ptr<mavsdk::Mavsdk> mavsdk, Transport& tp,
                                std::filesystem::path persistence_path)
    : mavsdk_(std::move(mavsdk)), transport_(tp), persistence_path_(std::move(persistence_path)) {}

std::unique_ptr<VehicleManager> VehicleManager::create(Transport& tp,
                                                         std::filesystem::path persistence_path) {
    return std::make_unique<VehicleManager>(VehicleConnection::create_shared_core(), tp,
                                             std::move(persistence_path));
}

VehicleManager::~VehicleManager() {
    // See the header comment: force_stop() in particular runs for up to
    // kForceStopLandingTimeoutS with vehicles_mutex_ released, holding a raw
    // ManagedVehicle*. Waiting here for every busy flag to clear guarantees
    // managed_vehicles_ never gets torn down out from under one of those
    // unlocked windows.
    std::unique_lock lock(vehicles_mutex_);
    busy_cv_.wait(lock, [this] {
        return std::none_of(managed_vehicles_.begin(), managed_vehicles_.end(),
                             [](const auto& entry) { return entry.second.busy; });
    });
}

ManagedVehicle* VehicleManager::find_locked(const std::string& vehicle_id) {
    const auto it = managed_vehicles_.find(vehicle_id);
    return it == managed_vehicles_.end() ? nullptr : &it->second;
}

const ManagedVehicle* VehicleManager::find_locked(const std::string& vehicle_id) const {
    const auto it = managed_vehicles_.find(vehicle_id);
    return it == managed_vehicles_.end() ? nullptr : &it->second;
}

std::unique_ptr<CommandExecutor> VehicleManager::make_executor(ManagedVehicle& vehicle) {
    return std::make_unique<CommandExecutor>(
        *vehicle.actions, *vehicle.telemetry, [this](const karshipta::v1::CommandAck& ack) {
            broadcast_command_ack(ack);
            // rejected commands are events a human should see (gateway rule 5)
            if (ack.status() == karshipta::v1::COMMAND_STATUS_REJECTED) {
                broadcast_rejection_event(ack);
            }
        });
}

std::optional<std::string> VehicleManager::add_vehicle_impl(const VehicleConfig& cfg,
                                                              bool should_persist) {
    if (cfg.vehicle_id.empty()) {
        return "vehicle_id is empty";
    }

    std::lock_guard lock(vehicles_mutex_);
    if (managed_vehicles_.contains(cfg.vehicle_id)) {
        return "vehicle_id '" + cfg.vehicle_id + "' already registered";
    }
    // system_id 0 means "first autopilot" (fleet.proto), so zeros are not
    // identities and must not collide with each other.
    if (cfg.system_id != 0) {
        for (const auto& [id, vehicle] : managed_vehicles_) {
            if (vehicle.config.system_id == cfg.system_id) {
                return "system_id " + std::to_string(cfg.system_id) +
                       " already bound to vehicle_id '" + id + "'";
            }
        }
    }

    spdlog::debug("add_vehicle: building object graph for '{}' (system_id={}, url={})",
                  cfg.vehicle_id, cfg.system_id, cfg.connection_url);

    // Each object below binds to the previous one by reference, so they must be
    // built in this order: connection first, then telemetry/actions off of it,
    // then the executor off of those. None of this connects to the vehicle yet
    // (VehicleConnection's constructor only stores state); that happens when
    // start() launches the reconnect worker.
    const std::optional<uint32_t> expected_system_id =
        cfg.system_id == 0 ? std::nullopt : std::optional<uint32_t>{cfg.system_id};
    auto connection =
        std::make_unique<VehicleConnection>(mavsdk_, cfg.connection_url, expected_system_id);
    auto telemetry = std::make_unique<TelemetryInfo>(*connection);
    auto actions = std::make_unique<VehicleActions>(*connection);

    ManagedVehicle managed{
        .config = cfg,
        .connection = std::move(connection),
        .telemetry = std::move(telemetry),
        .actions = std::move(actions),
        // executor and busy take ManagedVehicle's own default member
        // initializers (nullptr, false); executor is filled in below since it
        // needs a reference to `managed` itself.
    };
    managed.executor = make_executor(managed);

    managed_vehicles_.emplace(cfg.vehicle_id, std::move(managed));
    spdlog::info("vehicle '{}' registered (system_id={}, total={})", cfg.vehicle_id, cfg.system_id,
                 managed_vehicles_.size());

    if (should_persist) {
        persist_locked();
    }
    return std::nullopt;
}

bool VehicleManager::add_vehicle(const VehicleConfig& cfg) {
    const auto error = add_vehicle_impl(cfg);
    if (error) {
        spdlog::warn("add_vehicle rejected: {}", *error);
        return false;
    }
    return true;
}

std::vector<std::string> VehicleManager::list_vehicle_ids() const {
    std::lock_guard lock(vehicles_mutex_);
    std::vector<std::string> ids;
    ids.reserve(managed_vehicles_.size());
    for (const auto& entry : managed_vehicles_) {
        ids.push_back(entry.first);
    }
    return ids;
}

void VehicleManager::dispatch_command(const karshipta::v1::Command& command) {
    // The rejection ack is built under the lock but broadcast after releasing
    // it: Transport::broadcast is a blocking socket write and must not stall
    // every other manager call behind a slow client. The accept path below
    // gets the same treatment: enqueue() synchronously broadcasts its own
    // ACCEPTED ack, so it must not run while vehicles_mutex_ is held either.
    std::optional<karshipta::v1::CommandAck> rejection;
    CommandExecutor* executor = nullptr;
    {
        std::lock_guard lock(vehicles_mutex_);
        auto* vehicle = find_locked(command.vehicle_id());
        if (vehicle == nullptr) {
            spdlog::warn("dispatch_command rejected: unknown vehicle_id '{}'",
                         command.vehicle_id());
            rejection = make_rejected_ack(command, "unknown vehicle_id: " + command.vehicle_id());
        } else if (vehicle->executor == nullptr) {
            spdlog::warn("dispatch_command rejected: vehicle_id '{}' is stopped",
                         command.vehicle_id());
            rejection = make_rejected_ack(command, "vehicle is stopped");
        } else if (vehicle->busy) {
            // busy here means either a stop/force_stop/remove transition is
            // in flight, or another dispatch_command for this same vehicle
            // is mid-enqueue (see the comment on the executor-capture branch
            // below); either way the command is safe to retry shortly, so
            // the message must not claim the vehicle is stopped when it may
            // simply be a rare double-dispatch collision.
            spdlog::warn("dispatch_command rejected: vehicle_id '{}' is busy, retry shortly",
                         command.vehicle_id());
            rejection = make_rejected_ack(command, "vehicle is busy, retry shortly");
        } else {
            // busy blocks every quiescing path (stop/force_stop/remove) from
            // retiring this executor while it's set, so the raw pointer below
            // stays valid for the unlocked enqueue() call.
            vehicle->busy = true;
            executor = vehicle->executor.get();
        }
    }
    if (rejection) {
        broadcast_command_ack(*rejection);
        return;
    }
    executor->enqueue(command);
    clear_busy_and_notify(command.vehicle_id());
}

void VehicleManager::broadcast_command_ack(const karshipta::v1::CommandAck& ack) const {
    karshipta::v1::Envelope envelope;
    *envelope.mutable_command_ack() = ack;
    transport_.broadcast(serialize_envelope(envelope));
}

void VehicleManager::broadcast_rejection_event(const karshipta::v1::CommandAck& ack) const {
    karshipta::v1::Envelope envelope;
    auto* event = envelope.mutable_event();
    event->set_vehicle_id(ack.vehicle_id());
    event->set_timestamp_ms(unix_epoch_ms());
    event->set_severity(karshipta::v1::SEVERITY_WARNING);
    event->set_code("COMMAND_REJECTED");
    event->set_message(ack.message());
    transport_.broadcast(serialize_envelope(envelope));
}

bool VehicleManager::start(const std::string& vehicle_id) {
    std::lock_guard lock(vehicles_mutex_);
    auto* vehicle = find_locked(vehicle_id);
    if (vehicle == nullptr) {
        spdlog::warn("start rejected: unknown vehicle_id '{}'", vehicle_id);
        return false;
    }
    if (vehicle->busy) {
        spdlog::warn("start rejected: vehicle_id '{}' is mid-transition", vehicle_id);
        return false;
    }
    if (vehicle->reconnect_worker.joinable()) {
        spdlog::warn("start rejected: vehicle_id '{}' already running", vehicle_id);
        return false;
    }

    // A previous stop() retired the executor; restore the command path before
    // the vehicle comes back online.
    if (vehicle->executor == nullptr) {
        vehicle->executor = make_executor(*vehicle);
    }
    vehicle->reconnect_worker = std::jthread(
        [this, vehicle](std::stop_token stop_token) { run_reconnect_loop(*vehicle, stop_token); });
    return true;
}

void VehicleManager::start_all() {
    // Snapshot ids first: start() takes the lock itself, and iterating the
    // map while calling a locking method would self-deadlock.
    for (const auto& vehicle_id : list_vehicle_ids()) {
        start(vehicle_id);
    }
}

void VehicleManager::persist_locked() const {
    if (persistence_path_.empty()) {
        return;
    }
    YAML::Node root;
    YAML::Node vehicles(YAML::NodeType::Sequence);
    for (const auto& [id, vehicle] : managed_vehicles_) {
        YAML::Node entry;
        entry["vehicle_id"] = vehicle.config.vehicle_id;
        entry["connection_url"] = vehicle.config.connection_url;
        entry["mavlink_system_id"] = vehicle.config.system_id;
        entry["name"] = vehicle.config.name;
        entry["type"] = karshipta::v1::VehicleType_Name(vehicle.config.type);
        vehicles.push_back(entry);
    }
    root["vehicles"] = vehicles;

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

std::size_t VehicleManager::load_persisted() {
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

    const YAML::Node vehicles = root["vehicles"];
    if (!vehicles || !vehicles.IsSequence()) {
        return 0;
    }

    std::size_t loaded = 0;
    for (const auto& entry : vehicles) {
        try {
            VehicleConfig cfg;
            cfg.vehicle_id = entry["vehicle_id"].as<std::string>();
            cfg.connection_url = entry["connection_url"].as<std::string>();
            cfg.system_id = entry["mavlink_system_id"].as<unsigned int>();
            cfg.name = entry["name"] ? entry["name"].as<std::string>() : std::string{};
            const std::string type_name = entry["type"] ? entry["type"].as<std::string>() : std::string{};
            if (!type_name.empty() && !karshipta::v1::VehicleType_Parse(type_name, &cfg.type)) {
                spdlog::warn("persisted vehicle '{}' has unknown type '{}', defaulting to unspecified",
                             cfg.vehicle_id, type_name);
            }
            // should_persist=false: reconstructing entries already on disk
            // must not rewrite the file once per entry while loading it.
            if (const auto error = add_vehicle_impl(cfg, /*should_persist=*/false)) {
                spdlog::warn("skipping persisted vehicle '{}': {}", cfg.vehicle_id, *error);
                continue;
            }
            ++loaded;
        } catch (const YAML::Exception& entry_error) {
            spdlog::warn("skipping malformed persisted vehicle entry: {}", entry_error.what());
        }
    }
    spdlog::info("loaded {} vehicle(s) from '{}'", loaded, persistence_path_.string());
    return loaded;
}

std::size_t VehicleManager::restore_and_start() {
    const std::size_t loaded = load_persisted();
    start_all();
    return loaded;
}

void VehicleManager::start_publishing(std::chrono::milliseconds interval) {
    publish_worker_ = std::jthread(
        [this, interval](std::stop_token stop_token) { run_publish_loop(interval, stop_token); });
}

void VehicleManager::run_publish_loop(std::chrono::milliseconds interval, std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        std::vector<std::vector<uint8_t>> frames;
        {
            // Cheap in-memory work only (TelemetryInfo's getters are cached
            // reads, not MAVSDK round-trips): safe to build every frame while
            // holding the lock. The broadcast itself is not, so it happens
            // after releasing it below.
            std::lock_guard lock(vehicles_mutex_);
            frames.reserve(managed_vehicles_.size());
            for (const auto& [id, vehicle] : managed_vehicles_) {
                karshipta::v1::Envelope envelope;
                *envelope.mutable_vehicle_state() =
                    build_vehicle_state(id, *vehicle.connection, *vehicle.telemetry);
                frames.push_back(serialize_envelope(envelope));
            }
        }
        for (const auto& frame : frames) {
            transport_.broadcast(frame);
        }
        std::this_thread::sleep_for(interval);
    }
}

void VehicleManager::send_vehicle_info(Transport::ClientId client) const {
    struct Snapshot {
        std::string vehicle_id;
        std::shared_ptr<mavsdk::System> system;
        karshipta::v1::VehicleType type;
    };
    // Snapshot cheap values under the lock, then release it before the
    // per-vehicle work below: query_firmware_version() is a blocking MAVSDK
    // round-trip, not cached state, and transport_.send() is a blocking
    // socket write - neither may run while vehicles_mutex_ is held, or every
    // other VehicleManager call stalls behind one client connect.
    std::vector<Snapshot> snapshots;
    {
        std::lock_guard lock(vehicles_mutex_);
        snapshots.reserve(managed_vehicles_.size());
        for (const auto& [id, vehicle] : managed_vehicles_) {
            snapshots.push_back({id, vehicle.connection->get_system(), vehicle.config.type});
        }
    }
    for (const auto& snapshot : snapshots) {
        if (!snapshot.system) {
            spdlog::warn(
                "client {} connected but vehicle '{}' has no discovered system yet, skipping "
                "VehicleInfo",
                client, snapshot.vehicle_id);
            continue;
        }
        karshipta::v1::VehicleInfo info;
        info.set_vehicle_id(snapshot.vehicle_id);
        info.set_type(snapshot.type);
        info.set_autopilot(kAutopilotName);
        info.set_mavlink_system_id(snapshot.system->get_system_id());
        info.set_firmware_version(query_firmware_version(snapshot.system));

        karshipta::v1::Envelope envelope;
        *envelope.mutable_vehicle_info() = info;
        transport_.send(client, serialize_envelope(envelope));
    }
}

bool VehicleManager::disarm_if_armed(const std::string& vehicle_id, ManagedVehicle& vehicle) {
    if (!vehicle.telemetry->is_armed()) {
        return true;
    }
    const mavsdk::Action::Result result = vehicle.actions->disarm();
    if (result == mavsdk::Action::Result::Success) {
        spdlog::info("vehicle_id '{}' disarmed", vehicle_id);
        return true;
    }
    spdlog::warn("disarm failed for vehicle_id '{}': {}", vehicle_id,
                 VehicleActions::result_name(result));
    return false;
}

void VehicleManager::stop_worker(ManagedVehicle& vehicle) {
    if (!vehicle.reconnect_worker.joinable()) {
        return;
    }
    // Joining under vehicles_mutex_ is deliberate: other threads read
    // joinable() under the lock, so the join must not race them. Worst case
    // it blocks ~3s (a discovery attempt is not stop-token-interruptible).
    // Not moved unlocked: std::jthread::joinable()/join() are not safe to
    // call concurrently on the same object from different threads, so an
    // unlocked join here would race is_started()/list_status()'s locked
    // joinable() reads on this exact object. A real fix needs a per-vehicle
    // mutex (finer than vehicles_mutex_), which is a larger change than this
    // pass's scope; the ~3s worst case is accepted for now.
    vehicle.reconnect_worker.request_stop();
    vehicle.reconnect_worker.join();
}

void VehicleManager::clear_busy_and_notify(const std::string& vehicle_id) {
    std::lock_guard lock(vehicles_mutex_);
    if (auto* v = find_locked(vehicle_id)) {
        v->busy = false;
    }
    // Unconditional: ~VehicleManager()'s wait must re-evaluate even when the
    // vehicle that just finished was erased by this same transition (a
    // successful remove_vehicle_impl()), not just when a flag actually flips.
    busy_cv_.notify_all();
}

std::optional<std::string> VehicleManager::verify_grounded_and_disarm(
    const std::string& vehicle_id, ManagedVehicle& vehicle) {
    // Unlocked: is_in_air()/is_armed()/disarm() are blocking MAVSDK calls.
    // Safe because vehicle.busy == true, set by the caller before its own
    // executor-retiring unlock window began.
    //
    // link_state() is checked explicitly here, not inferred from
    // is_in_air()/is_armed() returning false: TelemetryInfo only defaults to
    // false before the plugin is ever created (never connected). Once
    // created, it keeps returning MAVSDK's last-cached value even after the
    // link drops, which could be stale-true for a vehicle that was armed or
    // airborne right before the drop. Every ground-safety guard in this class
    // must gate on link_state() itself, never trust is_in_air()/is_armed()
    // alone to mean "unavailable therefore safe."
    const bool link_ok = vehicle.connection->link_state() != VehicleConnection::LinkState::kLinkDown;
    const bool grounded = link_ok && !vehicle.telemetry->is_in_air();
    const bool disarmed = grounded && disarm_if_armed(vehicle_id, vehicle);
    if (link_ok && grounded && disarmed) {
        return std::nullopt;
    }

    std::lock_guard lock(vehicles_mutex_);
    vehicle.executor = make_executor(vehicle);
    if (!link_ok) return "link dropped during transition";
    if (!grounded) return "vehicle took off during transition";
    return "failed to disarm";
}

bool VehicleManager::stop(const std::string& vehicle_id) {
    ManagedVehicle* vehicle = nullptr;
    std::unique_ptr<CommandExecutor> retired_executor;
    {
        std::lock_guard lock(vehicles_mutex_);
        vehicle = find_locked(vehicle_id);
        if (vehicle == nullptr) {
            spdlog::warn("stop rejected: unknown vehicle_id '{}'", vehicle_id);
            return false;
        }
        if (vehicle->busy) {
            spdlog::warn("stop rejected: vehicle_id '{}' is mid-transition", vehicle_id);
            return false;
        }
        // Before any side effect: a stop() that rejects must not have already
        // disarmed the vehicle.
        if (!vehicle->reconnect_worker.joinable()) {
            spdlog::warn("stop rejected: vehicle_id '{}' not running", vehicle_id);
            return false;
        }
        // Link down after discovery means telemetry cannot be trusted (see
        // verify_grounded_and_disarm()'s comment on why is_in_air()/
        // is_armed() alone can't stand in for this check). Never-discovered
        // passes; nothing we ever saw can be airborne because of us.
        if (vehicle->connection->link_state() == VehicleConnection::LinkState::kLinkDown) {
            spdlog::warn("stop rejected: vehicle_id '{}' link is down, state unknown; "
                         "use force_stop",
                         vehicle_id);
            return false;
        }
        if (vehicle->telemetry->is_in_air()) {
            spdlog::warn("stop rejected: vehicle_id '{}' is in the air", vehicle_id);
            return false;
        }
        vehicle->busy = true;
        retired_executor = std::move(vehicle->executor);
    }
    // busy is set: no other transition can start, remove_vehicle cannot erase
    // this entry, so `vehicle` stays valid across the unlocked phases below.
    ScopeExit clear_busy{[this, &vehicle_id] { clear_busy_and_notify(vehicle_id); }};

    // Unlocked: joins the executor worker (may be mid-MAVSDK-call for
    // seconds) and broadcasts rejection acks for whatever was still queued.
    retired_executor.reset();

    // Re-check now that no command path exists that could have armed or
    // launched the vehicle between the guard above and the quiesce just now.
    if (const auto error = verify_grounded_and_disarm(vehicle_id, *vehicle)) {
        spdlog::warn("stop rejected: vehicle_id '{}' {}", vehicle_id, *error);
        return false;
    }

    std::lock_guard lock(vehicles_mutex_);
    stop_worker(*vehicle);
    spdlog::info("vehicle_id '{}' stopped", vehicle_id);
    return true;
}

void VehicleManager::stop_all() {
    for (const auto& vehicle_id : list_vehicle_ids()) {
        stop(vehicle_id);
    }
}

bool VehicleManager::force_stop(const std::string& vehicle_id) {
    ManagedVehicle* vehicle = nullptr;
    std::unique_ptr<CommandExecutor> retired_executor;
    {
        std::lock_guard lock(vehicles_mutex_);
        vehicle = find_locked(vehicle_id);
        if (vehicle == nullptr) {
            spdlog::warn("force_stop rejected: unknown vehicle_id '{}'", vehicle_id);
            return false;
        }
        if (vehicle->busy) {
            spdlog::warn("force_stop rejected: vehicle_id '{}' is mid-transition", vehicle_id);
            return false;
        }
        vehicle->busy = true;
        retired_executor = std::move(vehicle->executor);
    }
    ScopeExit clear_busy{[this, &vehicle_id] { clear_busy_and_notify(vehicle_id); }};

    retired_executor.reset();  // unlocked; see stop()

    const auto link = vehicle->connection->link_state();
    const bool airborne = vehicle->telemetry->is_in_air();
    if (airborne || link == VehicleConnection::LinkState::kLinkDown) {
        // Direct call is safe: the executor is retired, so this is the only
        // command path to the vehicle. Best-effort by design; if the link is
        // down the autopilot may still be flying its own failsafe RTL.
        const mavsdk::Action::Result result = vehicle->actions->return_to_launch();
        if (result == mavsdk::Action::Result::Success) {
            spdlog::warn("force_stop: rtl commanded for vehicle_id '{}'", vehicle_id);
        } else {
            spdlog::warn("force_stop: rtl failed for vehicle_id '{}': {}", vehicle_id,
                         VehicleActions::result_name(result));
        }

        // Supervise the flight home instead of abandoning it: the reconnect
        // worker keeps running, and only a vehicle that is provably connected,
        // landed, and disarmed lets us proceed. `connected` is required in
        // the condition below specifically because is_in_air()/is_armed()
        // do NOT reliably read false on link-down once the plugin has been
        // created once (see verify_grounded_and_disarm()); without the
        // explicit connected check, a link drop mid-RTL could be misread as
        // "landed and disarmed" purely because the cached values happened to
        // predate the vehicle taking off, silently reintroducing an
        // unsupervised flying vehicle.
        const auto deadline = std::chrono::steady_clock::now() + kForceStopLandingTimeout;
        bool safe_on_ground = false;
        while (std::chrono::steady_clock::now() < deadline) {
            const bool connected =
                vehicle->connection->link_state() == VehicleConnection::LinkState::kConnected;
            if (connected && !vehicle->telemetry->is_in_air() &&
                !vehicle->telemetry->is_armed()) {
                safe_on_ground = true;
                break;
            }
            std::this_thread::sleep_for(kReconnectPollInterval);
        }
        if (!safe_on_ground) {
            std::lock_guard lock(vehicles_mutex_);
            vehicle->executor = make_executor(*vehicle);
            spdlog::error("force_stop timed out for vehicle_id '{}': not confirmed landed and "
                          "disarmed within {}s; monitoring stays active",
                          vehicle_id, kForceStopLandingTimeout.count());
            return false;
        }
    }

    // Grounded (or confirmed landed): disarm is best-effort here, unlike
    // stop(); taking the vehicle offline is the whole point of force_stop.
    (void)disarm_if_armed(vehicle_id, *vehicle);

    std::lock_guard lock(vehicles_mutex_);
    stop_worker(*vehicle);
    spdlog::warn("vehicle_id '{}' force-stopped", vehicle_id);
    return true;
}

void VehicleManager::force_stop_all() {
    for (const auto& vehicle_id : list_vehicle_ids()) {
        force_stop(vehicle_id);
    }
}

bool VehicleManager::is_started(const std::string& vehicle_id) const {
    std::lock_guard lock(vehicles_mutex_);
    const auto* vehicle = find_locked(vehicle_id);
    return vehicle != nullptr && vehicle->reconnect_worker.joinable();
}

bool VehicleManager::is_connected(const std::string& vehicle_id) const {
    std::lock_guard lock(vehicles_mutex_);
    const auto* vehicle = find_locked(vehicle_id);
    return vehicle != nullptr && vehicle->connection->is_connected();
}

std::vector<VehicleStatus> VehicleManager::list_status() const {
    std::lock_guard lock(vehicles_mutex_);
    std::vector<VehicleStatus> statuses;
    statuses.reserve(managed_vehicles_.size());
    for (const auto& [id, vehicle] : managed_vehicles_) {
        statuses.push_back(VehicleStatus{
            .vehicle_id = id,
            .started = vehicle.reconnect_worker.joinable(),
            .connected = vehicle.connection->is_connected(),
        });
    }
    return statuses;
}

std::optional<std::string> VehicleManager::remove_vehicle_impl(const std::string& vehicle_id) {
    ManagedVehicle* vehicle = nullptr;
    std::unique_ptr<CommandExecutor> retired_executor;
    {
        std::lock_guard lock(vehicles_mutex_);
        vehicle = find_locked(vehicle_id);
        if (vehicle == nullptr) {
            return "unknown vehicle_id: " + vehicle_id;
        }
        if (vehicle->busy) {
            return "vehicle is mid-transition";
        }
        // Same trust rule as stop() (see verify_grounded_and_disarm()):
        // erasing the connection to a possibly-flying vehicle removes the
        // only way to ever command it again.
        if (vehicle->connection->link_state() == VehicleConnection::LinkState::kLinkDown) {
            return "link is down, vehicle state unknown";
        }
        if (vehicle->telemetry->is_in_air()) {
            return "vehicle is in the air";
        }
        vehicle->busy = true;
        retired_executor = std::move(vehicle->executor);
    }
    ScopeExit clear_busy{[this, &vehicle_id] { clear_busy_and_notify(vehicle_id); }};

    retired_executor.reset();  // unlocked; see stop()

    if (const auto error = verify_grounded_and_disarm(vehicle_id, *vehicle)) {
        return *error;
    }

    std::lock_guard lock(vehicles_mutex_);
    stop_worker(*vehicle);
    managed_vehicles_.erase(vehicle_id);
    persist_locked();
    spdlog::info("vehicle_id '{}' removed (total={})", vehicle_id, managed_vehicles_.size());
    return std::nullopt;
}

bool VehicleManager::remove_vehicle(const std::string& vehicle_id) {
    const auto error = remove_vehicle_impl(vehicle_id);
    if (error) {
        spdlog::warn("remove_vehicle rejected: vehicle_id '{}': {}", vehicle_id, *error);
        return false;
    }
    return true;
}

void VehicleManager::remove_all() {
    for (const auto& vehicle_id : list_vehicle_ids()) {
        remove_vehicle(vehicle_id);
    }
}

karshipta::v1::VehicleConfigAck VehicleManager::handle_add_vehicle(
    const karshipta::v1::AddVehicle& request) {
    karshipta::v1::VehicleConfigAck ack;
    ack.set_request_id(request.request_id());
    ack.set_vehicle_id(request.vehicle_id());

    const VehicleConfig cfg{
        .vehicle_id = request.vehicle_id(),
        .connection_url = request.connection_url(),
        .system_id = request.mavlink_system_id(),
        .type = request.type(),
        .name = request.name(),
    };

    std::optional<std::string> error;
    try {
        error = add_vehicle_impl(cfg);
    } catch (const std::invalid_argument& bad_url) {
        // VehicleConnection rejects an empty/invalid URL by throwing; a bad
        // wire request must become a REJECTED ack, not a gateway crash.
        error = bad_url.what();
    }
    if (!error && !start(cfg.vehicle_id)) {
        error = "vehicle registered but failed to start";
        // Roll back the registration: leaving a registered-but-never-started
        // vehicle behind would strand it permanently, since add_vehicle_impl
        // rejects a retry with the same vehicle_id as "already registered."
        // Best-effort and results ignored: a concurrent remove racing in
        // during this window is exactly why start() could have failed on a
        // freshly-added vehicle in the first place, and in that case this
        // call is a harmless no-op (already gone).
        (void)remove_vehicle_impl(cfg.vehicle_id);
    }

    if (error) {
        spdlog::warn("add_vehicle request '{}' rejected: {}", request.request_id(), *error);
        ack.set_status(karshipta::v1::VEHICLE_CONFIG_STATUS_REJECTED);
        ack.set_message(*error);
    } else {
        ack.set_status(karshipta::v1::VEHICLE_CONFIG_STATUS_ACCEPTED);
        ack.set_message("connection attempt underway");
    }
    return ack;
}

karshipta::v1::VehicleConfigAck VehicleManager::handle_remove_vehicle(
    const karshipta::v1::RemoveVehicle& request) {
    karshipta::v1::VehicleConfigAck ack;
    ack.set_request_id(request.request_id());
    ack.set_vehicle_id(request.vehicle_id());

    const auto error = remove_vehicle_impl(request.vehicle_id());
    if (error) {
        spdlog::warn("remove_vehicle request '{}' rejected: {}", request.request_id(), *error);
        ack.set_status(karshipta::v1::VEHICLE_CONFIG_STATUS_REJECTED);
        ack.set_message(*error);
    } else {
        ack.set_status(karshipta::v1::VEHICLE_CONFIG_STATUS_ACCEPTED);
        ack.set_message("vehicle removed");
    }
    return ack;
}

void VehicleManager::run_reconnect_loop(ManagedVehicle& vehicle, std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        if (!vehicle.connection->connect_with_retry(stop_token)) {
            // Only false if stop_token was cancelled during the retry itself.
            break;
        }
        spdlog::info("vehicle connected (system_id={})", vehicle.config.system_id);
        // PX4 forgets requested stream rates across a link drop, so this is
        // re-requested on every reconnect, not just the first.
        vehicle.telemetry->set_telemetry_rate(kTelemetryRateHz);

        while (vehicle.connection->is_connected() && !stop_token.stop_requested()) {
            std::this_thread::sleep_for(kReconnectPollInterval);
        }

        if (stop_token.stop_requested()) {
            break;
        }
        spdlog::warn("vehicle link lost (system_id={}), reconnecting", vehicle.config.system_id);
    }
}
