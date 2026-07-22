//
// Created by amir abkhoshk on 13/07/2026.
//

#ifndef KARSHIPTA_GATEWAY_WARD_MANAGER_H
#define KARSHIPTA_GATEWAY_WARD_MANAGER_H
#include <chrono>
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
#include <karshipta/v1/envelope.pb.h>
#include <karshipta/v1/fleet.pb.h>
#include <ward_connection.h>
#include <ward_actions.h>
#include <ward_mission.h>
#include <mission_importer.h>
#include <telemetry.h>
#include <command_executor.h>
#include <transport.h>

struct WardConfig {
    std::string ward_id;
    std::string connection_url;
    // MAVLink system id this ward broadcasts. 0 means "bind to the first
    // autopilot on the endpoint" (fleet.proto AddWard.mavlink_system_id),
    // which is only safe when wards connect one at a time.
    unsigned int system_id;
    karshipta::v1::WardClass ward_class = karshipta::v1::WARD_CLASS_UNSPECIFIED;
    // Display name; empty means the console should fall back to ward_id
    // (matches fleet.proto AddWard.name's comment).
    std::string name;
};

// One ward's full object graph plus its own transition state. Held by
// WardManager as a shared_ptr (see managed_wards_): a lookup copies the
// shared_ptr under the structural lock and releases it immediately, so one
// ward's slow teardown never blocks a lookup for any other ward.
//
// Lifetime invariant: nothing may retain a copy of this shared_ptr beyond the
// synchronous scope of the call that obtained it (no background thread, no
// member field elsewhere). Every current call site follows this already
// (dispatch_command, the handle_mission_*/dispatch_mission_upload_and_start
// methods, run_publish_loop's per-tick snapshot, and ~WardManager()'s own
// snapshot all let their copy go out of scope before returning/sleeping).
// This is what lets ~WardManager() bound this object's true lifetime to
// managed_wards_'s own destruction: relax this invariant and a lingering
// copy could keep a ward (and its CommandExecutor's worker thread) alive
// past WardManager's own teardown.
struct ManagedWard {
    // The config this ward was built from (persistence and WardInfo both
    // read this instead of asking WardConnection, so neither needs a live
    // connection to answer). config.system_id replaces a standalone field.
    // Set once at construction, never reassigned: safe to read without
    // mutex_.
    WardConfig config;
    // Pointers below are set once at construction and never reassigned; the
    // pointee's own thread-safety (WardConnection/TelemetryInfo/etc. each
    // guard their own internal state) is what protects concurrent use, not
    // this struct. Only executor is ever reassigned after construction,
    // which is why it (along with busy, pending_auto_start_mission_id, and
    // reconnect_worker) lives under mutex_ below instead of alongside these.
    std::unique_ptr<WardConnection> connection;
    std::unique_ptr<TelemetryInfo> telemetry;
    std::unique_ptr<WardActions> actions;
    // Owns mission upload/download/start/pause/progress for this ward
    // (BRIEF.md M5). Built before executor (which holds a reference to it)
    // and after actions/telemetry, matching the same by-reference build
    // order as the rest of this quartet.
    std::unique_ptr<WardMission> mission;
    // Converts a customer-uploaded QGC/WPL mission file into a Mission that
    // can be handed to `mission`'s enqueue_upload(). Only needs `connection`,
    // so its position relative to `mission`/`executor` doesn't matter for
    // build order, unlike the rest of this quartet.
    std::unique_ptr<MissionImporter> mission_importer;

    // Guards executor/busy/pending_auto_start_mission_id/reconnect_worker
    // below: this ward's own transition state, independent of every other
    // ward's. Lock order: WardManager::wards_mutex_ (structural:
    // managed_wards_ itself) before this, never the other way; nothing ever
    // holds two different wards' mutex_ at once. Mutable so const query
    // methods (is_started(), etc.) can lock a const ManagedWard&.
    mutable std::mutex mutex_;
    // Null while the ward is stopped or a transition is quiescing it;
    // dispatch_command() rejects commands in that window. Guarded by mutex_.
    std::unique_ptr<CommandExecutor> executor;
    // True while a stop()/force_stop()/remove_ward() transition is in
    // flight, including its unlocked phases. While set: no other transition
    // on THIS ward may begin, remove_ward_impl() may not erase this entry
    // from managed_wards_, and only the thread that set it may mutate
    // executor/reconnect_worker/pending_auto_start_mission_id. Guarded by
    // mutex_.
    bool busy = false;
    // Set by dispatch_mission_upload_and_start() alongside the enqueued
    // upload; cleared by run_publish_loop() the moment that mission_id's
    // upload result is drained. A match on a successful result there is
    // what turns into an auto-issued StartMissionCommand (see pending_start
    // in run_publish_loop) - enqueue_upload() is non-blocking, so a fleet
    // mission assignment cannot safely dispatch StartMissionCommand right
    // after calling it; this flag defers the start until the upload this
    // gateway process itself just kicked off is actually known to have
    // landed on the ward. Guarded by mutex_.
    std::optional<std::string> pending_auto_start_mission_id;
    // Last: its loop calls connection->connect_with_retry()/is_connected() on
    // every iteration, so it must stop and join before connection (and the
    // other members above it) are torn down. Empty (not joinable) until
    // WardManager::start() launches it. Guarded by mutex_ (every
    // joinable()/request_stop()/join() call site takes mutex_ first;
    // std::jthread's joinable()/join() are not safe to call concurrently on
    // the same object from two threads without this).
    std::jthread reconnect_worker;
};

// Snapshot of one ward's start/connect state, returned by
// WardManager::list_status(). started == reconnect_worker running (does
// not imply connected); connected == currently linked to its autopilot.
struct WardStatus {
    std::string ward_id;
    bool started;
    bool connected;
};

// Owns the fleet: one ManagedWard per configured ward, all sharing one
// Mavsdk core. All public methods are thread-safe. Locking is two-level:
// wards_mutex_ is structural only (managed_wards_'s map shape: insert,
// erase, lookup), always fast; each ward's own transition state (busy,
// executor, reconnect_worker) lives behind that ward's own ManagedWard::
// mutex_, so one ward's slow teardown or reconnect-join never blocks any
// other ward's calls. No raw pointer or reference into the internal map
// ever escapes wards_mutex_; find_shared_locked() returns a shared_ptr
// copy instead, which is what keeps a ward alive for the duration of an
// unlocked call even if it's concurrently removed from the map.
class WardManager {
public:
    // Default publish interval for start_publishing(): BRIEF.md M2's ~5Hz
    // per-ward WardState target.
    static constexpr std::chrono::milliseconds kDefaultPublishInterval{200};

    // persistence_path: where add_ward_impl()/remove_ward_impl() persist
    // the fleet after every successful mutation, and where load_persisted()
    // reads from. An empty path (the default) disables persistence entirely,
    // so tests stay disk-I/O-free unless they opt in.
    WardManager(std::shared_ptr<mavsdk::Mavsdk> mavsdk, Transport& tp,
                   std::filesystem::path persistence_path = {});

    // Builds the shared Mavsdk core itself (WardConnection::create_shared_core())
    // so callers never need to name WardConnection just to construct a
    // WardManager. The one entry point main.cpp (or any future consumer)
    // should use.
    static std::unique_ptr<WardManager> create(Transport& tp,
                                                    std::filesystem::path persistence_path = {});
    // Snapshots every currently registered ward (structural lock, once),
    // then waits for each one's own busy flag to clear before letting it go:
    // force_stop() in particular spends up to kForceStopLandingTimeoutS
    // unlocked, holding a shared_ptr<ManagedWard>, and without this wait a
    // concurrent destructor could tear that ward down out from under it.
    // No shared condition_variable spans this (busy now lives in N per-
    // ward mutexes, which a single condition_variable's one-mutex contract
    // can't wait across); each ward is polled on its own mutex_ instead,
    // acceptable because this runs once, at shutdown, never on a hot path.
    // Once every snapshot ward's busy flag has cleared, the local
    // snapshot's shared_ptrs go out of scope and managed_wards_ destroys
    // each ManagedWard's members in reverse declaration order:
    // reconnect_worker stops and joins first, then the executor (rejecting
    // whatever it still queued), then mission/actions/telemetry/connection.
    // This does NOT proactively RTL wards that were never told to stop;
    // graceful shutdown of a still-flying, never-force_stop()ped fleet needs
    // main.cpp to call force_stop_all() before dropping the WardManager,
    // which isn't wired up yet (no signal handling exists in main.cpp today).
    // Callers must not invoke NEW methods concurrently with destruction;
    // that's a normal C++ lifetime rule this class does not attempt to lift.
    // Relies on the ManagedWard lifetime invariant documented on that
    // struct: nothing outside this snapshot and managed_wards_ itself may
    // be holding a shared_ptr<ManagedWard> once this snapshot is taken.
    ~WardManager();

    WardManager(const WardManager&) = delete;
    WardManager& operator=(const WardManager&) = delete;
    WardManager(WardManager&&) = delete;
    WardManager& operator=(WardManager&&) = delete;

    // Builds the WardConnection/TelemetryInfo/WardActions/CommandExecutor
    // graph for cfg and registers it under cfg.ward_id. Does not connect to
    // the ward; construction only. Returns false, adding nothing, if
    // ward_id is empty or already registered, or if a nonzero system_id is
    // already bound to a different ward_id. Throws std::invalid_argument
    // for an empty connection_url (from WardConnection).
    bool add_ward(const WardConfig& cfg);

    // Every currently registered ward_id (map key order, i.e. sorted).
    // Just the ids, no start/connect state; use list_status() for that.
    [[nodiscard]] std::vector<std::string> list_ward_ids() const;

    // True if ward_id is registered here, MAVLink-connected or not. Read-only
    // query for a sibling ingestion path (HeraldWardManager) to check for a
    // ward_id collision before registering a non-MAVLink ward under the same
    // id; this class never calls it itself.
    [[nodiscard]] bool has_ward(const std::string& ward_id) const;

    // Routes command to the CommandExecutor of the ward named
    // command.ward_id(). Synthesizes and broadcasts a REJECTED CommandAck
    // (gateway rule 5: no silent drop) if that ward is unknown, stopped,
    // or mid-transition. Not const: briefly marks the ward busy so the
    // executor can be enqueued into without holding wards_mutex_ for the
    // ACCEPTED ack's broadcast (a blocking socket write that must not stall
    // every other ward's manager calls behind one slow client).
    void dispatch_command(const karshipta::v1::Command& command);

    // Routes mission to the WardMission of the ward named
    // mission.ward_id() (Envelope.mission_upload, console-editor-authored,
    // not a Command). Broadcasts a WARNING Event (gateway rule 5) if that
    // ward is unknown or stopped; the upload's own success/failure
    // surfaces later, via the publish tick polling take_upload_result().
    void handle_mission_upload(const karshipta::v1::Mission& mission);

    // Fleet-mission-assignment counterpart to handle_mission_upload(): same
    // validation and rejection-event behavior (unknown/stopped/busy ward),
    // but additionally arms an auto-start so that once this specific
    // upload's result comes back successful (polled on the next publish
    // tick, same as any other upload), a StartMissionCommand is issued for
    // this ward automatically - the gateway-side half of "upload an
    // independent copy to each selected ward and start them together"
    // (fleet.proto FleetMissionAssignment). Returns the same rejection
    // reason handle_mission_upload() would broadcast, so FleetManager can
    // also learn per-ward outcomes it might otherwise have to poll for.
    [[nodiscard]] std::optional<std::string> dispatch_mission_upload_and_start(
        const karshipta::v1::Mission& mission);

    // Converts upload via the ward's MissionImporter, then routes the
    // result through handle_mission_upload() exactly like a console-editor
    // mission. Broadcasts a WARNING Event immediately if the ward is
    // unknown/stopped or the conversion itself fails; MissionImporter::import()
    // is local parsing, not a MAVLink round trip, so this runs synchronously
    // rather than being queued.
    void handle_mission_file_upload(const karshipta::v1::MissionFileUpload& upload);

    // Queues a mission-download request on the ward named
    // request.ward_id(). Broadcasts a WARNING Event if that ward is
    // unknown or stopped; the result (a mission_download Envelope, or a
    // WARNING Event on failure) surfaces later, via the publish tick polling
    // take_download_result().
    void handle_mission_download_request(const karshipta::v1::MissionDownloadRequest& request);

    // Rejects an upstream envelope from a connection Transport marked
    // ClientRole::kViewer (gateway issue #20: read-only viewer mode). Never
    // forwards the payload; answers instead with the same reasoned
    // ack/event a normal precondition failure would produce (a REJECTED
    // CommandAck/WardConfigAck for kinds that carry one, a WARNING Event
    // for the mission-upload/download family, which has none), always with
    // message "read-only session". `client` is only used for the log line;
    // callers still route the switch on the envelope's payload kind, this
    // just supplies the read-only-specific rejection body. Call this
    // instead of dispatch_command()/handle_add_ward()/etc., not after
    // them: a viewer's envelope must never reach MAVSDK.
    void reject_viewer_envelope(Transport::ClientId client, const karshipta::v1::Envelope& envelope);

    // Launches reconnect_worker for the ward with this id, recreating its
    // CommandExecutor if a previous stop() retired it. Returns false if
    // ward_id is unknown, mid-transition, or already running.
    bool start(const std::string& ward_id);

    // Calls start() for every currently registered ward.
    void start_all();

    // Reads persistence_path (set at construction), registering each entry via
    // add_ward_impl() without re-persisting what's already on disk. Returns
    // 0 without error if no path was set or the file doesn't exist (first
    // run). A malformed entry is logged and skipped; the rest still load.
    std::size_t load_persisted();

    // load_persisted() followed by start_all(): the crash-recovery entry
    // point. Call once at boot, before serving any client, so a ward that
    // was mid-flight when the gateway last exited starts reconnecting
    // immediately instead of being forgotten. Returns how many wards were
    // loaded (not how many started; start() failures are logged individually).
    std::size_t restore_and_start();

    // Launches the shared telemetry-publish worker: every interval, broadcasts
    // one WardState per currently registered ward (connected=false for
    // one that's linked-down, never silently skipped). Independent of
    // start()/start_all()/restore_and_start(); call in either order. Call
    // once; a second call is a logic error (jthread already joinable).
    void start_publishing(std::chrono::milliseconds interval = kDefaultPublishInterval);

    // Sends one WardInfo to exactly this client for every registered
    // ward whose System has been discovered (link never established yet is
    // skipped for that ward only, logged, not fatal to the rest). Wire this
    // to Transport::on_connect so a client that connects after boot still
    // learns about the fleet.
    void send_ward_info(Transport::ClientId client) const;

    // Takes the ward offline safely: rejects if unknown, mid-transition,
    // not running, airborne, or link-down-after-discovery (telemetry cannot
    // be trusted, so ground state is unknown; use force_stop). Otherwise
    // retires the CommandExecutor first so no command can race the shutdown,
    // re-checks the ground state, disarms if armed (rejecting and restoring
    // the executor when the disarm fails), then stops reconnect_worker. The
    // ward stays registered.
    bool stop(const std::string& ward_id);

    // Calls stop() for every currently registered ward. Sequential; each
    // ward's rejection reasons apply individually.
    void stop_all();

    // Operator override for a ward stop() refuses to touch: retires the
    // executor, commands return_to_launch() when airborne or link-down, then
    // SUPERVISES the flight home, keeping reconnect_worker running and
    // polling until the ward is connected, landed, and disarmed (link-down
    // never reads as landed). Only then disarms best-effort and stops the
    // worker. On timeout (kForceStopLandingTimeoutS) the executor is restored
    // and monitoring is left running: a flying ward is never abandoned.
    // Returns false if unknown, mid-transition, or the supervision timed out.
    bool force_stop(const std::string& ward_id);

    // Calls force_stop() for every currently registered ward.
    void force_stop_all();

    // True if start() is running for ward_id and hasn't been stop()ped.
    // Does not imply connected. False (not an error) if ward_id is
    // unknown.
    [[nodiscard]] bool is_started(const std::string& ward_id) const;

    // True if ward_id is currently linked to its autopilot. False for
    // both "never started" and "started but link down"; false (not an
    // error) if ward_id is unknown.
    [[nodiscard]] bool is_connected(const std::string& ward_id) const;

    // Snapshot of every registered ward's start/connect state.
    [[nodiscard]] std::vector<WardStatus> list_status() const;

    // Applies stop()'s ground-safety rules (reject airborne or link-down,
    // disarm if armed), then erases the ward entirely. Returns false,
    // removing nothing, if any guard rejects.
    bool remove_ward(const std::string& ward_id);

    // Calls remove_ward() for every currently registered ward; rejected
    // wards stay in place. Check list_status() afterward for survivors.
    void remove_all();

    // Wire-level entry points: translate an AddWard/RemoveWard request
    // from a console client and produce the WardConfigAck to send back.
    // Never throws on bad input; every rejection carries a reason.
    karshipta::v1::WardConfigAck handle_add_ward(const karshipta::v1::AddWard& request);
    karshipta::v1::WardConfigAck handle_remove_ward(const karshipta::v1::RemoveWard& request);

private:
    std::shared_ptr<mavsdk::Mavsdk> mavsdk_;
    Transport& transport_;
    // Empty disables persistence. Set once at construction, never mutated
    // after, so it's safe to read from any thread without wards_mutex_.
    std::filesystem::path persistence_path_;
    // Structural lock: guards managed_wards_'s shape only (insert, erase,
    // lookup). Does NOT guard any individual ward's busy/executor/
    // reconnect_worker/pending_auto_start_mission_id; those live behind that
    // ward's own ManagedWard::mutex_ instead, so this lock is always held
    // briefly. Mutable so const query methods can lock. Lock order: this
    // before a ward's ManagedWard::mutex_, and that before any
    // WardConnection/TelemetryInfo/WardActions internal mutex; nothing
    // locks in the other direction, and no code ever holds two different
    // wards' mutex_ at once.
    mutable std::mutex wards_mutex_;
    std::map<std::string, std::shared_ptr<ManagedWard>> managed_wards_;
    // Shared across the whole fleet, unlike reconnect_worker (one per
    // ward): the publish tick is cheap in-memory work for every ward
    // back to back, not I/O-bound work that benefits from its own thread per
    // ward. Declared last (after managed_wards_) so it stops and joins
    // before managed_wards_ tears down; it reads managed_wards_ on
    // every tick.
    std::jthread publish_worker_;

    // Copies the shared_ptr for ward_id under wards_mutex_ (briefly) and
    // returns it; nullptr if unknown. The returned ward stays valid for as
    // long as the caller holds this copy, independent of whether it's
    // concurrently erased from managed_wards_ by another thread. Callers
    // must not retain the returned shared_ptr beyond the synchronous scope
    // of the call that obtained it (see ManagedWard's lifetime invariant).
    [[nodiscard]] std::shared_ptr<ManagedWard> find_shared_locked(
        const std::string& ward_id) const;

    // Reason-returning cores (nullopt = success) shared by the bool public
    // methods (which log) and the handle_* wire entry points (which put the
    // reason in the ack).
    // should_persist=false is only for load_persisted(), which is
    // reconstructing entries already on disk and must not rewrite the file
    // once per entry while doing so.
    [[nodiscard]] std::optional<std::string> add_ward_impl(const WardConfig& cfg,
                                                                bool should_persist = true);
    [[nodiscard]] std::optional<std::string> remove_ward_impl(const std::string& ward_id);

    // Writes every managed ward's config to persistence_path_ as YAML.
    // Caller must hold wards_mutex_. No-op if persistence_path_ is empty.
    // Write failure is logged, not thrown or rolled back: the in-memory
    // mutation that triggered this already succeeded.
    void persist_locked() const;

    // Body of publish_worker_, run on start_publishing()'s jthread.
    void run_publish_loop(std::chrono::milliseconds interval, std::stop_token stop_token);

    // Builds the CommandExecutor wired to broadcast its acks; shared by
    // add_ward_impl() and every path that restores a retired executor.
    // Captures &transport_ directly, never `this`: a CommandExecutor's
    // worker thread can outlive WardManager itself if some other
    // shared_ptr<ManagedWard> copy is (incorrectly) still held elsewhere
    // when WardManager is destroyed, and this callback must stay safe to
    // invoke even then. transport_'s referent is guaranteed by the whole
    // system's construction order (main.cpp builds Transport before
    // WardManager) to outlive it regardless.
    [[nodiscard]] std::unique_ptr<CommandExecutor> make_executor(ManagedWard& ward);

    // Clears ward's busy flag under ward->mutex_. Takes the shared_ptr
    // directly (not a ward_id to re-look-up): the caller already holds a
    // copy from before the busy window began, and using it here means this
    // works correctly even if the ward has since been erased from
    // managed_wards_ (e.g. remove_ward_impl's own success path).
    void clear_busy(const std::shared_ptr<ManagedWard>& ward);

    // Shared epilogue for stop() and remove_ward_impl(), called once the
    // ward's executor has already been retired (so no command can race
    // this check) and ward.busy == true (so nothing else can touch it).
    // Re-checks link + ground state, disarms if armed, and returns nullopt on
    // success. On any failure, restores the executor (so the ward remains
    // usable) and returns a rejection reason instead. Does not touch
    // reconnect_worker, busy, or the map; callers finish those themselves.
    [[nodiscard]] std::optional<std::string> verify_grounded_and_disarm(
        const std::string& ward_id, ManagedWard& ward);

    // Wraps ack in an Envelope and broadcasts it. Takes no lock; safe from
    // executor worker threads and from under wards_mutex_.
    void broadcast_command_ack(const karshipta::v1::CommandAck& ack) const;

    // Wraps a REJECTED ack into a WARNING Event and broadcasts it (gateway
    // rule 5: rejections are events a human should see). Caller checks
    // ack.status() first; takes no lock, same as broadcast_command_ack.
    void broadcast_rejection_event(const karshipta::v1::CommandAck& ack) const;

    // Wraps a link up/down transition into an Event and broadcasts it: INFO/
    // "LINK_CONNECTED" when connected is true, WARNING/"LINK_LOST" when
    // false. Called from run_reconnect_loop, which already runs unlocked, so
    // this takes no lock either, same as broadcast_command_ack.
    void broadcast_link_event(const std::string& ward_id, bool connected) const;

    // Wraps a WARNING Event with the given code/message and broadcasts it.
    // Shared by every mission-related rejection path (unknown/stopped
    // ward, MissionImporter failure, upload failure, download failure) so
    // each of them isn't hand-rolling the same Envelope/Event construction.
    // Takes no lock, same as broadcast_command_ack.
    void broadcast_mission_event(const std::string& ward_id, const std::string& code,
                                 const std::string& message) const;

    // Wraps a downloaded Mission into an Envelope.mission_download and
    // broadcasts it. Takes no lock, same as broadcast_command_ack.
    void broadcast_mission_download(const karshipta::v1::Mission& mission) const;

    // Wraps a successful AddWard/RemoveWard into an INFO Event and
    // broadcasts it, so every connected console (not just the one that made
    // the request) learns the fleet changed. Only called from
    // handle_add_ward()/handle_remove_ward() on their accepted path;
    // a rejection is answered by the WardConfigAck alone, since nothing
    // about the fleet actually changed for other consoles to learn about.
    // Takes no lock, same as broadcast_command_ack.
    void broadcast_fleet_event(const std::string& ward_id, bool added) const;

    // Body of ward.reconnect_worker, run on start()'s jthread. Connects,
    // requests the telemetry stream rate (PX4 forgets this across a link
    // drop, so it's re-requested on every reconnect, not just the first),
    // waits while connected, and on drop connects again, until stop_token is
    // cancelled. Touches only ward.connection, ward.telemetry, and
    // ward.config, so it never needs any lock at all (joining under
    // ward.mutex_ cannot deadlock).
    void run_reconnect_loop(ManagedWard& ward, std::stop_token stop_token);

    // No-op (returns true) if ward isn't armed; otherwise attempts to
    // disarm and returns whether it succeeded. Precondition: caller has set
    // ward.busy (the pointees it reads are stable while busy holds).
    // Blocking MAVSDK call; prefer calling it unlocked.
    [[nodiscard]] bool disarm_if_armed(const std::string& ward_id, ManagedWard& ward);

    // Requests reconnect_worker to stop and joins it; no-op if not running.
    // Must be called under ward.mutex_ (other threads read joinable() on
    // this same ward under that same mutex; std::jthread's joinable()/
    // join() are not safe to call concurrently on one object without it).
    void stop_worker(ManagedWard& ward);
};

#endif  // KARSHIPTA_GATEWAY_WARD_MANAGER_H
