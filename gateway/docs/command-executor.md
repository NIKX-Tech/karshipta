# CommandExecutor

`libs/ward/include/command_executor.h`, `libs/ward/src/command_executor.cpp`

## Overview

`CommandExecutor` closes the command loop (BRIEF.md M3/M5): decoded `Command`
messages go in, `WardActions`/`WardMission` calls happen on a worker
thread, and every command is answered with a `CommandAck`. It never touches
the wire itself; `main.cpp` decodes Envelopes and hands them to
`WardManager::dispatch_command`, which routes to the right ward's
`CommandExecutor` and turns its acks back into Envelopes through the
`Transport`.

## Responsibilities

- Execute exactly the schema's command set through `WardActions` and
  `WardMission`, mapping each `Command.action` case to its MAVSDK call.
- Answer every command: ACCEPTED when it enters the queue, then SUCCESS or
  REJECTED with a human-readable reason (gateway rule 5). Commands still
  queued at shutdown are rejected with "gateway shutting down", never dropped.
- Keep transport threads free: `enqueue()` is non-blocking; the blocking
  MAVSDK Action calls run on the executor's own `std::jthread`.
- Keep that worker thread interruptible even mid-command: `start_mission`/
  `pause_mission` go through `WardMission`'s async calls rather than its
  blocking ones, so a shutdown request can wake the wait for their result
  the same way it wakes the queue wait, instead of being stuck inside a
  MAVSDK call with no stop hook at all (gateway issue #69).

## Explicitly out of scope

- **Decoding bytes.** `main.cpp` parses the Envelope and routes by payload
  case; undecodable frames are logged there (no command_id exists to ack).
- **Ward routing.** `WardManager::dispatch_command` owns routing by
  `ward_id` across the fleet; `main.cpp` never touches `CommandExecutor`
  directly (see `ward-manager.md`).
- **Everything about a mission except starting/pausing it.** Upload, progress,
  `repeat_count` looping, RTL-item translation, and interrupt tracking all
  belong to `WardMission` (see `ward-mission.md`); `CommandExecutor`
  only calls its `start_async()`/`pause_async()`.
- **Event envelopes.** `WardManager::make_executor`'s ack callback
  publishes a WARNING `Event` for every rejection; the executor only reports
  acks.

## Ack semantics

| Moment | Ack |
|---|---|
| `enqueue()` on a command with an action | ACCEPTED ("queued") |
| `enqueue()` on a command without an action | REJECTED ("command has no action"), nothing queued |
| MAVSDK call returns Success | SUCCESS, message blank except `pause_mission` (see below) |
| MAVSDK call returns anything else | REJECTED with `WardActions::result_name()` or `WardMission::result_name()` text, whichever plugin was called |
| shutdown with commands still queued | REJECTED ("gateway shutting down") |

SUCCESS means the autopilot accepted the command, not that the maneuver
finished; progress is visible through the telemetry stream (mode, altitude,
armed) and, for missions, `WardMission::get_progress()`.

`pause_mission`'s SUCCESS message is `"pause"` or `"hold"` (never blank) so
the console can tell which of the two actually ran, since one command can
resolve to either depending on flight mode (see Command mapping below).
`dispatch()` normalizes every branch into `{bool, string}` for exactly this
reason: it can no longer assume a single MAVSDK `Result` enum, since
`pause_mission` alone can produce either `mavsdk::Mission::Result` or
`mavsdk::Action::Result` depending on which branch runs.

## Command mapping

- `arm` -> `arm()`; `disarm{force:false}` -> `disarm()`; `disarm{force:true}`
  -> `kill()` (motors stop immediately; the console confirms before sending).
- `takeoff` -> `set_takeoff_altitude(altitude_rel_m)` when positive, then
  `takeoff()`.
- `land` -> `land()`; `rtl` -> `return_to_launch()`.
- `goto` -> optional `set_current_speed(speed_m_s)` when positive, then
  `goto_location(lat, lon, msl, NaN)` (NaN keeps the current yaw). A zero
  `altitude_msl_m` from the console means "keep the current altitude": the
  executor reads current MSL from `TelemetryInfo::get_position()`, adding
  `altitude_rel_m` above home when the target carries one.
- `start_mission` -> always `WardMission::start_async()`.
- `pause_mission` -> `WardMission::pause_async()` (mission-aware, keeps the
  mission resumable) when `TelemetryInfo::get_flight_mode() ==
  mavsdk::Telemetry::FlightMode::Mission`; `WardActions::hold()` (manual
  hold, unrelated to any mission, still its blocking call) for every other
  flight mode, including never-connected (`Unknown`).

## Command authority freshness (gateway issue #182)

`enqueue()` acks ACCEPTED immediately, but the command itself can sit behind
others in the worker's queue, or just take a while to reach the front, before
`dispatch()` actually runs it. For `arm`, `disarm` (both force values),
`takeoff`, `land`, and `rtl` only, `dispatch()` rejects the command outright,
before doing anything else, if its `timestamp_ms` is more than
`kCommandAuthorityWindowMs` (5000ms, matching `ward_manager.cpp`'s
`kTeardownWaitBound`) away from the gateway's own clock at the moment it
actually runs - rejection message `"command timestamp outside authority
window"`. `goto`, `set_home`, `start_mission`, and `pause_mission` are
deliberately not gated yet; extending to them is a reasonable next step once
this window value is proven out, not part of this pass.

The check is on absolute drift, `|now_ms - timestamp_ms| > window`, not a
one-sided age: this is a self-hosted product with no guaranteed clock sync
between the console's host and the gateway's host, so `timestamp_ms` can
legitimately be a little ahead of the gateway's own clock, and that must
still read as fresh. A one-sided check (reject only when `timestamp_ms` is
too far in the *past*) would let a command timestamped arbitrarily far in
the *future* bypass this gate entirely, defeating the point of it. `drift_ms`
is computed by subtracting the smaller of `now_ms`/`timestamp_ms` from the
larger, in `uint64_t`, so it never wraps and the computation never needs
signed arithmetic (or risks signed-overflow UB) regardless of how bogus
`timestamp_ms` is, including `0` or a value near `UINT64_MAX`.

Internally-generated commands (the auto-RTL `WardManager` enqueues on
mission completion, the auto-land/pause-mission `FleetManager` enqueues on
geofence breach) stamp a fresh gateway-local timestamp right before
`enqueue()`, so they pass this check the same way a console command does;
they are not exempted from it.

## Threading

`enqueue()` runs on whatever thread the transport delivers frames from and
only takes the queue mutex. The worker is a `std::jthread` waiting on a
`std::condition_variable_any` with the thread's `stop_token`, so destruction
wakes it without a separate flag. `worker_` is the last member: it stops and
joins before anything it uses is destroyed. The ack callback fires from both
the enqueueing thread (ACCEPTED) and the worker (terminal), so it must be
thread-safe; `Transport::broadcast` is.

`dispatch_start_mission()`/`dispatch_pause()` fire `WardMission::
start_async()`/`pause_async()` and then wait for the result the same
interruptible way `run()` waits on the queue: a `std::condition_variable_any`
checked against `stop_token`, not a raw block inside MAVSDK. This is
`command_executor.cpp`'s `wait_for_mission_result()` (gateway issue #69):
`start_mission()`/`pause_mission()` are documented blocking by MAVSDK with no
cancel counterpart at all, unlike upload/download, so the fix here is to
never call the blocking form from the worker in the first place, rather than
add a way to interrupt it after the fact. If the executor is asked to stop
before the async callback fires, the wait returns early with `"gateway
shutting down"` and never reads the callback's actual result; the callback
itself only ever writes into a `shared_ptr`-owned holder (`command_executor.
cpp`'s `PendingMissionResult`), never into `CommandExecutor`/`WardMission`
state, so it stays safe to run whenever MAVSDK actually invokes it, even
after the wait that was watching for it has already given up.

## Automated tests

`gateway/tests/ward/command_executor_test.cpp`, against a never-connected
ward (no autopilot, no Docker):

- `EveryActionKindAnswersAcceptedThenTerminal`: arm, disarm, force disarm,
  takeoff, land, rtl, goto each get ACCEPTED first and a reasoned REJECTED
  after; no hang, no crash.
- `StartMissionFailsWithoutConnection`: exercises `dispatch_start_mission()`'s
  async wait via `start_async()`, whose `ensure_mission()` check fails
  synchronously (never connected), so the wait resolves immediately with
  `NoSystem`.
- `PauseMissionFallsBackToHoldOutsideMissionFlightModeAndFailsWithoutConnection`:
  not connected means flight mode is `Unknown`, never `Mission`, so this
  exercises the `hold()` branch, not `pause_async()`.
- `CommandWithoutActionIsRejectedWithoutQueueing`: exactly one ack.
- `AckEchoesCommandAndWardIds`.
- `StaleTimestampRejectsArmBeforeReachingWardActions`,
  `StaleTimestampRejectsEveryGatedDestructiveAction`: a 10s-old `timestamp_ms`
  on each gated action gets the exact `"command timestamp outside authority
  window"` rejection, fast, never a real MAVSDK-derived failure string.
- `StaleTimestampDoesNotGateGoto`, `StaleTimestampDoesNotGateSetHome`: the
  same stale timestamp on an ungated action still reaches the real dispatch
  path and fails for the ordinary no-connection reason, not this one.
- `TimestampSlightlyAheadOfGatewayClockIsStillFresh`: a `timestamp_ms` 1s
  ahead of the gateway's own clock (ordinary clock skew) is not rejected.
- `TimestampFarInTheFutureIsRejectedNotWavedThrough`: a `timestamp_ms` 1
  hour ahead is rejected the same way a stale one is, proving the bound is
  symmetric, not just a one-sided staleness check.

## Manual verification

With PX4 SITL running and the console connected (`PUBLIC_GATEWAY_WS_URL`):
Arm (tracker ACCEPTED then SUCCESS, ward arms), Takeoff 20 m (altitude
climbs in the detail panel), Goto a map point (ward flies there), RTL
(mode RETURN, descends, disarms). Takeoff while airborne rejects with the
autopilot's reason in the tracker and a COMMAND_REJECTED event in the feed.
