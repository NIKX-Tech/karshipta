# CommandExecutor

`libs/vehicle/include/command_executor.h`, `libs/vehicle/src/command_executor.cpp`

## Overview

`CommandExecutor` closes the command loop (BRIEF.md M3): decoded `Command`
messages go in, `VehicleActions` calls happen on a worker thread, and every
command is answered with a `CommandAck`. It never touches the wire itself;
`main.cpp` decodes Envelopes and hands them to `VehicleManager::
dispatch_command`, which routes to the right vehicle's `CommandExecutor` and
turns its acks back into Envelopes through the `Transport`.

## Responsibilities

- Execute exactly the schema's command set through `VehicleActions`, mapping
  each `Command.action` case to its MAVSDK call.
- Answer every command: ACCEPTED when it enters the queue, then SUCCESS or
  REJECTED with a human-readable reason (gateway rule 5). Commands still
  queued at shutdown are rejected with "gateway shutting down", never dropped.
- Keep transport threads free: `enqueue()` is non-blocking; the blocking
  MAVSDK Action calls run on the executor's own `std::jthread`.

## Explicitly out of scope

- **Decoding bytes.** `main.cpp` parses the Envelope and routes by payload
  case; undecodable frames are logged there (no command_id exists to ack).
- **Vehicle routing.** `VehicleManager::dispatch_command` owns routing by
  `vehicle_id` across the fleet; `main.cpp` never touches `CommandExecutor`
  directly (see `vehicle-manager.md`).
- **Missions.** `start_mission`/`pause_mission` reject with a pointer to M5
  (issue #17) until the Mission plugin lands.
- **Event envelopes.** `VehicleManager::make_executor`'s ack callback
  publishes a WARNING `Event` for every rejection; the executor only reports
  acks.

## Ack semantics

| Moment | Ack |
|---|---|
| `enqueue()` on a command with an action | ACCEPTED ("queued") |
| `enqueue()` on a command without an action | REJECTED ("command has no action"), nothing queued |
| MAVSDK call returns Success | SUCCESS |
| MAVSDK call returns anything else | REJECTED with `VehicleActions::result_name()` text |
| shutdown with commands still queued | REJECTED ("gateway shutting down") |

SUCCESS means the autopilot accepted the command, not that the maneuver
finished; progress is visible through the telemetry stream (mode, altitude,
armed), which is how the console already renders it.

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

## Threading

`enqueue()` runs on whatever thread the transport delivers frames from and
only takes the queue mutex. The worker is a `std::jthread` waiting on a
`std::condition_variable_any` with the thread's `stop_token`, so destruction
wakes it without a separate flag. `worker_` is the last member: it stops and
joins before anything it uses is destroyed. The ack callback fires from both
the enqueueing thread (ACCEPTED) and the worker (terminal), so it must be
thread-safe; `Transport::broadcast` is.

## Automated tests

`gateway/tests/vehicle/command_executor_test.cpp`, against a never-connected
vehicle (no autopilot, no Docker):

- `EveryActionKindAnswersAcceptedThenTerminal`: arm, disarm, force disarm,
  takeoff, land, rtl, goto each get ACCEPTED first and a reasoned REJECTED
  after; no hang, no crash.
- `MissionCommandsRejectWithM5Pointer`.
- `CommandWithoutActionIsRejectedWithoutQueueing`: exactly one ack.
- `AckEchoesCommandAndVehicleIds`.

## Manual verification

With PX4 SITL running and the console connected (`PUBLIC_GATEWAY_WS_URL`):
Arm (tracker ACCEPTED then SUCCESS, vehicle arms), Takeoff 20 m (altitude
climbs in the detail panel), Goto a map point (vehicle flies there), RTL
(mode RETURN, descends, disarms). Takeoff while airborne rejects with the
autopilot's reason in the tracker and a COMMAND_REJECTED event in the feed.
