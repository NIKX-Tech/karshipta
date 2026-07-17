# VehicleManager

`libs/vehicle/include/vehicle_manager.h`, `libs/vehicle/src/vehicle_manager.cpp`

## Overview

`VehicleManager` owns the fleet: one `ManagedVehicle` (a `VehicleConnection` +
`TelemetryInfo` + `VehicleActions` + `CommandExecutor` quartet) per registered
vehicle, all sharing one `mavsdk::Mavsdk` core, keyed by `vehicle_id`. It is the
**only** class `main.cpp` (or any future consumer) talks to for vehicle
interaction: nothing outside `gateway/libs/vehicle/` constructs or calls
`VehicleConnection`/`TelemetryInfo`/`VehicleActions`/`CommandExecutor` directly.
Telemetry publishing, the on-connect `VehicleInfo` handshake, command routing,
and fleet persistence are all facade methods on this class.

## Responsibilities

- Register/deregister vehicles by `vehicle_id`, building and tearing down each
  one's object graph (`add_vehicle`/`remove_vehicle`, and the wire-facing
  `handle_add_vehicle`/`handle_remove_vehicle` that answer a console's
  `AddVehicle`/`RemoveVehicle` with a `VehicleConfigAck`).
- Persist the fleet to disk after every successful add/remove, and reload it
  at boot (`load_persisted`/`restore_and_start`) so a gateway that crashes or
  restarts while a vehicle is airborne reconnects to it instead of forgetting
  it (see Persistence below).
- Keep every vehicle reconnecting on its own timeline
  (`start`/`start_all`, one `reconnect_worker` `jthread` per vehicle) so one
  dead SITL container does not stall the rest of the fleet.
- Route an incoming `Command` to the right vehicle's `CommandExecutor`
  (`dispatch_command`), rejecting with a reason (gateway rule 5) if the
  `vehicle_id` is unknown, stopped, or mid-transition.
- Take vehicles offline safely: `stop` (rejects if airborne or link-down,
  ground-safety guarded) and `force_stop` (operator override that commands
  RTL and supervises the flight home before disarming).
- Publish `VehicleState` for every registered vehicle at a shared rate
  (`start_publishing`) and send `VehicleInfo` to a newly connected client
  (`send_vehicle_info`).

## Explicitly out of scope

- **MAVLink/telemetry/action mechanics themselves.** Delegated entirely to
  `VehicleConnection`/`TelemetryInfo`/`VehicleActions`/`CommandExecutor`; this
  class only orchestrates them per vehicle and never duplicates their logic.
- **Missions.** `Mission`/`MissionProgress` land in M5.
- **Graceful shutdown of a still-flying fleet.** The destructor waits for any
  in-flight transition to quiesce before tearing down, but does not
  proactively `force_stop_all()` a fleet that was never told to stop; that
  needs `main.cpp` to call it before dropping the `VehicleManager`, which
  isn't wired up yet (no signal handling exists in `main.cpp` today).
- **A relay transport.** `VehicleManager` talks to whatever `Transport&` it's
  given; it has no idea whether that's the plain WebSocket server or the M4
  relay transport.

## Public API

| Member | Behavior |
|---|---|
| `static unique_ptr<VehicleManager> create(Transport&, filesystem::path persistence_path = {})` | Builds the shared `Mavsdk` core itself. The entry point every consumer outside this library should use. |
| `add_vehicle(const VehicleConfig&)` | Builds the object graph and registers it; does not connect. Rejects an empty or already-registered `vehicle_id`, or a nonzero `system_id` already bound elsewhere. |
| `dispatch_command(const Command&)` | Routes to the vehicle's `CommandExecutor`; synthesizes a REJECTED `CommandAck` for unknown/stopped/busy vehicles. |
| `handle_mission_upload(const Mission&)` | Routes to the vehicle's `VehicleMission::enqueue_upload()`; broadcasts a WARNING `Event` for unknown/stopped/busy vehicles (no `CommandAck`-shaped ack exists for missions). |
| `handle_mission_file_upload(const MissionFileUpload&)` | Converts via the vehicle's `MissionImporter::import()` (synchronous - local parsing, not a MAVLink round trip), then routes through `handle_mission_upload()`'s same path on success; a WARNING `Event` on either the vehicle lookup or the import itself failing. |
| `handle_mission_download_request(const MissionDownloadRequest&)` | Routes to the vehicle's `VehicleMission::enqueue_download()`; broadcasts a WARNING `Event` for unknown/stopped/busy vehicles. Result surfaces later via the publish tick. |
| `start(vehicle_id)` / `start_all()` | Launches (or relaunches) a vehicle's `reconnect_worker`. |
| `stop(vehicle_id)` / `stop_all()` | Takes a vehicle offline; rejects airborne or link-down (state unknown). |
| `force_stop(vehicle_id)` / `force_stop_all()` | Operator override: commands RTL, supervises landing, then disarms and stops. |
| `load_persisted()` | Reloads every vehicle from `persistence_path`; 0 if unset or the file doesn't exist. |
| `restore_and_start()` | `load_persisted()` then `start_all()` - the crash-recovery entry point. |
| `start_publishing(interval = kDefaultPublishInterval)` | Launches the shared telemetry-publish worker. |
| `send_vehicle_info(client)` | Sends one `VehicleInfo` per discovered vehicle to a single client (for `Transport::on_connect`). |
| `is_started`/`is_connected`/`list_status`/`list_vehicle_ids` | Read-only fleet queries. |
| `remove_vehicle(vehicle_id)` / `remove_all()` | Applies `stop`'s ground-safety rules, then erases. |
| `handle_add_vehicle`/`handle_remove_vehicle` | Wire entry points: `AddVehicle`/`RemoveVehicle` in, `VehicleConfigAck` out. Never throw. |

## Persistence

`persistence_path` (constructor/`create()` parameter, empty = disabled) is
where the fleet is durably recorded, per `fleet.proto`'s own header comment:
*"the gateway owns persistence (its config file becomes the durable form of
these requests)."* Every successful `add_vehicle`/`remove_vehicle` rewrites
the whole file (`persist_locked()`, called under `vehicles_mutex_` - the file
is small and the event is rare/human-triggered, so writing under the lock is
the safer choice over racing two mutations to disk out of order). The file is
YAML (yaml-cpp), one entry per vehicle: `vehicle_id`, `connection_url`,
`mavlink_system_id`, `name`, and `type` (via the proto-generated
`VehicleType_Name`/`_Parse`).

`load_persisted()` reconstructs each entry via the same `add_vehicle_impl()`
core `add_vehicle()` uses, but with persistence suppressed for that call (it
would otherwise rewrite the file once per loaded entry for no reason - the
content on disk hasn't changed). A malformed entry is logged and skipped; the
rest still load. `main.cpp` calls `restore_and_start()` once at boot, before
serving any client: a vehicle that was mid-flight when the gateway last exited
starts reconnecting immediately, so `force_stop`/`stop` can act on it again
instead of it being silently forgotten.

This one file doubles as the "config file lists vehicles" mechanism
`BRIEF.md`'s M4 section describes - no separate static-config-loader exists or
is needed. `gateway/config/` is a tracked directory (`.gitkeep`); the
generated `fleet_state.yaml` itself is gitignored.

## Threading

- `vehicles_mutex_` guards `managed_vehicles_` and every element's
  `executor`/`busy`/`reconnect_worker`. Lock order: `vehicles_mutex_` before
  any `VehicleConnection`/`TelemetryInfo`/`VehicleActions` internal mutex.
- Each vehicle's `reconnect_worker` is an independent `jthread`: connects,
  broadcasts an INFO `LINK_CONNECTED` `Event`, requests the telemetry stream
  rate (re-requested on every reconnect - PX4 forgets it across a link drop),
  waits while connected, and on drop broadcasts a WARNING `LINK_LOST` `Event`
  before reconnecting (not on a deliberate `stop()`/`force_stop()` - the loop
  only emits `LINK_LOST` when the drop wasn't requested).
- One shared `publish_worker_` (not one per vehicle: the per-tick work is
  cheap cached-getter reads, not I/O, so batching it on one thread is simpler
  and just as fast) builds every vehicle's `VehicleState` under the lock, then
  releases it before broadcasting - broadcasting under the lock would stall
  every other `VehicleManager` call behind one slow client for the whole
  tick, the same hazard `dispatch_command`'s ack path and
  `send_vehicle_info`'s blocking firmware query are equally careful to avoid.
- The same tick also polls each vehicle's `VehicleMission` (BRIEF.md M5):
  `get_progress()` into a `mission_progress` frame (only once something has
  actually been uploaded), `take_pending_return_to_launch()` (deferred into
  an unlocked synthetic `ReturnToLaunchCommand` enqueue, using the same
  busy-then-unlocked pattern as `dispatch_command`, since a concurrent
  `remove_vehicle` could otherwise destroy the executor mid-call),
  `take_upload_result()`/`take_download_result()` (deferred into
  `broadcast_mission_event()`/`broadcast_mission_download()` calls after the
  lock releases, for the same blocking-socket-write reason).
- Both worker types are declared after `managed_vehicles_` (`publish_worker_`
  last of all), so C++'s reverse-declaration-order destruction stops and
  joins every thread before the vehicles they read are torn down. The
  destructor additionally blocks until no vehicle is `busy`, since
  `force_stop()` spends up to two minutes unlocked holding a raw
  `ManagedVehicle*`.

## Automated tests

`gateway/tests/vehicle/vehicle_manager_test.cpp` (GoogleTest, no SITL/Docker -
`add_vehicle`/`load_persisted` never call `connect()`), against a `FakeTransport`
that records `broadcast()`/`send()` calls instead of touching a real socket:

- `AddVehicleRegistersAndListsId`, `AddVehicleRejectsEmptyId`,
  `AddVehicleRejectsDuplicateId`, `AddVehicleRejectsDuplicateSystemId`.
- `DispatchCommandRejectsUnknownVehicleWithReason`: the broadcast `CommandAck`
  carries the offending id in its message.
- `RemoveVehicleRemovesNeverStartedVehicle`.
- `PersistsOnAddAndRoundTripsOnReload`: two vehicles, distinct
  `type`/`name`/`system_id`; a fresh manager pointed at the same file reloads
  both, and a `system_id` collision against a reloaded vehicle is still
  rejected (proves `system_id` itself survived the round trip).
- `RemovePersistsRemoval`, `LoadPersistedOnMissingFileReturnsZero`,
  `MalformedEntryIsSkippedNotFatal`, `LoadPersistedDoesNotRewriteFile`.

## Manual verification

With PX4 SITL running (`gateway/CLAUDE.local.md` has the full command list),
build and run the gateway:

```
cmake -S gateway -B gateway/build
cmake --build gateway/build
./gateway/build/src/karshipta_gateway
```

First run: log shows `vehicle 'sitl-1' registered` (the seeded default) and
`gateway/config/fleet_state.yaml` is created with that entry. Kill the process
and rerun it: the log now shows `loaded 1 vehicle(s) from
'gateway/config/fleet_state.yaml'` instead of re-seeding, and the reconnect
loop resumes on its own.

Connect a WebSocket test client and send an `AddVehicle` for a second SITL
instance (port 14541): a `VehicleConfigAck` comes back ACCEPTED and
`fleet_state.yaml` now lists two vehicles. Send a `Command` for each
`vehicle_id` and confirm each is routed to the right vehicle. Send
`RemoveVehicle` for one (after `stop`ping it, or while it's still on the
ground) and confirm the file drops back to one entry.
