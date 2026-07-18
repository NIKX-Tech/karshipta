#ifndef KARSHIPTA_GATEWAY_VEHICLE_MISSION_H
#define KARSHIPTA_GATEWAY_VEHICLE_MISSION_H

#include <karshipta/v1/command.pb.h>
#include <mavsdk/plugins/mission/mission.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "vehicle_connection.h"

// Wraps MAVSDK's Mission plugin against exactly one connected vehicle.
// Scoped to what proto/karshipta/v1/command.proto's Mission/MissionProgress
// can carry, plus the repeat_count looping and RTL-item translation the
// Mission plugin has no native support for (BRIEF.md M5). Deliberately has no
// reference to VehicleActions or CommandExecutor: it only ever exposes state
// for VehicleManager to poll (get_progress(), take_pending_return_to_launch(),
// take_upload_result(), take_download_result()) from its existing shared
// publish tick, the same way it already reaches across a ManagedVehicle's
// other owned members. This keeps VehicleActions' single-caller-thread
// invariant intact (see gateway/docs/vehicle-mission.md) and avoids a
// construction-order cycle: CommandExecutor is built after this class and
// needs a reference to it (for StartMissionCommand/PauseMissionCommand), so
// this class cannot also hold a reference back to CommandExecutor.
class VehicleMission {
   public:
    // Result of one enqueue_upload() job, read once via take_upload_result().
    struct UploadResult {
        std::string mission_id;
        mavsdk::Mission::Result result;
    };

    // Result of one enqueue_download() job, read once via
    // take_download_result(). mission has no value on failure; message is
    // the failure reason then, empty on success.
    struct DownloadResult {
        std::optional<karshipta::v1::Mission> mission;
        std::string message;
    };

    // Binds this wrapper to a connection; does not create the Mission plugin
    // yet (that happens lazily in ensure_mission() on first use). Starts the
    // job worker thread immediately (cheap: it just waits on an empty
    // queue), mirroring CommandExecutor's constructor.
    explicit VehicleMission(VehicleConnection& connection);
    // Unsubscribes the mission-progress callback registered by
    // ensure_mission() (it captures `this`), then stops and joins worker_
    // (declared last, so it stops before mission_/progress_handle_ above it
    // are torn down). Jobs still queued at shutdown are dropped, not run;
    // nothing consumes take_upload_result()/take_download_result() for them,
    // same tradeoff CommandExecutor makes explicit for leftover commands.
    ~VehicleMission();

    VehicleMission(const VehicleMission&) = delete;
    VehicleMission& operator=(const VehicleMission&) = delete;
    VehicleMission(VehicleMission&&) = delete;
    VehicleMission& operator=(VehicleMission&&) = delete;

    // Human-readable text for a MAVSDK result, the string that Event carries
    // on a rejected upload/start/pause.
    static std::string result_name(mavsdk::Mission::Result result);

    // Implements the mission_upload payload (Envelope.mission_upload, not a
    // Command). Non-blocking: queues mission for upload_worker_, which
    // translates it into a mavsdk::Mission::MissionPlan and calls
    // upload_mission() (blocking, hence its own thread rather than running on
    // whatever thread received the frame). Resets this vehicle's repeat-pass
    // bookkeeping to mission.repeat_count() and clears any prior
    // interrupted()/pending-RTL state once the upload actually runs. If the
    // last item in mission.items() is MISSION_ACTION_RTL, that item is
    // stripped before upload (MAVSDK's Mission::MissionItem::VehicleAction has
    // no RTL member) and remembered: handle_progress() sets the
    // pending-return-to-launch flag once the final pass completes instead of
    // relying on the autopilot's own end-of-mission behavior, which this
    // class unconditionally suppresses (set_return_to_launch_after_mission(
    // false)) before every upload.
    void enqueue_upload(karshipta::v1::Mission mission);
    // Non-blocking: queues a request to download the mission currently
    // uploaded to the vehicle. worker_ calls Mission::download_mission()
    // (blocking, a real MAVLink transfer) and translates the result back
    // into a proto Mission. Read later via take_download_result(). Does not
    // touch repeat-pass/interrupted/pending-RTL state; a download is a
    // read-only, independent operation. Note: an RTL item stripped by
    // translate() at upload time was never an actual on-vehicle mission
    // item, so a downloaded mission never reconstructs one, even if this
    // same VehicleMission uploaded it with ends_with_return_to_launch_ set.
    void enqueue_download();
    // Implements StartMissionCommand. A mission must already be uploaded.
    // Blocking but fast (no data transfer), so CommandExecutor's worker calls
    // this directly rather than going through the upload queue.
    [[nodiscard]] mavsdk::Mission::Result start() const;
    // Implements PauseMissionCommand: HOLD mode, not a stop. Leaves
    // repeat-pass bookkeeping untouched so a later start() resumes the same
    // pass rather than restarting the count.
    [[nodiscard]] mavsdk::Mission::Result pause() const;
    // Async counterpart to start() (gateway issue #69). MAVSDK documents
    // start_mission()/pause_mission() as blocking with no cancel
    // counterpart, unlike upload_mission()/download_mission(); the fix here
    // is to never block inside them at all rather than add a way to
    // interrupt them after the fact. Fires start_mission_async() and
    // returns immediately; `callback` runs on whatever thread MAVSDK's own
    // I/O invokes it from, not the caller's, and must not assume anything
    // about this object's lifetime (mirrors handle_progress()'s existing
    // start_mission_async() re-trigger, which captures nothing risky for
    // the same reason). Same immediate-NoSystem behavior as start() when no
    // mission plugin exists yet.
    void start_async(mavsdk::Mission::ResultCallback callback) const;
    // Async counterpart to pause(). Same non-blocking rationale and
    // callback-lifetime caveat as start_async(); still HOLD mode, not a
    // stop, same as pause().
    void pause_async(mavsdk::Mission::ResultCallback callback) const;

    // Non-blocking cached read of the latest subscribe_mission_progress()
    // update, translated to the wire shape. Meant to be polled by
    // VehicleManager's existing shared publish loop the same way it already
    // polls TelemetryInfo. Default-constructed (mission_id empty,
    // finished=false) if nothing has been uploaded yet. finished is true only
    // once every repeat pass has completed; it is never set true by an
    // interrupt.
    [[nodiscard]] karshipta::v1::MissionProgress get_progress() const;

    // Called by whatever executes an interrupting command (land, RTL, goto,
    // force-disarm) against this vehicle while a mission may be active.
    // Marks the current mission interrupted, which stops handle_progress()
    // from re-triggering start_mission_async() for any remaining pass, from
    // ever setting the pending-return-to-launch flag for it, and from ever
    // reporting finished=true for it. Returns false (no-op) if no mission was
    // active, so the caller knows whether an interrupted-mission Event is
    // actually worth broadcasting.
    bool notify_interrupted();

    // Test-and-clear: true at most once per finished mission, the moment the
    // final pass of a mission whose last item was MISSION_ACTION_RTL
    // completes. VehicleManager's publish tick checks this for every vehicle
    // and, when true, enqueues a synthetic ReturnToLaunchCommand on that
    // vehicle's CommandExecutor, exactly as if the console had sent one.
    [[nodiscard]] bool take_pending_return_to_launch();

    // Test-and-clear: the outcome of the most recently finished
    // enqueue_upload() job, or nullopt if none has finished since the last
    // call. VehicleManager's publish tick checks this for every vehicle and
    // broadcasts an Event on a non-Success result (gateway rule 5); a
    // Success result needs no action, since the next MissionProgress frame
    // already shows the upload took effect.
    [[nodiscard]] std::optional<UploadResult> take_upload_result();

    // Test-and-clear: the outcome of the most recently finished
    // enqueue_download() job, or nullopt if none has finished since the last
    // call. VehicleManager's publish tick checks this for every vehicle and
    // broadcasts either the downloaded Mission (Envelope.mission_download) or
    // an Event on failure.
    [[nodiscard]] std::optional<DownloadResult> take_download_result();

   private:
    // The connection this mission wrapper sends commands through. Must
    // outlive this object.
    VehicleConnection& connection_;

    // Owned copy of the Mavsdk core, grabbed in ensure_mission(). Keeps the
    // core alive for as long as `mission_` exists, independent of
    // `connection_`'s own lifetime.
    mutable std::shared_ptr<mavsdk::Mavsdk> mavsdk_keepalive_;
    // The Mission plugin bound to the connected System. Null until
    // ensure_mission() lazily constructs it on first use.
    mutable std::unique_ptr<mavsdk::Mission> mission_;
    // Handle for the long-lived progress subscription registered by
    // ensure_mission(); needed because MAVSDK's unsubscribe_mission_progress()
    // takes the exact handle subscribe_mission_progress() returned.
    mutable std::optional<mavsdk::Mission::MissionProgressHandle> progress_handle_;
    // Serializes the lazy init in ensure_mission(): both CommandExecutor's
    // worker (start()/pause()) and upload_worker_ can trigger the first
    // creation concurrently. Once created, `mission_`/`progress_handle_` are
    // never reset, so reads after a successful ensure_mission() need no lock.
    mutable std::mutex init_mutex_;

    // Guards every field below: written from handle_progress() (a MAVSDK
    // subscription thread) and from enqueue_upload()/upload_worker_'s job
    // body, read from get_progress()/take_pending_return_to_launch()/
    // notify_interrupted() (VehicleManager's publish thread and whichever
    // thread executes an interrupting command). One lock rather than one per
    // field since all of it changes together at pass/mission boundaries.
    mutable std::mutex state_mutex_;
    // Extra passes still owed for the mission currently uploaded; set from
    // Mission.repeat_count() when upload_worker_ finishes a job, decremented
    // each time handle_progress() re-triggers a pass, never negative.
    std::uint32_t passes_remaining_ = 0;
    // True if the currently uploaded Mission's last item was
    // MISSION_ACTION_RTL (and was therefore stripped before upload); tells
    // handle_progress() to set pending_return_to_launch_ once the final pass
    // completes.
    bool ends_with_return_to_launch_ = false;
    // Set by notify_interrupted(); once true, handle_progress() stops
    // re-triggering passes, stops setting pending_return_to_launch_, and
    // stops updating last_progress_.finished.
    bool interrupted_ = false;
    // Set by handle_progress() when the final pass of an RTL-terminated
    // mission completes; cleared by take_pending_return_to_launch().
    bool pending_return_to_launch_ = false;
    // Cached copy of the latest broadcastable progress; what get_progress()
    // returns.
    karshipta::v1::MissionProgress last_progress_;
    // Set by worker_'s job body when an upload job finishes (success or
    // failure); cleared by take_upload_result().
    std::optional<UploadResult> pending_upload_result_;
    // Set by worker_'s job body when a download job finishes (success or
    // failure); cleared by take_download_result().
    std::optional<DownloadResult> pending_download_result_;

    // One queued unit of work for worker_: either upload a Mission, or (when
    // is_download is true) download whatever is currently on the vehicle.
    // mission is meaningful only when !is_download.
    struct Job {
        bool is_download = false;
        karshipta::v1::Mission mission;
    };

    // Guards job_queue_ only, separate from state_mutex_ so enqueue_upload()/
    // enqueue_download() (called from a transport thread) never contend with
    // handle_progress() (called from a MAVSDK subscription thread) for the
    // same lock, mirroring CommandExecutor's mutex_/queue_ split from its ack
    // bookkeeping.
    std::mutex queue_mutex_;
    std::condition_variable_any queue_changed_;
    std::deque<Job> job_queue_;
    // Last: its loop calls into mission_ (via ensure_mission()) on every job,
    // so it must stop and join before mission_/progress_handle_ above it are
    // torn down. Mirrors CommandExecutor::worker_'s declared-last rule.
    std::jthread worker_;

    // Lazily creates `mission_` and its progress subscription the first time
    // either is needed. Returns false if `connection_` isn't connected yet;
    // returns true immediately if already created.
    bool ensure_mission() const;
    // Logs the outcome of a command already sent to `mission_` and passes the
    // Result through unchanged, mirroring VehicleActions::log_result.
    static mavsdk::Mission::Result log_result(const std::string& label,
                                               mavsdk::Mission::Result result);
    // Body of worker_, run on its jthread. Pops one queued Job and either
    // translates and uploads it (resetting pass-tracking/interrupted state
    // and recording the outcome in pending_upload_result_), or downloads the
    // vehicle's current mission and records the outcome in
    // pending_download_result_.
    void run_worker(const std::stop_token& stop_token);
    // subscribe_mission_progress()'s callback body. Updates last_progress_
    // and, on a finished pass, either re-triggers start_mission_async() (more
    // passes remain and not interrupted), sets pending_return_to_launch_
    // (final pass, ends_with_return_to_launch_, not interrupted), or leaves
    // the vehicle where the mission ended (final pass, no trailing RTL item).
    void handle_progress(mavsdk::Mission::MissionProgress progress);
    // Translates mission.items() into a mavsdk::Mission::MissionPlan. Strips
    // a trailing MISSION_ACTION_RTL item and reports whether one was present
    // via ends_with_rtl, since MAVSDK's Mission::MissionItem has no RTL
    // vehicle_action to translate it to.
    static mavsdk::Mission::MissionPlan translate(const karshipta::v1::Mission& mission,
                                                   bool& ends_with_rtl);
    // Inverse of translate(): builds a proto Mission from a downloaded
    // MissionPlan. Best-effort, unlike translate()'s strict validation: a
    // MissionItem::VehicleAction with no MissionAction equivalent (e.g.
    // TransitionToFw/TransitionToMc) maps to MISSION_ACTION_WAYPOINT rather
    // than failing the whole download, since an imperfect displayed result is
    // far lower-risk than an imperfect uploaded command. Leaves mission_id/
    // vehicle_id unset; the caller (run_worker(), then VehicleManager) fills
    // those in, since MissionPlan carries neither and this class does not
    // know its own vehicle_id.
    static karshipta::v1::Mission translate_back(const mavsdk::Mission::MissionPlan& plan);
};

#endif  // KARSHIPTA_GATEWAY_VEHICLE_MISSION_H
