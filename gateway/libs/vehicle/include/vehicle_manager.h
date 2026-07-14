//
// Created by amir abkhoshk on 13/07/2026.
//

#ifndef KARSHIPTA_GATEWAY_VEHICLE_MANAGER_H
#define KARSHIPTA_GATEWAY_VEHICLE_MANAGER_H
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <mavsdk/mavsdk.h>
#include <karshipta/v1/fleet.pb.h>
#include <vehicle_connection.h>
#include <vehicle_actions.h>
#include <telemetry.h>
#include <command_executor.h>
#include <transport.h>

struct VehicleConfig {
    std::string vehicle_id;
    std::string connection_url;
    // MAVLink system id this vehicle broadcasts. 0 means "bind to the first
    // autopilot on the endpoint" (fleet.proto AddVehicle.mavlink_system_id),
    // which is only safe when vehicles connect one at a time.
    unsigned int system_id;
};

struct ManagedVehicle {
    unsigned int system_id;
    std::unique_ptr<VehicleConnection> connection;
    std::unique_ptr<TelemetryInfo> telemetry;
    std::unique_ptr<VehicleActions> actions;
    // Null while the vehicle is stopped or a transition is quiescing it;
    // dispatch_command() rejects commands in that window. Written only under
    // VehicleManager::vehicles_mutex_.
    std::unique_ptr<CommandExecutor> executor;
    // True while a stop()/force_stop()/remove_vehicle() transition is in
    // flight, including its unlocked phases. While set: no other transition
    // may begin, remove_vehicle() may not erase this entry, and only the
    // thread that set it may mutate this struct. Written only under
    // VehicleManager::vehicles_mutex_.
    bool busy = false;
    // Last: its loop calls connection->connect_with_retry()/is_connected() on
    // every iteration, so it must stop and join before connection (and the
    // other members above it) are torn down. Empty (not joinable) until
    // VehicleManager::start() launches it.
    std::jthread reconnect_worker;
};

// Snapshot of one vehicle's start/connect state, returned by
// VehicleManager::list_status(). started == reconnect_worker running (does
// not imply connected); connected == currently linked to its autopilot.
struct VehicleStatus {
    std::string vehicle_id;
    bool started;
    bool connected;
};

// Owns the fleet: one ManagedVehicle per configured vehicle, all sharing one
// Mavsdk core. All public methods are thread-safe (vehicles_mutex_); no
// pointer or reference into the internal map ever escapes the lock.
class VehicleManager {
public:
    VehicleManager(std::shared_ptr<mavsdk::Mavsdk> mavsdk, Transport& tp);
    // Blocks until no vehicle is mid-transition (busy) before letting
    // managed_vehicles_ destruct: force_stop() in particular spends up to
    // kForceStopLandingTimeoutS unlocked, holding a raw ManagedVehicle*, and
    // without this wait a concurrent destructor could free that vehicle out
    // from under it. Once every transition has quiesced, managed_vehicles_
    // destroys each ManagedVehicle's members in reverse declaration order:
    // reconnect_worker stops and joins first, then the executor (rejecting
    // whatever it still queued), then actions/telemetry/connection. This does
    // NOT proactively RTL vehicles that were never told to stop; graceful
    // shutdown of a still-flying, never-force_stop()ped fleet needs main.cpp
    // to call force_stop_all() before dropping the VehicleManager, which
    // isn't wired up yet (no signal handling exists in main.cpp today).
    // Callers must not invoke NEW methods concurrently with destruction;
    // that's a normal C++ lifetime rule this class does not attempt to lift.
    ~VehicleManager();

    VehicleManager(const VehicleManager&) = delete;
    VehicleManager& operator=(const VehicleManager&) = delete;
    VehicleManager(VehicleManager&&) = delete;
    VehicleManager& operator=(VehicleManager&&) = delete;

    // Builds the VehicleConnection/TelemetryInfo/VehicleActions/CommandExecutor
    // graph for cfg and registers it under cfg.vehicle_id. Does not connect to
    // the vehicle; construction only. Returns false, adding nothing, if
    // vehicle_id is empty or already registered, or if a nonzero system_id is
    // already bound to a different vehicle_id. Throws std::invalid_argument
    // for an empty connection_url (from VehicleConnection).
    bool add_vehicle(const VehicleConfig& cfg);

    // Every currently registered vehicle_id (map key order, i.e. sorted).
    // Just the ids, no start/connect state; use list_status() for that.
    [[nodiscard]] std::vector<std::string> list_vehicle_ids() const;

    // Routes command to the CommandExecutor of the vehicle named
    // command.vehicle_id(). Synthesizes and broadcasts a REJECTED CommandAck
    // (gateway rule 5: no silent drop) if that vehicle is unknown, stopped,
    // or mid-transition. Not const: briefly marks the vehicle busy so the
    // executor can be enqueued into without holding vehicles_mutex_ for the
    // ACCEPTED ack's broadcast (a blocking socket write that must not stall
    // every other vehicle's manager calls behind one slow client).
    void dispatch_command(const karshipta::v1::Command& command);

    // Launches reconnect_worker for the vehicle with this id, recreating its
    // CommandExecutor if a previous stop() retired it. Returns false if
    // vehicle_id is unknown, mid-transition, or already running.
    bool start(const std::string& vehicle_id);

    // Calls start() for every currently registered vehicle.
    void start_all();

    // Takes the vehicle offline safely: rejects if unknown, mid-transition,
    // not running, airborne, or link-down-after-discovery (telemetry cannot
    // be trusted, so ground state is unknown; use force_stop). Otherwise
    // retires the CommandExecutor first so no command can race the shutdown,
    // re-checks the ground state, disarms if armed (rejecting and restoring
    // the executor when the disarm fails), then stops reconnect_worker. The
    // vehicle stays registered.
    bool stop(const std::string& vehicle_id);

    // Calls stop() for every currently registered vehicle. Sequential; each
    // vehicle's rejection reasons apply individually.
    void stop_all();

    // Operator override for a vehicle stop() refuses to touch: retires the
    // executor, commands return_to_launch() when airborne or link-down, then
    // SUPERVISES the flight home, keeping reconnect_worker running and
    // polling until the vehicle is connected, landed, and disarmed (link-down
    // never reads as landed). Only then disarms best-effort and stops the
    // worker. On timeout (kForceStopLandingTimeoutS) the executor is restored
    // and monitoring is left running: a flying vehicle is never abandoned.
    // Returns false if unknown, mid-transition, or the supervision timed out.
    bool force_stop(const std::string& vehicle_id);

    // Calls force_stop() for every currently registered vehicle.
    void force_stop_all();

    // True if start() is running for vehicle_id and hasn't been stop()ped.
    // Does not imply connected. False (not an error) if vehicle_id is
    // unknown.
    [[nodiscard]] bool is_started(const std::string& vehicle_id) const;

    // True if vehicle_id is currently linked to its autopilot. False for
    // both "never started" and "started but link down"; false (not an
    // error) if vehicle_id is unknown.
    [[nodiscard]] bool is_connected(const std::string& vehicle_id) const;

    // Snapshot of every registered vehicle's start/connect state.
    [[nodiscard]] std::vector<VehicleStatus> list_status() const;

    // Applies stop()'s ground-safety rules (reject airborne or link-down,
    // disarm if armed), then erases the vehicle entirely. Returns false,
    // removing nothing, if any guard rejects.
    bool remove_vehicle(const std::string& vehicle_id);

    // Calls remove_vehicle() for every currently registered vehicle; rejected
    // vehicles stay in place. Check list_status() afterward for survivors.
    void remove_all();

    // Wire-level entry points: translate an AddVehicle/RemoveVehicle request
    // from a console client and produce the VehicleConfigAck to send back.
    // Never throws on bad input; every rejection carries a reason.
    karshipta::v1::VehicleConfigAck handle_add_vehicle(const karshipta::v1::AddVehicle& request);
    karshipta::v1::VehicleConfigAck handle_remove_vehicle(const karshipta::v1::RemoveVehicle& request);

private:
    std::shared_ptr<mavsdk::Mavsdk> mavsdk_;
    Transport& transport_;
    // Guards managed_vehicles_ (structure and every element's executor/busy/
    // reconnect_worker members). Mutable so const query methods can lock.
    // Lock order: vehicles_mutex_ before any VehicleConnection/TelemetryInfo/
    // VehicleActions internal mutex; nothing locks in the other direction.
    mutable std::mutex vehicles_mutex_;
    // Notified whenever a vehicle's busy flag clears (clear_busy_and_notify);
    // ~VehicleManager() waits on this until no vehicle is busy.
    mutable std::condition_variable busy_cv_;
    std::map<std::string, ManagedVehicle> managed_vehicles_;

    // Lookup while holding vehicles_mutex_. The returned pointer is only
    // valid under the lock, or between lock windows of a transition that has
    // set busy on that vehicle (busy blocks erase).
    [[nodiscard]] ManagedVehicle* find_locked(const std::string& vehicle_id);
    [[nodiscard]] const ManagedVehicle* find_locked(const std::string& vehicle_id) const;

    // Reason-returning cores (nullopt = success) shared by the bool public
    // methods (which log) and the handle_* wire entry points (which put the
    // reason in the ack). Both take vehicles_mutex_ themselves.
    [[nodiscard]] std::optional<std::string> add_vehicle_impl(const VehicleConfig& cfg);
    [[nodiscard]] std::optional<std::string> remove_vehicle_impl(const std::string& vehicle_id);

    // Builds the CommandExecutor wired to broadcast its acks; shared by
    // add_vehicle_impl() and every path that restores a retired executor.
    [[nodiscard]] std::unique_ptr<CommandExecutor> make_executor(ManagedVehicle& vehicle);

    // Clears vehicle_id's busy flag (no-op if the vehicle was erased in the
    // meantime, e.g. by remove_vehicle_impl's own success path) and always
    // notifies busy_cv_, so ~VehicleManager()'s wait re-evaluates even when
    // the vehicle that just finished no longer exists to be found.
    void clear_busy_and_notify(const std::string& vehicle_id);

    // Shared epilogue for stop() and remove_vehicle_impl(), called once the
    // vehicle's executor has already been retired (so no command can race
    // this check) and vehicle.busy == true (so nothing else can touch it).
    // Re-checks link + ground state, disarms if armed, and returns nullopt on
    // success. On any failure, restores the executor (so the vehicle remains
    // usable) and returns a rejection reason instead. Does not touch
    // reconnect_worker, busy, or the map; callers finish those themselves.
    [[nodiscard]] std::optional<std::string> verify_grounded_and_disarm(
        const std::string& vehicle_id, ManagedVehicle& vehicle);

    // Wraps ack in an Envelope and broadcasts it. Takes no lock; safe from
    // executor worker threads and from under vehicles_mutex_.
    void broadcast_command_ack(const karshipta::v1::CommandAck& ack) const;

    // Body of vehicle.reconnect_worker, run on start()'s jthread. Connects,
    // waits while connected, and on drop connects again, until stop_token is
    // cancelled. Touches only vehicle.connection and vehicle.system_id, so it
    // never needs vehicles_mutex_ (joining under the lock cannot deadlock).
    void run_reconnect_loop(ManagedVehicle& vehicle, std::stop_token stop_token);

    // No-op (returns true) if vehicle isn't armed; otherwise attempts to
    // disarm and returns whether it succeeded. Precondition: caller holds
    // vehicles_mutex_ OR has set vehicle.busy (the pointees it reads are
    // stable either way). Blocking MAVSDK call; prefer calling it unlocked.
    [[nodiscard]] bool disarm_if_armed(const std::string& vehicle_id, ManagedVehicle& vehicle);

    // Requests reconnect_worker to stop and joins it; no-op if not running.
    // Must be called under vehicles_mutex_ (other threads read joinable()).
    void stop_worker(ManagedVehicle& vehicle);
};

#endif  // KARSHIPTA_GATEWAY_VEHICLE_MANAGER_H
