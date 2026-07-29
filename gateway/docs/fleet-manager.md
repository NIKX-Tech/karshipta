# FleetManager

`libs/fleet/include/fleet_manager.h`, `libs/fleet/src/fleet_manager.cpp`
(persistence: `libs/fleet/include/fleet_zone_store.h`,
`libs/fleet/src/fleet_zone_store.cpp`, `libs/fleet/include/fleet_mission_store.h`,
`libs/fleet/src/fleet_mission_store.cpp`)


## Overview

`FleetManager` owns Fleet/Zone persistence (via `FleetZoneStore`), Fleet
Mission persistence (via a sibling `FleetMissionStore`), and the wire-level
request/ack translation for all three - the Fleet/Zone/Fleet-Mission
counterpart to `WardManager::handle_add_ward`/`handle_remove_ward`. Kept as
its own class rather than folded into `WardManager`: these are their own
resources with their own stores, and `WardManager` already owns enough
(gateway/CLAUDE.md rule 3, one clear owner per resource).

The two managers meet in two directions:
- `FleetManager`'s `handle_create_fleet_mission`/`handle_stop_fleet_mission`/
  `handle_update_fleet_mission_routes` are handed a `WardManager&` per call
  (never stored) to dispatch per-ward uploads/commands - dispatching is the
  only thing `FleetManager` ever needs from it.
- `WardManager` calls back into `FleetManager` via two observer callbacks
  (`handle_command_outcome`, `handle_mission_upload_outcome`) that
  `main.cpp` registers on it at startup, so a Fleet Mission's per-ward
  status can track a dispatch through to its real outcome without
  `WardManager` needing to know `FleetMission` exists at all.

A Fleet is a named, operator-defined group of wards (many-to-many
membership). A Zone is a named keep-in/keep-out polygon. A Fleet Mission is
a trackable, persisted unit grouping one independently-planned route per
ward - see `docs/fleet-mission-model.md` for the design and why it
replaced an earlier, flat-broadcast approach. All three are saved,
shared-within-one-operator objects, not live telemetry - this is why they
persist to SQLite instead of `WardManager`'s YAML.

## Responsibilities

- Translate each Fleet/Zone CRUD request (`CreateFleet`, `RenameFleet`,
  `DeleteFleet`, `AddWardToFleet`, `RemoveWardFromFleet`, `CreateZone`,
  `UpdateZone`, `DeleteZone`) into a `FleetZoneStore` call and a correlated
  `FleetAck`/`ZoneAck`. On success, create/rename/membership-change handlers
  also broadcast the affected `Fleet`/`Zone` to every connected client (not
  just the requester); delete has no updated object to send, so `main.cpp`
  broadcasts the returned ack itself, same as it already does for
  `WardConfigAck`.
- Create, stop, remove, and edit a Fleet Mission
  (`handle_create_fleet_mission`/`handle_stop_fleet_mission`/
  `handle_remove_fleet_mission`/`handle_update_fleet_mission_routes`),
  dispatching one independent upload or command per ward rather than one
  shared broadcast - see "Fleet Mission handlers" below.
- Track a dispatched Fleet Mission command/upload through to its real
  outcome (`handle_command_outcome`/`handle_mission_upload_outcome`,
  registered by `main.cpp` as `WardManager` observers) and update the
  affected ward's `WardMissionState` accordingly.
- Answer an upstream envelope from a read-only viewer connection
  (`reject_viewer_envelope`, gateway issue #20) with the same reasoned
  ack a normal precondition failure would produce - a real
  `FleetMissionAck` for all four Fleet Mission request kinds, not a
  generic fallback event.
- Send one `Fleet`/`Zone` envelope per persisted entity
  (`send_fleet_zone_snapshot`) and one `FleetMission` envelope per
  persisted mission (`send_fleet_mission_snapshot`) to a newly connected
  client, mirroring `WardManager::send_ward_info`.

## Fleet Mission handlers

- **`handle_create_fleet_mission(CreateFleetMission, WardManager&)`**
  (MAVLink builds only). Validates `fleet_id` (if set) exists and
  `ward_plans` is non-empty, persists the mission, then for each
  `ward_plan` builds that ward's own `Mission` (fresh `mission_id`, that
  ward's own items, shared `repeat_count`) and calls
  `ward_manager.dispatch_mission_upload_and_start()`. That call's
  synchronous return (`nullopt` = accepted, a reason = immediate rejection)
  becomes the ward's initial `WardMissionState` (`UPLOADING` or
  `REJECTED`); the real upload outcome lands later via
  `handle_mission_upload_outcome()`. `ACCEPTED` on the returned
  `FleetMissionAck` means "the mission was created," not "every ward's
  upload succeeded" - per-ward outcomes live in `ward_states()`.
- **`handle_stop_fleet_mission(StopFleetMission, WardManager&)`** (MAVLink
  builds only). Defaults an unspecified `action` to RTL, and for every ward
  not already `STOPPED`/`REJECTED`/`STOPPING`, dispatches the corresponding
  `Command` (`rtl`/`pause_mission`/`land`) via `ward_manager.dispatch_command()`.
  Writes that ward's `WardMissionState` to `STOPPING` **before** dispatching,
  not after: `dispatch_command()` can reject synchronously (unknown/stopped/
  busy ward), which - via the observer callback below - can call back into
  `handle_command_outcome()` before `dispatch_command()` even returns.
  Writing `STOPPING` first means that callback always sees, and correctly
  overwrites, the state this loop just wrote; the reverse order would let a
  synchronous rejection be silently clobbered by the `STOPPING` write that
  logically happened first. A `PendingStop{fleet_mission_id, ward_id}` is
  recorded (keyed by the synthesized `Command.command_id`) before dispatch,
  for the same reason.
- **`handle_remove_fleet_mission(RemoveFleetMission)`** - no `WardManager&`
  needed. Safety-gated: rejects unless every `ward_state.status()` is
  `STOPPED` or `REJECTED` (never started). Deliberately does **not** re-verify
  against live MAVSDK state the way `WardManager::remove_ward_impl` does:
  deleting a `FleetMission` record only touches persisted tracking rows,
  never a ward's connection or flight state - the safety-critical check
  already happened inside `handle_stop_fleet_mission`'s `dispatch_command`
  calls.
- **`handle_update_fleet_mission_routes(UpdateFleetMissionRoutes, WardManager&)`**
  (MAVLink builds only, "Edit"). Same STOPPED/REJECTED gate as Remove, then
  replaces every `ward_plan` and re-runs the same upload-and-start loop
  `handle_create_fleet_mission` uses: submitting an edit re-plans **and**
  re-dispatches, it does not just save a draft.

## Explicitly out of scope

- **Zone geometry validation beyond vertex count.** `handle_create_zone`
  rejects fewer than 3 vertices; it does not check for self-intersection or
  validate the polygon is simple. Geometry is immutable after create -
  `handle_update_zone` can only change name/type/altitude bounds, matching
  the console's editing scope.
- **Whether a mission actually respects a Zone.** `FleetManager` persists
  Zones and lets the console/gateway read them; it does not itself check a
  Fleet Mission's `WardMissionPlan` waypoints against any Zone (advisory,
  console-side only - see `docs/fleet-mission-model.md`).
- **Ward object graph mechanics.** Delegated entirely to `WardManager`; this
  class only calls `dispatch_mission_upload_and_start()`/`dispatch_command()`
  on it, and never touches a ward's `CommandExecutor`/`WardConnection`
  directly.
- **Interrupting a mission for reasons other than Stop.** Land/RTL/goto/
  forced-disarm issued directly against a ward (not through a Fleet
  Mission) interrupt that ward's own active mission via
  `WardMission::notify_interrupted()` (wired in `CommandExecutor::dispatch()`),
  but do not themselves update any `FleetMission`'s `WardMissionState` -
  only a `StopFleetMission` dispatch is tracked that way.

## Public API

| Member | Behavior |
|---|---|
| `FleetManager(Transport&, filesystem::path db_path, filesystem::path fleet_mission_db_path)` | Opens (or creates) two SQLite databases - `db_path` via `FleetZoneStore`, `fleet_mission_db_path` via `FleetMissionStore`; `":memory:"` works for tests, and production always passes two distinct real paths. |
| `handle_create_fleet`/`handle_rename_fleet`/`handle_delete_fleet` | Wire entry points: request in, `FleetAck` out. Never throw - see Error handling below. |
| `handle_add_ward_to_fleet`/`handle_remove_ward_from_fleet` | Idempotent membership changes; `FleetAck` out. |
| `handle_create_zone`/`handle_update_zone`/`handle_delete_zone` | Wire entry points: request in, `ZoneAck` out. `handle_create_zone` additionally rejects fewer than 3 vertices before ever calling the store. |
| `handle_create_fleet_mission`/`handle_stop_fleet_mission`/`handle_update_fleet_mission_routes` | MAVLink builds only (`KARSHIPTA_GATEWAY_ENABLE_MAVLINK`), each takes a `WardManager&`. See "Fleet Mission handlers" above. `FleetMissionAck` out. |
| `handle_remove_fleet_mission` | Always available (no `WardManager&` needed). `FleetMissionAck` out. |
| `handle_command_outcome(CommandAck)` | Registered by `main.cpp` as `WardManager::set_command_outcome_observer`'s target. Only acts on a terminal ack (`SUCCESS`/`REJECTED`/`TIMEOUT`) whose `command_id` matches a `StopFleetMission` dispatch this class issued; anything else is silently ignored. |
| `handle_mission_upload_outcome(ward_id, mission_id, success, message)` | Registered by `main.cpp` as `WardManager::set_mission_upload_outcome_observer`'s target. Scans persisted Fleet Missions for the one whose `ward_state` for `ward_id` is `UPLOADING` with this exact `mission_id`, and flips it to `ACTIVE`/`REJECTED`. |
| `reject_viewer_envelope(client, Envelope)` | Answers a viewer connection's upstream Fleet/Zone/Fleet-Mission envelope with a REJECTED `FleetAck`/`ZoneAck`/`FleetMissionAck`, always message `"read-only session"`. |
| `send_fleet_zone_snapshot(client)` | Sends one `Fleet` envelope per fleet and one `Zone` envelope per zone to exactly this client. |
| `send_fleet_mission_snapshot(client)` | Sends one `FleetMission` envelope per persisted mission to exactly this client. Both snapshot methods wire to `Transport::on_connect` alongside `WardManager::send_ward_info`. |

## Error handling

Every `handle_*` method wraps its **entire body**, not just the initial
store call, in one `try`/`catch (const std::exception&)`. This matters
because several handlers call back into a store a second time on the
success path (e.g. `find_fleet()`/`find_zone()`, or re-reading a
`FleetMission` to broadcast it) - wrapping only the first call left that
second call able to throw uncaught. A caught exception becomes the same
`REJECTED` ack shape a normal validation failure already produces, with
`"internal error: "` plus `error.what()` as the message, and an
`spdlog::error` line. `main.cpp`'s envelope switch has no try/catch of its
own around any `FleetManager` call: this class's own contract (gateway
rule 5, "never throw on bad input") is what keeps a single bad request - a
full disk, a permissions problem, a corrupt row - from taking the whole
gateway process down, same guarantee `WardManager::handle_add_ward`
already gives.

`send_fleet_zone_snapshot()`/`send_fleet_mission_snapshot()` have no ack
to reject (they run from `Transport::on_connect`, before any request
exists), so a store failure there is logged and that one client's
snapshot is skipped, rather than propagating and dropping the new
connection entirely.

`FleetZoneStore`/`FleetMissionStore` themselves are unaffected by any of
this: their own documented contract (see their headers) is to keep
throwing on a genuine SQLite failure - `FleetManager` is the boundary that
converts that throw into an observable, per-request rejection instead of a
process crash.

## Concurrency

`FleetZoneStore` and `FleetMissionStore` each hold their own `mutable
std::mutex mutex_`, taken for the entire body of every public method.
`Transport::on_receive` fires on a per-connection worker thread, one
thread per connection, so two clients mutating state concurrently is a
real scenario, not a hypothetical. Coarse-grained (one mutex per store, not
per-table/per-row) deliberately: a handful of rows at this scale, not a hot
path. This also closes a real TOCTOU gap `add_ward_to_fleet()`/
`remove_ward_from_fleet()` otherwise have (existence check, then a separate
mutating statement): a concurrent `delete_fleet()` between the two could
otherwise turn the mutation into a foreign-key violation.

`create_zone()`'s zone-row insert and per-vertex insert loop, and
`FleetMissionStore::create_fleet_mission()`'s mission-row-plus-per-ward-
plan-plus-per-ward-state inserts, each additionally run inside a single
SQLite transaction (an RAII `Transaction` type, duplicated per store file
rather than shared via a header, matching this repo's existing
convention). Without this, a failure partway through the insert loop would
leave a partially-committed, corrupted record permanently in the database.

`FleetManager` itself additionally holds a `pending_stops_mutex_` guarding
`pending_stops_` (the `Command.command_id -> {fleet_mission_id, ward_id}`
correlation map `handle_stop_fleet_mission`/`handle_command_outcome` share)
- this is `FleetManager`-only bookkeeping, not persisted state, so it gets
its own lock rather than reusing either store's.

## Automated tests

`gateway/tests/fleet/fleet_zone_store_test.cpp` (GoogleTest, `:memory:`
database unless noted): create/rename/delete/idempotent-membership/
multi-fleet-membership for Fleet; create/update/delete/vertex-ordering for
Zone; a separate real-file-backed `FleetZoneStorePersistenceTest` suite
covers round-tripping across two store instances pointed at the same file,
plus a read-only-directory test proving `create_zone()`'s transaction
actually rolls back on a mid-write failure.

`gateway/tests/fleet/fleet_mission_store_test.cpp`: the same shape of
coverage for `FleetMissionStore` - create/get/list, per-ward state and
aggregate status updates, `update_ward_plans` (Edit) replacing routes and
resetting state, delete, and a matching persistence/read-only-directory
suite.

`gateway/tests/fleet/fleet_manager_test.cpp` (`FakeTransport`, plus a real
`WardManager` wired with its own command-outcome/mission-upload-outcome
observers pointed at the `FleetManager` under test, mirroring exactly how
`main.cpp` wires the two together): accept/reject paths for every Fleet/
Zone/Fleet-Mission handler, viewer rejection, and the connect-time
snapshots. Fleet Mission coverage specifically includes: an ad-hoc
selection with unregistered wards settling every `ward_state` to
`REJECTED`; Stop skipping an already-`REJECTED` ward rather than
re-dispatching to it; Remove accepting once every ward is
`STOPPED`/`REJECTED` and rejecting while a registered-but-unconnected ward
is still `UPLOADING`; Edit rejecting under that same "still active" gate.
`FleetManagerCrashSafetyTest.StoreFailureBecomesRejectedAckInsteadOfAnUncaughtException`
uses a read-only-directory technique against a real file-backed
`FleetManager` to prove a store failure surfaces as a `REJECTED` ack
rather than an uncaught exception escaping the handler.
