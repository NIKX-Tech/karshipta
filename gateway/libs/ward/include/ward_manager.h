//
// Created by amir abkhoshk on 13/07/2026.
//

#ifndef KARSHIPTA_GATEWAY_WARD_MANAGER_H
#define KARSHIPTA_GATEWAY_WARD_MANAGER_H
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

struct ManagedWard {
    // The config this ward was built from (persistence and WardInfo both
    // read this instead of asking WardConnection, so neither needs a live
    // connection to answer). config.system_id replaces a standalone field.
    WardConfig config;
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
    // Null while the ward is stopped or a transition is quiescing it;
    // dispatch_command() rejects commands in that window. Written only under
    // WardManager::wards_mutex_.
    std::unique_ptr<CommandExecutor> executor;
    // True while a stop()/force_stop()/remove_ward() transition is in
    // flight, including its unlocked phases. While set: no other transition
    // may begin, remove_ward() may not erase this entry, and only the
    // thread that set it may mutate this struct. Written only under
    // WardManager::wards_mutex_.
    bool busy = false;
    // Last: its loop calls connection->connect_with_retry()/is_connected() on
    // every iteration, so it must stop and join before connection (and the
    // other members above it) are torn down. Empty (not joinable) until
    // WardManager::start() launches it.
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
// Mavsdk core. All public methods are thread-safe (wards_mutex_); no
// pointer or reference into the internal map ever escapes the lock.
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
    // Blocks until no ward is mid-transition (busy) before letting
    // managed_wards_ destruct: force_stop() in particular spends up to
    // kForceStopLandingTimeoutS unlocked, holding a raw ManagedWard*, and
    // without this wait a concurrent destructor could free that ward out
    // from under it. Once every transition has quiesced, managed_wards_
    // destroys each ManagedWard's members in reverse declaration order:
    // reconnect_worker stops and joins first, then the executor (rejecting
    // whatever it still queued), then mission/actions/telemetry/connection.
    // This does
    // NOT proactively RTL wards that were never told to stop; graceful
    // shutdown of a still-flying, never-force_stop()ped fleet needs main.cpp
    // to call force_stop_all() before dropping the WardManager, which
    // isn't wired up yet (no signal handling exists in main.cpp today).
    // Callers must not invoke NEW methods concurrently with destruction;
    // that's a normal C++ lifetime rule this class does not attempt to lift.
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
    // Guards managed_wards_ (structure and every element's executor/busy/
    // reconnect_worker members). Mutable so const query methods can lock.
    // Lock order: wards_mutex_ before any WardConnection/TelemetryInfo/
    // WardActions internal mutex; nothing locks in the other direction.
    mutable std::mutex wards_mutex_;
    // Notified whenever a ward's busy flag clears (clear_busy_and_notify);
    // ~WardManager() waits on this until no ward is busy.
    mutable std::condition_variable busy_cv_;
    std::map<std::string, ManagedWard> managed_wards_;
    // Shared across the whole fleet, unlike reconnect_worker (one per
    // ward): the publish tick is cheap in-memory work for every ward
    // back to back, not I/O-bound work that benefits from its own thread per
    // ward. Declared last (after managed_wards_) so it stops and joins
    // before managed_wards_ tears down; it reads managed_wards_ on
    // every tick.
    std::jthread publish_worker_;

    // Lookup while holding wards_mutex_. The returned pointer is only
    // valid under the lock, or between lock windows of a transition that has
    // set busy on that ward (busy blocks erase).
    [[nodiscard]] ManagedWard* find_locked(const std::string& ward_id);
    [[nodiscard]] const ManagedWard* find_locked(const std::string& ward_id) const;

    // Reason-returning cores (nullopt = success) shared by the bool public
    // methods (which log) and the handle_* wire entry points (which put the
    // reason in the ack). Both take wards_mutex_ themselves.
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
    [[nodiscard]] std::unique_ptr<CommandExecutor> make_executor(ManagedWard& ward);

    // Clears ward_id's busy flag (no-op if the ward was erased in the
    // meantime, e.g. by remove_ward_impl's own success path) and always
    // notifies busy_cv_, so ~WardManager()'s wait re-evaluates even when
    // the ward that just finished no longer exists to be found.
    void clear_busy_and_notify(const std::string& ward_id);

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
    // ward.config, so it never needs wards_mutex_ (joining under the
    // lock cannot deadlock).
    void run_reconnect_loop(ManagedWard& ward, std::stop_token stop_token);

    // No-op (returns true) if ward isn't armed; otherwise attempts to
    // disarm and returns whether it succeeded. Precondition: caller holds
    // wards_mutex_ OR has set ward.busy (the pointees it reads are
    // stable either way). Blocking MAVSDK call; prefer calling it unlocked.
    [[nodiscard]] bool disarm_if_armed(const std::string& ward_id, ManagedWard& ward);

    // Requests reconnect_worker to stop and joins it; no-op if not running.
    // Must be called under wards_mutex_ (other threads read joinable()).
    void stop_worker(ManagedWard& ward);
};

#endif  // KARSHIPTA_GATEWAY_WARD_MANAGER_H
