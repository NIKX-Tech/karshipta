//
// Created by amir abkhoshk on 13/07/2026.
//

#ifndef KARSHIPTA_GATEWAY_VEHICLE_MANAGER_H
#define KARSHIPTA_GATEWAY_VEHICLE_MANAGER_H
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
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
#include <vehicle_mission.h>
#include <mission_importer.h>
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
    karshipta::v1::VehicleType type = karshipta::v1::VEHICLE_TYPE_UNSPECIFIED;
    // Display name; empty means the console should fall back to vehicle_id
    // (matches fleet.proto AddVehicle.name's comment).
    std::string name;
};

// One vehicle's full object graph plus its own transition state. Held by
// VehicleManager as a shared_ptr (see managed_vehicles_): a lookup copies the
// shared_ptr under the structural lock and releases it immediately, so one
// vehicle's slow teardown never blocks a lookup for any other vehicle.
//
// Lifetime invariant: nothing may retain a copy of this shared_ptr beyond the
// synchronous scope of the call that obtained it (no background thread, no
// member field elsewhere). Every current call site follows this already
// (dispatch_command, the handle_mission_* methods, run_publish_loop's per-tick
// snapshot, and ~VehicleManager()'s own snapshot all let their copy go out of
// scope before returning/sleeping). This is what lets ~VehicleManager() bound
// this object's true lifetime to managed_vehicles_'s own destruction: relax
// this invariant and a lingering copy could keep a vehicle (and its
// CommandExecutor's worker thread) alive past VehicleManager's own teardown.
struct ManagedVehicle {
    // The config this vehicle was built from (persistence and VehicleInfo both
    // read this instead of asking VehicleConnection, so neither needs a live
    // connection to answer). config.system_id replaces a standalone field.
    // Set once at construction, never reassigned: safe to read without
    // mutex_.
    VehicleConfig config;
    // Pointers below are set once at construction and never reassigned; the
    // pointee's own thread-safety (VehicleConnection/TelemetryInfo/etc. each
    // guard their own internal state) is what protects concurrent use, not
    // this struct. Only executor is ever reassigned after construction, which
    // is why it (along with busy and reconnect_worker) lives under mutex_
    // below instead of alongside these.
    std::unique_ptr<VehicleConnection> connection;
    std::unique_ptr<TelemetryInfo> telemetry;
    std::unique_ptr<VehicleActions> actions;
    // Owns mission upload/download/start/pause/progress for this vehicle
    // (BRIEF.md M5). Built before executor (which holds a reference to it)
    // and after actions/telemetry, matching the same by-reference build
    // order as the rest of this quartet.
    std::unique_ptr<VehicleMission> mission;
    // Converts a customer-uploaded QGC/WPL mission file into a Mission that
    // can be handed to `mission`'s enqueue_upload(). Only needs `connection`,
    // so its position relative to `mission`/`executor` doesn't matter for
    // build order, unlike the rest of this quartet.
    std::unique_ptr<MissionImporter> mission_importer;

    // Guards executor/busy/reconnect_worker below: this vehicle's own
    // transition state, independent of every other vehicle's. Lock order:
    // VehicleManager::vehicles_mutex_ (structural: managed_vehicles_ itself)
    // before this, never the other way; nothing ever holds two different
    // vehicles' mutex_ at once. Mutable so const query methods (is_started(),
    // etc.) can lock a const ManagedVehicle&.
    mutable std::mutex mutex_;
    // Null while the vehicle is stopped or a transition is quiescing it;
    // dispatch_command() rejects commands in that window. Guarded by mutex_.
    std::unique_ptr<CommandExecutor> executor;
    // True while a stop()/force_stop()/remove_vehicle() transition is in
    // flight, including its unlocked phases. While set: no other transition
    // on THIS vehicle may begin, remove_vehicle_impl() may not erase this
    // entry from managed_vehicles_, and only the thread that set it may
    // mutate executor/reconnect_worker. Guarded by mutex_.
    bool busy = false;
    // Last: its loop calls connection->connect_with_retry()/is_connected() on
    // every iteration, so it must stop and join before connection (and the
    // other members above it) are torn down. Empty (not joinable) until
    // VehicleManager::start() launches it. Guarded by mutex_ (every
    // joinable()/request_stop()/join() call site takes mutex_ first;
    // std::jthread's joinable()/join() are not safe to call concurrently on
    // the same object from two threads without this).
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
// Mavsdk core. All public methods are thread-safe. Locking is two-level:
// vehicles_mutex_ is structural only (managed_vehicles_'s map shape: insert,
// erase, lookup), always fast; each vehicle's own transition state (busy,
// executor, reconnect_worker) lives behind that vehicle's own ManagedVehicle::
// mutex_, so one vehicle's slow teardown or reconnect-join never blocks any
// other vehicle's calls. No raw pointer or reference into the internal map
// ever escapes vehicles_mutex_; find_shared_locked() returns a shared_ptr
// copy instead, which is what keeps a vehicle alive for the duration of an
// unlocked call even if it's concurrently removed from the map.
class VehicleManager {
public:
    // Default publish interval for start_publishing(): BRIEF.md M2's ~5Hz
    // per-vehicle VehicleState target.
    static constexpr std::chrono::milliseconds kDefaultPublishInterval{200};

    // persistence_path: where add_vehicle_impl()/remove_vehicle_impl() persist
    // the fleet after every successful mutation, and where load_persisted()
    // reads from. An empty path (the default) disables persistence entirely,
    // so tests stay disk-I/O-free unless they opt in.
    VehicleManager(std::shared_ptr<mavsdk::Mavsdk> mavsdk, Transport& tp,
                   std::filesystem::path persistence_path = {});

    // Builds the shared Mavsdk core itself (VehicleConnection::create_shared_core())
    // so callers never need to name VehicleConnection just to construct a
    // VehicleManager. The one entry point main.cpp (or any future consumer)
    // should use.
    static std::unique_ptr<VehicleManager> create(Transport& tp,
                                                    std::filesystem::path persistence_path = {});
    // Snapshots every currently registered vehicle (structural lock, once),
    // then waits for each one's own busy flag to clear before letting it go:
    // force_stop() in particular spends up to kForceStopLandingTimeoutS
    // unlocked, holding a shared_ptr<ManagedVehicle>, and without this wait a
    // concurrent destructor could tear that vehicle down out from under it.
    // No shared condition_variable spans this (busy now lives in N per-
    // vehicle mutexes, which a single condition_variable's one-mutex contract
    // can't wait across); each vehicle is polled on its own mutex_ instead,
    // acceptable because this runs once, at shutdown, never on a hot path.
    // Once every snapshot vehicle's busy flag has cleared, the local
    // snapshot's shared_ptrs go out of scope and managed_vehicles_ destroys
    // each ManagedVehicle's members in reverse declaration order:
    // reconnect_worker stops and joins first, then the executor (rejecting
    // whatever it still queued), then mission/actions/telemetry/connection.
    // This does NOT proactively RTL vehicles that were never told to stop;
    // graceful shutdown of a still-flying, never-force_stop()ped fleet needs
    // main.cpp to call force_stop_all() before dropping the VehicleManager,
    // which isn't wired up yet (no signal handling exists in main.cpp today).
    // Callers must not invoke NEW methods concurrently with destruction;
    // that's a normal C++ lifetime rule this class does not attempt to lift.
    // Relies on the ManagedVehicle lifetime invariant documented on that
    // struct: nothing outside this snapshot and managed_vehicles_ itself may
    // be holding a shared_ptr<ManagedVehicle> once this snapshot is taken.
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

    // Routes mission to the VehicleMission of the vehicle named
    // mission.vehicle_id() (Envelope.mission_upload, console-editor-authored,
    // not a Command). Broadcasts a WARNING Event (gateway rule 5) if that
    // vehicle is unknown or stopped; the upload's own success/failure
    // surfaces later, via the publish tick polling take_upload_result().
    void handle_mission_upload(const karshipta::v1::Mission& mission);

    // Converts upload via the vehicle's MissionImporter, then routes the
    // result through handle_mission_upload() exactly like a console-editor
    // mission. Broadcasts a WARNING Event immediately if the vehicle is
    // unknown/stopped or the conversion itself fails; MissionImporter::import()
    // is local parsing, not a MAVLink round trip, so this runs synchronously
    // rather than being queued.
    void handle_mission_file_upload(const karshipta::v1::MissionFileUpload& upload);

    // Queues a mission-download request on the vehicle named
    // request.vehicle_id(). Broadcasts a WARNING Event if that vehicle is
    // unknown or stopped; the result (a mission_download Envelope, or a
    // WARNING Event on failure) surfaces later, via the publish tick polling
    // take_download_result().
    void handle_mission_download_request(const karshipta::v1::MissionDownloadRequest& request);

    // Launches reconnect_worker for the vehicle with this id, recreating its
    // CommandExecutor if a previous stop() retired it. Returns false if
    // vehicle_id is unknown, mid-transition, or already running.
    bool start(const std::string& vehicle_id);

    // Calls start() for every currently registered vehicle.
    void start_all();

    // Reads persistence_path (set at construction), registering each entry via
    // add_vehicle_impl() without re-persisting what's already on disk. Returns
    // 0 without error if no path was set or the file doesn't exist (first
    // run). A malformed entry is logged and skipped; the rest still load.
    std::size_t load_persisted();

    // load_persisted() followed by start_all(): the crash-recovery entry
    // point. Call once at boot, before serving any client, so a vehicle that
    // was mid-flight when the gateway last exited starts reconnecting
    // immediately instead of being forgotten. Returns how many vehicles were
    // loaded (not how many started; start() failures are logged individually).
    std::size_t restore_and_start();

    // Launches the shared telemetry-publish worker: every interval, broadcasts
    // one VehicleState per currently registered vehicle (connected=false for
    // one that's linked-down, never silently skipped). Independent of
    // start()/start_all()/restore_and_start(); call in either order. Call
    // once; a second call is a logic error (jthread already joinable).
    void start_publishing(std::chrono::milliseconds interval = kDefaultPublishInterval);

    // Sends one VehicleInfo to exactly this client for every registered
    // vehicle whose System has been discovered (link never established yet is
    // skipped for that vehicle only, logged, not fatal to the rest). Wire this
    // to Transport::on_connect so a client that connects after boot still
    // learns about the fleet.
    void send_vehicle_info(Transport::ClientId client) const;

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
    // Empty disables persistence. Set once at construction, never mutated
    // after, so it's safe to read from any thread without vehicles_mutex_.
    std::filesystem::path persistence_path_;
    // Structural lock: guards managed_vehicles_'s shape only (insert, erase,
    // lookup). Does NOT guard any individual vehicle's busy/executor/
    // reconnect_worker; those live behind that vehicle's own
    // ManagedVehicle::mutex_ instead, so this lock is always held briefly.
    // Mutable so const query methods can lock. Lock order: this before a
    // vehicle's ManagedVehicle::mutex_, and that before any
    // VehicleConnection/TelemetryInfo/VehicleActions internal mutex; nothing
    // locks in the other direction, and no code ever holds two different
    // vehicles' mutex_ at once.
    mutable std::mutex vehicles_mutex_;
    std::map<std::string, std::shared_ptr<ManagedVehicle>> managed_vehicles_;
    // Shared across the whole fleet, unlike reconnect_worker (one per
    // vehicle): the publish tick is cheap in-memory work for every vehicle
    // back to back, not I/O-bound work that benefits from its own thread per
    // vehicle. Declared last (after managed_vehicles_) so it stops and joins
    // before managed_vehicles_ tears down; it reads managed_vehicles_ on
    // every tick.
    std::jthread publish_worker_;

    // Copies the shared_ptr for vehicle_id under vehicles_mutex_ (briefly)
    // and returns it; nullptr if unknown. The returned vehicle stays valid
    // for as long as the caller holds this copy, independent of whether it's
    // concurrently erased from managed_vehicles_ by another thread. Callers
    // must not retain the returned shared_ptr beyond the synchronous scope of
    // the call that obtained it (see ManagedVehicle's lifetime invariant).
    [[nodiscard]] std::shared_ptr<ManagedVehicle> find_shared_locked(
        const std::string& vehicle_id) const;

    // Reason-returning cores (nullopt = success) shared by the bool public
    // methods (which log) and the handle_* wire entry points (which put the
    // reason in the ack).
    // should_persist=false is only for load_persisted(), which is
    // reconstructing entries already on disk and must not rewrite the file
    // once per entry while doing so.
    [[nodiscard]] std::optional<std::string> add_vehicle_impl(const VehicleConfig& cfg,
                                                                bool should_persist = true);
    [[nodiscard]] std::optional<std::string> remove_vehicle_impl(const std::string& vehicle_id);

    // Writes every managed vehicle's config to persistence_path_ as YAML.
    // Caller must hold vehicles_mutex_. No-op if persistence_path_ is empty.
    // Write failure is logged, not thrown or rolled back: the in-memory
    // mutation that triggered this already succeeded.
    void persist_locked() const;

    // Body of publish_worker_, run on start_publishing()'s jthread.
    void run_publish_loop(std::chrono::milliseconds interval, std::stop_token stop_token);

    // Builds the CommandExecutor wired to broadcast its acks; shared by
    // add_vehicle_impl() and every path that restores a retired executor.
    // Captures &transport_ directly, never `this`: a CommandExecutor's worker
    // thread can outlive VehicleManager itself if some other shared_ptr<
    // ManagedVehicle> copy is (incorrectly) still held elsewhere when
    // VehicleManager is destroyed, and this callback must stay safe to invoke
    // even then. transport_'s referent is guaranteed by the whole system's
    // construction order (main.cpp builds Transport before VehicleManager) to
    // outlive it regardless.
    [[nodiscard]] std::unique_ptr<CommandExecutor> make_executor(ManagedVehicle& vehicle);

    // Clears vehicle's busy flag under vehicle->mutex_. Takes the shared_ptr
    // directly (not a vehicle_id to re-look-up): the caller already holds a
    // copy from before the busy window began, and using it here means this
    // works correctly even if the vehicle has since been erased from
    // managed_vehicles_ (e.g. remove_vehicle_impl's own success path).
    void clear_busy(const std::shared_ptr<ManagedVehicle>& vehicle);

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

    // Wraps a REJECTED ack into a WARNING Event and broadcasts it (gateway
    // rule 5: rejections are events a human should see). Caller checks
    // ack.status() first; takes no lock, same as broadcast_command_ack.
    void broadcast_rejection_event(const karshipta::v1::CommandAck& ack) const;

    // Wraps a link up/down transition into an Event and broadcasts it: INFO/
    // "LINK_CONNECTED" when connected is true, WARNING/"LINK_LOST" when
    // false. Called from run_reconnect_loop, which already runs unlocked, so
    // this takes no lock either, same as broadcast_command_ack.
    void broadcast_link_event(const std::string& vehicle_id, bool connected) const;

    // Wraps a WARNING Event with the given code/message and broadcasts it.
    // Shared by every mission-related rejection path (unknown/stopped
    // vehicle, MissionImporter failure, upload failure, download failure) so
    // each of them isn't hand-rolling the same Envelope/Event construction.
    // Takes no lock, same as broadcast_command_ack.
    void broadcast_mission_event(const std::string& vehicle_id, const std::string& code,
                                 const std::string& message) const;

    // Wraps a downloaded Mission into an Envelope.mission_download and
    // broadcasts it. Takes no lock, same as broadcast_command_ack.
    void broadcast_mission_download(const karshipta::v1::Mission& mission) const;

    // Wraps a successful AddVehicle/RemoveVehicle into an INFO Event and
    // broadcasts it, so every connected console (not just the one that made
    // the request) learns the fleet changed. Only called from
    // handle_add_vehicle()/handle_remove_vehicle() on their accepted path;
    // a rejection is answered by the VehicleConfigAck alone, since nothing
    // about the fleet actually changed for other consoles to learn about.
    // Takes no lock, same as broadcast_command_ack.
    void broadcast_fleet_event(const std::string& vehicle_id, bool added) const;

    // Body of vehicle.reconnect_worker, run on start()'s jthread. Connects,
    // requests the telemetry stream rate (PX4 forgets this across a link
    // drop, so it's re-requested on every reconnect, not just the first),
    // waits while connected, and on drop connects again, until stop_token is
    // cancelled. Touches only vehicle.connection, vehicle.telemetry, and
    // vehicle.config, so it never needs any lock at all (joining under
    // vehicle.mutex_ cannot deadlock).
    void run_reconnect_loop(ManagedVehicle& vehicle, std::stop_token stop_token);

    // No-op (returns true) if vehicle isn't armed; otherwise attempts to
    // disarm and returns whether it succeeded. Precondition: caller has set
    // vehicle.busy (the pointees it reads are stable while busy holds).
    // Blocking MAVSDK call; prefer calling it unlocked.
    [[nodiscard]] bool disarm_if_armed(const std::string& vehicle_id, ManagedVehicle& vehicle);

    // Requests reconnect_worker to stop and joins it; no-op if not running.
    // Must be called under vehicle.mutex_ (other threads read joinable() on
    // this same vehicle under that same mutex; std::jthread's joinable()/
    // join() are not safe to call concurrently on one object without it).
    void stop_worker(ManagedVehicle& vehicle);
};

#endif  // KARSHIPTA_GATEWAY_VEHICLE_MANAGER_H
