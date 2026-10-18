# CalibrationExecutor

`libs/ward/include/calibration_executor.h`, `libs/ward/src/calibration_executor.cpp`

## Overview

`CalibrationExecutor` implements pre-flight sensor calibration
(karshipta#248): `StartCalibration` and `CancelCalibration` go in,
`WardActions`'s `Calibration` calls happen on a worker thread, and every
intermediate step (percentage progress, an operator-facing instruction
string, or both) streams back to the requesting client as a
`CalibrationProgress` message, ending in exactly one terminal
SUCCESS/FAILED/CANCELLED/REJECTED. It never touches the wire itself;
`main.cpp` decodes Envelopes and hands them to `WardManager::
handle_start_calibration`/`handle_cancel_calibration`, which route to the
right ward's `CalibrationExecutor` and turn its callback back into
Envelopes through the `Transport`.

Calibration is a guided, operator-paced, multi-step process - MAVSDK's
`Calibration` plugin streams progress and instructions (e.g. "rotate to
the next orientation") rather than answering once, and the whole thing can
run for as long as the operator takes to physically move the vehicle -
categorically too slow and too interactive for the network thread, the
same reasoning that moved flight log download onto its own worker thread
(`LogDownloadExecutor`, `log-download-executor.md`).

## Responsibilities

- Resolve `StartCalibration` into a stream of `CalibrationProgress`
  messages via whichever of `WardActions::calibrate_gyro()`/
  `calibrate_accelerometer()`/`calibrate_magnetometer()`/
  `calibrate_level_horizon()` matches `calibration_type`.
- Allow at most one calibration in flight per ward at a time: a second
  `StartCalibration` while one is already running is rejected immediately
  (`CALIBRATION_STATUS_REJECTED`), without ever touching the worker
  thread - a vehicle cannot usefully calibrate two sensors at once.
- Route every result to the requesting client only, never broadcast: same
  "meaningful just to the requester" reasoning as `LogDownloadExecutor`.
- Resolve `CancelCalibration` by calling `WardActions::
  cancel_calibration()` directly on the calling thread (never the worker,
  which may be blocked deep inside whichever `calibrate_*` call is
  running) - MAVSDK's `cancel()` is already a short, ordinary blocking
  call, nothing to bridge. A cancel whose `calibration_request_id` does
  not match the currently running job (stale, or targets nothing) is a
  logged no-op.

## Explicitly out of scope

- **Deciding whether a calibration is safe to attempt.** MAVSDK itself
  refuses to calibrate an armed vehicle (`Calibration::Result::
  FailedArmed`); this class adds no extra flight-safety gating of its own
  on top of that, and does not retry or reinterpret that refusal.
- **`gimbal_accelerometer` calibration.** No ward class with a gimbal
  exists yet - same "needs a capability flag first" reasoning
  karshipta#256 already gives for camera/gimbal features. Only
  gyro/accelerometer/magnetometer/level-horizon are wired.
- **Reporting a cancel's own success/failure directly.** `cancel()`'s
  result is not sent back on its own path at all; a successful cancel
  surfaces as the running job's own terminal `CalibrationProgress{
  CANCELLED}` shortly after, and a failed one simply leaves that job
  running, observable as it continuing to send `RUNNING` updates.
- **Ward routing.** `WardManager::handle_start_calibration`/
  `handle_cancel_calibration` own routing by `ward_id`, including the two
  synchronous rejections (unknown ward, not connected) that never reach
  this class at all - see `ward-manager.md`.

## Lifetime: never retired, unlike CommandExecutor

Same treatment as `LogDownloadExecutor`: `ManagedWard::
calibration_executor` is built once, in `add_ward_impl()`, and lives for
as long as the ward's own `WardActions` does - never retired/rebuilt by
`stop()`/`force_stop()`/`remove_ward()`. For calibration specifically this
is doubly safe: MAVSDK's own `FailedArmed` check already means a
calibration cannot succeed while the vehicle is armed, and keeping this
executor alive through a `stop()`/`force_stop()` transition means a stuck
calibration can still be cancelled even while the operator is also trying
to take the ward offline.

## Status semantics

| Moment | Message |
|---|---|
| `StartCalibration`, a calibration is already running for this ward | `CalibrationProgress{REJECTED}`, no worker-thread round trip |
| `StartCalibration` with an unspecified/unrecognized `calibration_type` | `CalibrationProgress{REJECTED}`, no worker-thread round trip |
| intermediate `Result::Next` from MAVSDK | `CalibrationProgress{RUNNING}`, `has_progress`/`has_instruction` say which of `progress`/`instruction` are meaningful on this message |
| MAVSDK terminal result is `Success` | `CalibrationProgress{SUCCESS}` |
| MAVSDK terminal result is `Cancelled` | `CalibrationProgress{CANCELLED}` |
| MAVSDK terminal result is anything else (`Failed`, `NoSystem`, `Busy`, `CommandDenied`, `Timeout`, `FailedArmed`, `Unsupported`, ...) | `CalibrationProgress{FAILED}` with `WardActions::result_name()` text |
| gateway shutdown with a job still queued | dropped, not acked - the console's own request timeout is what settles it (no "accepted" ack was ever sent for a dropped job to retract) |

## Threading

`enqueue_start()` runs on whatever thread the transport delivers frames
from (via `WardManager`) and only takes the queue mutex; it reserves the
one-job slot (`running_request_id_`) synchronously, before the job is
even handed to the worker, so two `StartCalibration` requests arriving
back to back can never both be accepted. The worker is a `std::jthread`
waiting on a `std::condition_variable_any` with the thread's `stop_token`,
same shape as `CommandExecutor::run()`/`LogDownloadExecutor::run()`.
`worker_` is the last member: it stops and joins before `actions_` (and
anything else it uses) is destroyed. Every progress callback fires from
the worker thread and takes the requesting client's `Transport::
ClientId`, so it can `Transport::send()` just that client.

`cancel()` deliberately does **not** go through the worker thread or the
queue at all - it calls `WardActions::cancel_calibration()` directly on
whichever thread called `cancel()` (via `WardManager`, ultimately the
network thread), since that MAVSDK call is already a short, ordinary
blocking round trip, not something that needs bridging to a worker.

## Automated tests

`gateway/tests/ward/ward_manager_test.cpp`, against a never-connected
ward (no autopilot, no Docker) - the only cases testable without a live
connection, since a real calibration needs a real vehicle to guide:

- `HandleStartCalibrationRejectsUnknownWardWithReason`
- `HandleStartCalibrationRejectsUnconnectedWardWithReason`
- `HandleCancelCalibrationOnUnknownWardSendsNothing`
- `RejectViewerEnvelopeRejectsStartCalibrationWithProgress`
- `RejectViewerEnvelopeIgnoresCancelCalibrationSilently`

## Manual verification

With PX4 SITL running and the console connected: start a level-horizon
calibration (the simplest of the four - no physical movement needed in
SITL) for a ward and confirm progress/instruction text streams in, ending
SUCCESS; start a second calibration for the same ward while the first is
still running and confirm it is rejected immediately ("already running");
cancel a running calibration and confirm it settles CANCELLED, not stuck.
Attempting calibration on an armed ward should fail with MAVSDK's own
`FailedArmed` reason surfaced verbatim.
