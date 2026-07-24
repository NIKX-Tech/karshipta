# FleetManager

`libs/fleet/include/fleet_manager.h`, `libs/fleet/src/fleet_manager.cpp`
(persistence: `libs/fleet/include/fleet_zone_store.h`,
`libs/fleet/src/fleet_zone_store.cpp`)


## Overview

`FleetManager` owns Fleet/Zone persistence (via one `FleetZoneStore`) and the
wire-level request/ack translation for them - the Fleet/Zone counterpart to
`WardManager::handle_add_ward`/`handle_remove_ward`. Kept as its own class
rather than folded into `WardManager`: Fleet/Zone are their own resource with
their own store, and `WardManager` already owns enough (gateway/CLAUDE.md
rule 3, one clear owner per resource). The one place the two managers meet is
`handle_fleet_mission_assignment()`, handed a `WardManager&` per call rather
than storing one, since fanning a mission out to wards is the only thing
`FleetManager` ever needs from it.

A Fleet is a named, operator-defined group of wards (many-to-many
membership). A Zone is a named keep-in/keep-out polygon. Both are
saved, shared-within-one-operator objects, not live telemetry - this is why
they persist to SQLite (`FleetZoneStore`) instead of `WardManager`'s YAML:
Fleet<->ward membership is many-to-many and Zone vertices are ordered, both
more naturally relational than a flat list.

## Responsibilities

- Translate each Fleet/Zone CRUD request (`CreateFleet`, `RenameFleet`,
  `DeleteFleet`, `AddWardToFleet`, `RemoveWardFromFleet`, `CreateZone`,
  `UpdateZone`, `DeleteZone`) into a `FleetZoneStore` call and a correlated
  `FleetAck`/`ZoneAck`. On success, create/rename/membership-change handlers
  also broadcast the affected `Fleet`/`Zone` to every connected client (not
  just the requester), so state stays in sync across more than one open
  console; delete has no updated object to send, so `main.cpp` broadcasts
  the returned ack itself, same as it already does for `WardConfigAck`.
- Fan a fleet-wide mission out to each selected ward as an independent
  upload + start (`handle_fleet_mission_assignment`, MAVLink builds only -
  see below).
- Answer an upstream envelope from a read-only viewer connection
  (`reject_viewer_envelope`, gateway issue #20) with the same reasoned
  ack/event a normal precondition failure would produce.
- Send one `Fleet`/`Zone` envelope per persisted entity to a newly connected
  client (`send_fleet_zone_snapshot`), mirroring `WardManager::send_ward_info`.

## Explicitly out of scope

- **Zone geometry validation beyond vertex count.** `handle_create_zone`
  rejects fewer than 3 vertices; it does not check for self-intersection or
  validate the polygon is simple. Geometry is immutable after create -
  `handle_update_zone` can only change name/type/altitude bounds, matching
  the console's v0 editing scope.
- **Whether a mission actually respects a Zone.** `FleetManager` persists
  Zones and lets the console/WardManager read them; it does not itself check
  a `FleetMissionAssignment`'s waypoints against any Zone.
- **Ward object graph mechanics.** Delegated entirely to `WardManager`; this
  class only calls `dispatch_mission_upload_and_start()` on it, once per
  selected ward, and never touches a ward's `CommandExecutor`/
  `WardConnection` directly.

## Public API

| Member | Behavior |
|---|---|
| `FleetManager(Transport&, filesystem::path db_path)` | Opens (or creates) the SQLite database at `db_path` via `FleetZoneStore`; `":memory:"` works for tests. |
| `handle_create_fleet`/`handle_rename_fleet`/`handle_delete_fleet` | Wire entry points: request in, `FleetAck` out. Never throw - see Error handling below. |
| `handle_add_ward_to_fleet`/`handle_remove_ward_from_fleet` | Idempotent membership changes; `FleetAck` out. |
| `handle_create_zone`/`handle_update_zone`/`handle_delete_zone` | Wire entry points: request in, `ZoneAck` out. `handle_create_zone` additionally rejects fewer than 3 vertices before ever calling the store. |
| `handle_fleet_mission_assignment(request, WardManager&)` | MAVLink builds only (`KARSHIPTA_GATEWAY_ENABLE_MAVLINK`). No dedicated ack type exists for this request, mirroring solo `Envelope.mission_upload`, which has none either: a whole-request rejection (unknown `fleet_id`, empty `ward_ids`) becomes a gateway-level WARNING `Event` (`Event.ward_id` empty); each ward's own upload/start outcome surfaces through `WardManager`'s existing `CommandAck`/`Event` channels, exactly as it would for a solo mission. |
| `reject_viewer_envelope(client, Envelope)` | Answers a viewer connection's upstream Fleet/Zone/mission-assignment envelope with a REJECTED `FleetAck`/`ZoneAck` (or a WARNING `Event` for `FleetMissionAssignment`, which has no ack type), always message `"read-only session"`. |
| `send_fleet_zone_snapshot(client)` | Sends one `Fleet` envelope per fleet and one `Zone` envelope per zone to exactly this client. Wire this to `Transport::on_connect` alongside `WardManager::send_ward_info`. |

## Error handling

Every `handle_*` method wraps its **entire body**, not just the initial
store call, in one `try`/`catch (const std::exception&)`. This matters
because several handlers call back into the store a second time on the
success path (`find_fleet()`/`find_zone()` to build the post-mutation
broadcast) - wrapping only the first call left that second call able to
throw uncaught. A caught exception becomes the same `REJECTED` ack shape a
normal validation failure already produces, with `"internal error: "` plus
`error.what()` as the message, and an `spdlog::error` line. `main.cpp`'s
envelope switch has no try/catch of its own around any `FleetManager` call:
this class's own contract (gateway rule 5, "never throw on bad input") is
what keeps a single bad request - a full disk, a permissions problem, a
corrupt row - from taking the whole gateway process down, same guarantee
`WardManager::handle_add_ward` already gives.

`send_fleet_zone_snapshot()` has no ack to reject (it runs from
`Transport::on_connect`, before any request exists), so a store failure
there is logged and that one client's snapshot is skipped, rather than
propagating and dropping the new connection entirely.

`FleetZoneStore` itself is unaffected by any of this: its own documented
contract (see its header) is to keep throwing on a genuine SQLite failure -
`FleetManager` is the boundary that converts that throw into an observable,
per-request rejection instead of a process crash.

## Concurrency

`FleetZoneStore` holds one `mutable std::mutex mutex_`, taken for the entire
body of every public method (create/rename/delete/list, for both Fleet and
Zone). `Transport::on_receive` fires on a per-connection worker thread, one
thread per connection, so two clients mutating Fleet/Zone state concurrently
is a real scenario, not a hypothetical. Coarse-grained (one mutex for the
whole store, not per-table/per-row) deliberately: a handful of rows at this
scale, not a hot path. This also closes a real TOCTOU gap
`add_ward_to_fleet()`/`remove_ward_from_fleet()` otherwise have (existence
check, then a separate mutating statement): a concurrent `delete_fleet()`
between the two could otherwise turn the mutation into a foreign-key
violation. One lock around each method makes that interleaving impossible.

`create_zone()`'s zone-row insert and per-vertex insert loop additionally run
inside a single SQLite transaction (an RAII `Transaction` type in
`fleet_zone_store.cpp`'s anonymous namespace: `BEGIN` in its constructor,
`COMMIT` in `commit()`, a best-effort `ROLLBACK` in its destructor if
`commit()` was never reached). Without this, a failure partway through the
vertex loop left the zone row committed with an incomplete vertex list - a
corrupted polygon permanently in the database. Every other mutating method is
already a single SQL statement, so no other method needs this.

## Automated tests

`gateway/tests/fleet/fleet_zone_store_test.cpp` (GoogleTest, `:memory:`
database unless noted): create/rename/delete/idempotent-membership/
multi-fleet-membership for Fleet; create/update/delete/vertex-ordering for
Zone; a separate real-file-backed `FleetZoneStorePersistenceTest` suite
covers round-tripping across two store instances pointed at the same file,
plus `CreateZoneOnReadOnlyDirectoryThrowsWithoutPartialWrite` (makes the
containing directory read-only, so SQLite's rollback-journal creation fails
on every write attempt - a permission bit on the database file itself is not
enough, since POSIX permission checks happen at `open()`, not on every write
to an already-open descriptor - then asserts `create_zone()` still throws
*and* `list_zones()` shows nothing partially written, proving the
transaction actually rolls back).

`gateway/tests/fleet/fleet_manager_test.cpp` (`FakeTransport`, same pattern
as `ward_manager_test.cpp`): accept/reject paths for every handler, the
mission-assignment fan-out (unknown fleet, empty ward selection, an ad-hoc
selection with no `fleet_id`, per-ward rejection), viewer rejection, and the
connect-time snapshot. `FleetManagerCrashSafetyTest.
StoreFailureBecomesRejectedAckInsteadOfAnUncaughtException` uses the same
read-only-directory technique against a real file-backed `FleetManager` to
prove a store failure surfaces as a `REJECTED` ack rather than an uncaught
exception escaping the handler (which gtest would otherwise report as a
crash, not an assertion failure).
