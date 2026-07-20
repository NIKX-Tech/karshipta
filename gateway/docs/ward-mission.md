# WardMission

`libs/ward/include/ward_mission.h`, `libs/ward/src/ward_mission.cpp`

## Overview

`WardMission` wraps MAVSDK's `Mission` plugin for exactly **one**
already-connected ward: uploading a mission, starting/pausing it,
reporting its progress, and looping `repeat_count` extra passes (BRIEF.md
M5). It reads through a `WardConnection&` rather than owning a `System`
itself, and lazily constructs the underlying `mavsdk::Mission` plugin (and
registers its one permanent progress subscription) the first time any
method is called, once that connection has succeeded. It holds no
reference to `WardActions` or `CommandExecutor` at all: it only exposes
state (`get_progress()`, `take_pending_return_to_launch()`,
`take_upload_result()`) for `WardManager`'s existing shared publish tick
to poll, the same way it already polls `TelemetryInfo`.

## Responsibilities

- Lazily bind a `mavsdk::Mission` plugin to `WardConnection::get_system()`
  once the connection is live (`ensure_mission()`), and register its one
  permanent `subscribe_mission_progress()` callback at the same time.
- Validate and translate a proto `karshipta::v1::Mission` into a
  `mavsdk::Mission::MissionPlan`, off the calling thread (`enqueue_upload()`
  + its own worker thread), since `upload_mission()` blocks.
- Suppress the autopilot's own end-of-mission RTL/land before every upload
  (`set_return_to_launch_after_mission(false)`), and separately track
  whether the uploaded mission's last item was `MISSION_ACTION_RTL` (MAVSDK
  has no such `vehicle_action`), signaling that intent via
  `take_pending_return_to_launch()` instead.
- Re-trigger `start_mission_async()` between repeat passes, and mark a
  mission `finished` only once every pass has completed.
- Track whether the current mission was interrupted by something outside
  this class (`notify_interrupted()`), so a re-trigger or a `finished`
  report never fires for a mission a console command already cut short.

## Explicitly out of scope

- **Connection lifecycle.** Owned by `WardConnection`; `WardMission`
  only reads `is_connected()`, `get_system()`, and `get_mavsdk()` from it.
- **Non-mission commands** (arm/disarm/takeoff/land/rtl/goto). `WardActions`'
  job, using MAVSDK's `Action` plugin against the same `WardConnection`.
  `WardMission` never references `WardActions`.
- **Dispatching `StartMissionCommand`/`PauseMissionCommand`, or deciding
  between a mission-aware pause and a manual hold.** `CommandExecutor`'s
  job; it calls `start()`/`pause()` directly, synchronously, from its own
  worker thread. `WardMission` never references `CommandExecutor`.
- **`MissionRaw` and mission file import (QGC/Mission Planner).**
  `MissionImporter`'s job (`gateway/docs/mission-importer.md`); this class
  only ever executes an already-valid proto `Mission`, however it was
  produced.
- **Publishing to the wire.** Translating `get_progress()`'s result and the
  two pending-flag accessors into `Envelope` frames is `WardManager`'s
  job (its existing publish tick), not this class's.

## Public API

| Member | Behavior |
|---|---|
| `explicit WardMission(WardConnection&)` | Binds to a connection. Does not create the `Mission` plugin yet; that happens lazily on first use. |
| `~WardMission()` | Unsubscribes the mission-progress callback if the plugin was ever created. |
| `static std::string result_name(mavsdk::Mission::Result)` | Human-readable text for a `Mission::Result`. |
| `void enqueue_upload(karshipta::v1::Mission)` | Non-blocking. Queues an upload job for the worker thread. Outcome (success or a rejection reason) is read later via `take_upload_result()`. |
| `void enqueue_download()` | Non-blocking. Queues a request to download the mission currently on the ward. Outcome is read later via `take_download_result()`. Does not touch repeat-pass/interrupted/pending-RTL state. |
| `mavsdk::Mission::Result start() const` | Implements `StartMissionCommand`. Blocking but fast; a mission must already be uploaded. |
| `mavsdk::Mission::Result pause() const` | Implements the mission-aware branch of `PauseMissionCommand`. Blocking but fast. |
| `karshipta::v1::MissionProgress get_progress() const` | Non-blocking cached read of the latest progress. Default (empty `mission_id`, `finished=false`) until an upload has actually succeeded. |
| `bool notify_interrupted()` | Marks the current mission interrupted if one is active; returns `false` (no-op) otherwise. Idempotent. |
| `bool take_pending_return_to_launch()` | Test-and-clear: `true` at most once, when the final pass of an RTL-terminated mission completes. |
| `std::optional<UploadResult> take_upload_result()` | Test-and-clear: the outcome of the most recently finished `enqueue_upload()` job. |
| `std::optional<DownloadResult> take_download_result()` | Test-and-clear: the outcome of the most recently finished `enqueue_download()` job. `mission` has no value on failure. |

## Design: one job queue and worker thread for both upload and download

`upload_mission()` and `download_mission()` are both blocking, potentially
slow MAVLink data transfers, so neither `enqueue_upload()` nor
`enqueue_download()` calls its MAVSDK counterpart directly. Both push onto
the same `job_queue_` (a small tagged `Job{bool is_download; Mission
mission;}`, `mission` meaningful only when `!is_download`), mirroring
`CommandExecutor`'s own queue/worker split (`libs/ward/src/command_executor.cpp`):

```cpp
void WardMission::enqueue_upload(karshipta::v1::Mission mission) {
    { std::lock_guard lock(queue_mutex_); job_queue_.push_back(Job{false, std::move(mission)}); }
    queue_changed_.notify_one();
}
void WardMission::enqueue_download() {
    { std::lock_guard lock(queue_mutex_); job_queue_.push_back(Job{true, {}}); }
    queue_changed_.notify_one();
}
```

Sharing one queue and one worker thread serializes upload and download
against the same `mission_` plugin instance automatically - a download
requested while an upload is mid-flight simply waits its turn, no extra
synchronization needed.

`run_worker()` (the body of `worker_`, a `std::jthread`) waits on
`queue_changed_`, pops one `Job`, and branches on `is_download`:

- **Upload**: `validate_mission()` (free function, see below) first, then
  `ensure_mission()`, then `translate()` and `upload_mission()`.
- **Download**: `ensure_mission()`, then `download_mission()` (blocking),
  then `translate_back()` (the inverse of `translate()`) to build a proto
  `Mission`, with `mission_id` reused from `last_progress_` if this process
  uploaded something, or synthesized otherwise.

Unlike `CommandExecutor::enqueue()`, neither has a synchronous accept/reject
signal - every outcome, including a validation failure, is only observable
later via `take_upload_result()`/`take_download_result()`. Jobs still
queued at shutdown are dropped, not run, the same tradeoff `CommandExecutor`
documents for its own queue.

## Design: translate_back() is best-effort, unlike translate()

`translate()` (upload direction) is strict: `validate_mission()` rejects
anything it can't faithfully represent before `ensure_mission()` is even
called. `translate_back()` (download direction) is deliberately more
lenient: a `MissionItem::VehicleAction` with no `MissionAction` equivalent
(`TransitionToFw`/`TransitionToMc`, from a VTOL mission this class never
uploaded itself) is approximated as `MISSION_ACTION_WAYPOINT` rather than
failing the whole download. The reasoning is asymmetric on purpose: an
imperfect *displayed* mission is far lower-risk than an imperfect *uploaded*
one, so download favors always returning something over strict fidelity.

One known, accepted gap: an RTL item `translate()` strips before upload was
never an actual on-ward mission item (see the RTL-item design section
below), so `translate_back()` can't reconstruct it - downloading a mission
this same process uploaded with a trailing RTL item will not show that item,
even though the console displayed one on upload.

## Design: validation lives outside `translate()`

`translate()`'s signature (`static MissionPlan translate(const Mission&, bool&
ends_with_rtl)`) has no way to report a failure - it only ever produces a
plan. A separate free function in `ward_mission.cpp`'s anonymous
namespace, `validate_mission()`, runs first and rejects what `translate()`
cannot: an empty `ward_id`, an empty `items` list, or a
`MISSION_ACTION_RTL` item anywhere but last. `translate()` documents this
as a precondition rather than re-checking it. A validation failure records
`Mission::Result::InvalidArgument` into `pending_upload_result_` without
`ensure_mission()` or any MAVSDK call ever running.

## Design: RTL-item translation and the deferred return-to-launch

MAVSDK's `Mission::MissionItem::VehicleAction` enum has no RTL member
(only `None`/`Takeoff`/`Land`/`TransitionToFw`/`TransitionToMc`), so a
schema `MissionItem` with `action = MISSION_ACTION_RTL` cannot become a
native mission item. `translate()` strips a trailing RTL item before
building the `MissionPlan` and reports its presence via the `ends_with_rtl`
out-parameter; `run_worker()` stores that as
`ends_with_return_to_launch_` on a successful upload. `MISSION_ACTION_HOLD`
has a similar gap (no `Hold` `VehicleAction`) but is expressed instead via
`loiter_time_s`, which MAVSDK does support per-item.

Separately, MAVSDK's `set_return_to_launch_after_mission(false)` only takes
effect on the *next* upload per its own documentation, so it is called
unconditionally before every `upload_mission()` call, not just the first
ever. This means the autopilot never performs its own end-of-mission
RTL/land; when `ends_with_return_to_launch_` is set, `handle_progress()`
instead sets `pending_return_to_launch_` once the final pass completes,
for `WardManager` to act on explicitly (via `WardActions`) - an
explicit gateway decision rather than an autopilot default.

## Design: repeat passes and interrupts

Three pieces of state under `state_mutex_` drive this:

- `passes_remaining_` - set to `Mission.repeat_count()` on a successful
  upload, decremented each time `handle_progress()` re-triggers a pass.
- `interrupted_` - set by `notify_interrupted()`; once true, `handle_progress()`
  stops re-triggering, stops setting `pending_return_to_launch_`, and stops
  ever reporting `finished=true` for that mission.
- `ends_with_return_to_launch_` - see above.

`handle_progress()` (the `subscribe_mission_progress()` callback body, so it
runs on a MAVSDK-internal thread) checks `progress.current == progress.total`
for "this pass is finished." If interrupted, it does nothing further. If
passes remain, it decrements and fires `start_mission_async()` (fire-and-forget,
logging-only callback - non-blocking, safe to call while holding
`state_mutex_`). Only on the true final pass does it set `finished=true`
and, if applicable, `pending_return_to_launch_ = true`.

`notify_interrupted()`'s "is a mission currently active" check is
`!interrupted_ && !last_progress_.mission_id().empty() &&
!last_progress_.finished()`. `last_progress_` is populated as soon as an
upload succeeds (`run_worker()`, not only from the first
`handle_progress()` callback) specifically so this check - and
`get_progress()`'s own documented default - behave correctly from the
moment of upload, not only after the first progress frame arrives.

## Thread safety

- `init_mutex_` guards the one-time lazy construction in `ensure_mission()`.
  Once `mission_`/`progress_handle_` are set, they are never reset, so
  reads after a successful `ensure_mission()` need no lock.
- `queue_mutex_`/`queue_changed_`/`job_queue_` are separate from
  `state_mutex_` so `enqueue_upload()` (called from a transport thread)
  never contends with `handle_progress()` (called from a MAVSDK thread) for
  the same lock - the same split `CommandExecutor` uses between its queue
  and its ack bookkeeping.
- `state_mutex_` guards every piece of progress/pass/interrupt state,
  written from both `run_worker()`'s thread and `handle_progress()`'s
  MAVSDK thread, and read from `get_progress()`/`notify_interrupted()`/
  `take_pending_return_to_launch()`/`take_upload_result()` (whatever thread
  `WardManager`'s publish tick or an interrupting command runs on).

## RAII and ownership rules

```cpp
explicit WardMission(WardConnection& connection);
~WardMission();  // unsubscribes mission-progress if the plugin was ever created
```

- **No default construction, no copy/move declared** (implicitly deleted by
  the reference member `connection_` and the non-copyable `std::jthread`/
  `std::mutex` members): mirrors `WardActions`/`TelemetryInfo`.
- **Does not own `connection_`.** The caller must keep the referenced
  `WardConnection` alive for at least as long as this `WardMission`.
- **Destructor unsubscribes before `worker_` joins.** The explicit
  destructor body runs first (unsubscribing the progress callback), then
  `worker_`'s own `jthread` destructor requests a stop and joins,
  which can race a job still in flight against the same `mission_`
  instance - relying on MAVSDK's plugin methods tolerating concurrent
  calls, the same implicit assumption `TelemetryInfo`'s destructor makes.

## Constraints and preconditions

- **Requires a successful `WardConnection::connect()` before any real
  work happens.** `start()`/`pause()` return `Mission::Result::NoSystem` if
  not yet connected; a queued upload records the same result via
  `take_upload_result()` rather than ever calling MAVSDK.
- **Does not rebind across reconnects**, same as `TelemetryInfo`.
- **`start()`/`pause()` are not queued.** They are fast MAVSDK calls (no
  data transfer), called directly and synchronously by `CommandExecutor`'s
  own worker thread - only `enqueue_upload()` goes through this class's own
  queue.
- **A mission must be uploaded before `start()`.** This class does not
  enforce that itself; MAVSDK's own `Result` reflects it.

## Automated tests

`gateway/tests/ward/ward_mission_test.cpp`, against the same
deliberately-unconnected-`WardConnection` strategy `command_executor_test.cpp`
uses for `WardActions`: validation rejections (empty `ward_id`, empty
items, non-terminal RTL) are covered directly since they never touch
MAVSDK; a well-formed RTL-terminated mission is confirmed to pass
validation and fail at `NoSystem` instead; `start()`/`pause()`/`enqueue_download()`
all fail fast without a connection; the accessor defaults (including
`take_download_result()`) and the destructor's queue-dropping behavior are
covered. Actual upload/download/progress/repeat-loop behavior against a live MAVLink
mission handshake is **not** exercised - there is no MAVSDK server-plugin
or SITL harness in this repo, and a fake heartbeat-only autopilot core
(the pattern `ward_connection_test.cpp` uses) does not implement the
mission upload protocol, so it would not give deterministic results. Same
accepted gap `command_executor_test.cpp` documents for `WardActions`.

## Manual verification

With one PX4 SITL container running:

```
docker run --rm -it -p 14550:14550/udp -p 14540:14540/udp px4io/px4-sitl:latest
```

Build and run the gateway (`gateway/CLAUDE.local.md` has the full command
list). From a throwaway call (e.g. a scratch harness or a temporary line in
`main.cpp`), call `enqueue_upload()` with a small real mission and observe:

```
[info] mission plugin created
[info] mission upload succeeded
```

followed by progress-driven log lines as the ward flies it, and, if the
mission's last item was `MISSION_ACTION_RTL`, a `pending_return_to_launch_`
flip once the final pass completes (observable today only by polling
`take_pending_return_to_launch()` directly; `WardManager` does not yet
act on it - that wiring is a separate follow-up task).
