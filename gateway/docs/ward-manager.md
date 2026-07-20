# WardManager

`libs/ward/include/ward_manager.h`, `libs/ward/src/ward_manager.cpp`


## Overview

`WardManager` owns the fleet: one `ManagedWard` (a `WardConnection` +
`TelemetryInfo` + `WardActions` + `CommandExecutor` quartet) per registered
ward, all sharing one `mavsdk::Mavsdk` core, keyed by `ward_id`. It is the
**only** class `main.cpp` (or any future consumer) talks to for ward
interaction: nothing outside `gateway/libs/ward/` constructs or calls
`WardConnection`/`TelemetryInfo`/`WardActions`/`CommandExecutor` directly.
Telemetry publishing, the on-connect `WardInfo` handshake, command routing,
and fleet persistence are all facade methods on this class.

## Responsibilities

- Register/deregister wards by `ward_id`, building and tearing down each
  one's object graph (`add_ward`/`remove_ward`, and the wire-facing
  `handle_add_ward`/`handle_remove_ward` that answer a console's
  `AddWard`/`RemoveWard` with a `WardConfigAck`, and, on success,
  broadcast an INFO `WARD_ADDED`/`WARD_REMOVED` `Event` so every
  connected console, not just the requester, learns the fleet changed).
- Persist the fleet to disk after every successful add/remove, and reload it
  at boot (`load_persisted`/`restore_and_start`) so a gateway that crashes or
  restarts while a ward is airborne reconnects to it instead of forgetting
  it (see Persistence below).
- Keep every ward reconnecting on its own timeline
  (`start`/`start_all`, one `reconnect_worker` `jthread` per ward) so one
  dead SITL container does not stall the rest of the fleet.
- Route an incoming `Command` to the right ward's `CommandExecutor`
  (`dispatch_command`), rejecting with a reason (gateway rule 5) if the
  `ward_id` is unknown, stopped, or mid-transition.
- Take wards offline safely: `stop` (rejects if airborne or link-down,
  ground-safety guarded) and `force_stop` (operator override that commands
  RTL and supervises the flight home before disarming).
- Publish `WardState` for every registered ward at a shared rate
  (`start_publishing`) and send `WardInfo` to a newly connected client
  (`send_ward_info`).
- Answer an upstream envelope from a read-only viewer connection
  (`reject_viewer_envelope`, gateway issue #20) with the same reasoned
  ack/event a normal precondition failure would produce, without ever
  reaching `dispatch_command`/`handle_add_ward`/etc. Deciding *whether* a
  connection is a viewer is `Transport`'s job (`Transport::role()`); this
  class only owns what happens once `main.cpp` has already made that call.

## Explicitly out of scope

- **MAVLink/telemetry/action mechanics themselves.** Delegated entirely to
  `WardConnection`/`TelemetryInfo`/`WardActions`/`CommandExecutor`; this
  class only orchestrates them per ward and never duplicates their logic.
- **Missions.** `Mission`/`MissionProgress` land in M5.
- **Graceful shutdown of a still-flying fleet.** The destructor waits for any
  in-flight transition to quiesce before tearing down, but does not
  proactively `force_stop_all()` a fleet that was never told to stop; that
  needs `main.cpp` to call it before dropping the `WardManager`, which
  isn't wired up yet (no signal handling exists in `main.cpp` today).
- **A relay transport.** `WardManager` talks to whatever `Transport&` it's
  given; it has no idea whether that's the plain WebSocket server or the M4
  relay transport.

## Public API

| Member | Behavior |
|---|---|
| `static unique_ptr<WardManager> create(Transport&, filesystem::path persistence_path = {})` | Builds the shared `Mavsdk` core itself. The entry point every consumer outside this library should use. |
| `add_ward(const WardConfig&)` | Builds the object graph and registers it; does not connect. Rejects an empty or already-registered `ward_id`, or a nonzero `system_id` already bound elsewhere. |
| `dispatch_command(const Command&)` | Routes to the ward's `CommandExecutor`; synthesizes a REJECTED `CommandAck` for unknown/stopped/busy wards. |
| `handle_mission_upload(const Mission&)` | Routes to the ward's `WardMission::enqueue_upload()`; broadcasts a WARNING `Event` for unknown/stopped/busy wards (no `CommandAck`-shaped ack exists for missions). |
| `handle_mission_file_upload(const MissionFileUpload&)` | Converts via the ward's `MissionImporter::import()` (synchronous - local parsing, not a MAVLink round trip), then routes through `handle_mission_upload()`'s same path on success; a WARNING `Event` on either the ward lookup or the import itself failing. |
| `handle_mission_download_request(const MissionDownloadRequest&)` | Routes to the ward's `WardMission::enqueue_download()`; broadcasts a WARNING `Event` for unknown/stopped/busy wards. Result surfaces later via the publish tick. |
| `start(ward_id)` / `start_all()` | Launches (or relaunches) a ward's `reconnect_worker`. |
| `stop(ward_id)` / `stop_all()` | Takes a ward offline; rejects airborne or link-down (state unknown). |
| `force_stop(ward_id)` / `force_stop_all()` | Operator override: commands RTL, supervises landing, then disarms and stops. |
| `load_persisted()` | Reloads every ward from `persistence_path`; 0 if unset or the file doesn't exist. |
| `restore_and_start()` | `load_persisted()` then `start_all()` - the crash-recovery entry point. |
| `start_publishing(interval = kDefaultPublishInterval)` | Launches the shared telemetry-publish worker. |
| `send_ward_info(client)` | Sends one `WardInfo` per discovered ward to a single client (for `Transport::on_connect`). |
| `is_started`/`is_connected`/`list_status`/`list_ward_ids` | Read-only fleet queries. |
| `remove_ward(ward_id)` / `remove_all()` | Applies `stop`'s ground-safety rules, then erases. |
| `handle_add_ward`/`handle_remove_ward` | Wire entry points: `AddWard`/`RemoveWard` in, `WardConfigAck` out. Never throw. |
| `reject_viewer_envelope(client, Envelope)` | Answers a viewer connection's upstream `Envelope` with a REJECTED `CommandAck`/`WardConfigAck` (plus a WARNING `Event` for `Command`) or a WARNING `Event` (mission family, which has no ack type), always message `"read-only session"`. Never touches `managed_wards_` - it is a protocol-level answer, not a ward operation. |

## Persistence

`persistence_path` (constructor/`create()` parameter, empty = disabled) is
where the fleet is durably recorded, per `fleet.proto`'s own header comment:
*"the gateway owns persistence (its config file becomes the durable form of
these requests)."* Every successful `add_ward`/`remove_ward` rewrites
the whole file (`persist_locked()`, called under `wards_mutex_` - the file
is small and the event is rare/human-triggered, so writing under the lock is
the safer choice over racing two mutations to disk out of order). The file is
YAML (yaml-cpp), one entry per ward: `ward_id`, `connection_url`,
`mavlink_system_id`, `name`, and `ward_class` (via the proto-generated
`WardClass_Name`/`_Parse`).

`load_persisted()` reconstructs each entry via the same `add_ward_impl()`
core `add_ward()` uses, but with persistence suppressed for that call (it
would otherwise rewrite the file once per loaded entry for no reason - the
content on disk hasn't changed). A malformed entry is logged and skipped; the
rest still load. `main.cpp` calls `restore_and_start()` once at boot, before
serving any client: a ward that was mid-flight when the gateway last exited
starts reconnecting immediately, so `force_stop`/`stop` can act on it again
instead of it being silently forgotten.

This one file doubles as the "config file lists wards" mechanism
`BRIEF.md`'s M4 section describes - no separate static-config-loader exists or
is needed. `gateway/config/` is a tracked directory (`.gitkeep`); the
generated `fleet_state.yaml` itself is gitignored.

## Threading

- `wards_mutex_` guards `managed_wards_` and every element's
  `executor`/`busy`/`reconnect_worker`. Lock order: `wards_mutex_` before
  any `WardConnection`/`TelemetryInfo`/`WardActions` internal mutex.
- Each ward's `reconnect_worker` is an independent `jthread`: connects,
  broadcasts an INFO `LINK_CONNECTED` `Event`, requests the telemetry stream
  rate (re-requested on every reconnect - PX4 forgets it across a link drop),
  waits while connected, and on drop broadcasts a WARNING `LINK_LOST` `Event`
  before reconnecting (not on a deliberate `stop()`/`force_stop()` - the loop
  only emits `LINK_LOST` when the drop wasn't requested).
- `handle_add_ward()`/`handle_remove_ward()` broadcast an INFO
  `WARD_ADDED`/`WARD_REMOVED` `Event` (`broadcast_fleet_event()`) on
  their accepted path only, same no-lock-held pattern as
  `broadcast_command_ack()`/`broadcast_link_event()`. A rejection is answered
  by the `WardConfigAck` alone: nothing about the fleet actually changed
  for another console to learn about.
- One shared `publish_worker_` (not one per ward: the per-tick work is
  cheap cached-getter reads, not I/O, so batching it on one thread is simpler
  and just as fast) builds every ward's `WardState` under the lock, then
  releases it before broadcasting - broadcasting under the lock would stall
  every other `WardManager` call behind one slow client for the whole
  tick, the same hazard `dispatch_command`'s ack path and
  `send_ward_info`'s blocking firmware query are equally careful to avoid.
- The same tick also polls each ward's `WardMission` (BRIEF.md M5):
  `get_progress()` into a `mission_progress` frame (only once something has
  actually been uploaded), `take_pending_return_to_launch()` (deferred into
  an unlocked synthetic `ReturnToLaunchCommand` enqueue, using the same
  busy-then-unlocked pattern as `dispatch_command`, since a concurrent
  `remove_ward` could otherwise destroy the executor mid-call),
  `take_upload_result()`/`take_download_result()` (deferred into
  `broadcast_mission_event()`/`broadcast_mission_download()` calls after the
  lock releases, for the same blocking-socket-write reason).
- Both worker types are declared after `managed_wards_` (`publish_worker_`
  last of all), so C++'s reverse-declaration-order destruction stops and
  joins every thread before the wards they read are torn down. The
  destructor additionally blocks until no ward is `busy`, since
  `force_stop()` spends up to two minutes unlocked holding a raw
  `ManagedWard*`.

## Automated tests

`gateway/tests/ward/ward_manager_test.cpp` (GoogleTest, no SITL/Docker
container - most tests never call `connect()` at all, and the ones that do
connect to an in-process fake `Mavsdk` core over loopback instead), against a
`FakeTransport` that records `broadcast()`/`send()` calls instead of touching
a real socket:

- `AddWardRegistersAndListsId`, `AddWardRejectsEmptyId`,
  `AddWardRejectsDuplicateId`, `AddWardRejectsDuplicateSystemId`.
- `DispatchCommandRejectsUnknownWardWithReason`: the broadcast `CommandAck`
  carries the offending id in its message.
- `RemoveWardRemovesNeverStartedWard`.
- `PersistsOnAddAndRoundTripsOnReload`: two wards, distinct
  `ward_class`/`name`/`system_id`; a fresh manager pointed at the same file reloads
  both, and a `system_id` collision against a reloaded ward is still
  rejected (proves `system_id` itself survived the round trip).
- `RemovePersistsRemoval`, `LoadPersistedOnMissingFileReturnsZero`,
  `MalformedEntryIsSkippedNotFatal`, `LoadPersistedDoesNotRewriteFile`.
- `HandleAddWardAcceptsPersistsStartsAndEmitsEvent`: an `AddWard`
  request is ACCEPTED, persisted, starts the reconnect worker, and broadcasts
  a `WARD_ADDED` `Event`.
- `HandleAddWardRejectsDuplicateIdWithReasonAndNoEvent`,
  `HandleRemoveWardRejectsUnknownWardWithReason`: REJECTED acks carry a
  reason and broadcast no fleet-change `Event`.
- `HandleRemoveWardAcceptsPersistsRemovalAndEmitsEvent`: mirrors the add
  case for `RemoveWard`.
- `HandleRemoveWardRejectsWhileAirborne`: against a `FakeInAirAutopilot` (a
  second in-process `Mavsdk` core, same "fake autopilot" pattern as
  `ward_connection_test.cpp`, that continuously republishes
  `EXTENDED_SYS_STATE`/`InAir` via the `TelemetryServer` plugin) - a real
  connect, then a `RemoveWard` for a genuinely airborne ward comes back
  REJECTED ("ward is in the air") and the ward stays registered.
- `RejectViewerEnvelopeRejects{Command,AddWard,RemoveWard,MissionUpload,
  MissionDownloadRequest}With{Ack,Event}`: one test per upstream `Envelope`
  kind, each asserting the reasoned rejection's exact shape (`CommandAck`'s
  companion `Event`, message always `"read-only session"`) and that the
  ward-level state (`list_ward_ids()`, an existing ward's
  registration) never changed - proof the envelope never reached
  `dispatch_command`/`handle_add_ward`/etc.

## Manual verification

With PX4 SITL running (`gateway/CLAUDE.local.md` has the full command list),
build and run the gateway:

```
cmake -S gateway -B gateway/build
cmake --build gateway/build
./gateway/build/src/karshipta_gateway
```

First run: log shows `ward 'sitl-1' registered` (the seeded default) and
`gateway/config/fleet_state.yaml` is created with that entry. Kill the process
and rerun it: the log now shows `loaded 1 ward(s) from
'gateway/config/fleet_state.yaml'` instead of re-seeding, and the reconnect
loop resumes on its own.

Connect a WebSocket test client and send an `AddWard` for a second SITL
instance (port 14541): a `WardConfigAck` comes back ACCEPTED and
`fleet_state.yaml` now lists two wards. Send a `Command` for each
`ward_id` and confirm each is routed to the right ward. Send
`RemoveWard` for one (after `stop`ping it, or while it's still on the
ground) and confirm the file drops back to one entry.
