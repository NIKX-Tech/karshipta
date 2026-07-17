# MissionImporter

`libs/vehicle/include/mission_importer.h`, `libs/vehicle/src/mission_importer.cpp`

## Overview

`MissionImporter` converts a customer-supplied mission file (proto
`MissionFileUpload`: a QGroundControl `.plan` or Mission Planner WPL file,
sent as raw text) into an ordinary proto `Mission`, so it can flow through
the exact same `VehicleMission::enqueue_upload()` a console-editor-authored
mission already uses. It wraps MAVSDK's `MissionRaw` plugin, but only for
its `import_*_from_string()` parsers; it never uploads, starts, pauses, or
tracks progress. It reads through a `VehicleConnection&` rather than owning
a `System` itself, and lazily constructs the underlying `mavsdk::MissionRaw`
plugin the first time `import()` is called, once that connection has
succeeded (`ensure_mission_raw()`).

## Responsibilities

- Lazily bind a `mavsdk::MissionRaw` plugin to `VehicleConnection::get_system()`
  once the connection is live.
- Call the right MAVSDK import parser for `MissionFileUpload.format`
  (`import_qgroundcontrol_mission_from_string()` or
  `import_mission_planner_mission_from_string()`).
- Map each raw MAV_CMD mission item onto the one `MissionAction` it can
  represent, rejecting the whole file (not silently dropping the item) when
  a command or coordinate frame has no equivalent.
- Synthesize a `mission_id` for the resulting `Mission` (imported files carry
  no such id themselves).

## Explicitly out of scope

- **Uploading, starting, pausing, or tracking the imported mission.**
  `VehicleMission`'s job, via the exact same `Mission` type this class
  produces; `MissionImporter` never references `VehicleMission`.
- **Mission-level validation** (non-empty `vehicle_id`, RTL only as the last
  item). `VehicleMission::enqueue_upload()`'s own `validate_mission()`
  already enforces these on whatever `Mission` it receives, imported or not;
  duplicating that here would just be two places to keep in sync.
- **Wiring the wire-level `MissionFileUpload`/`MissionDownloadRequest`
  payloads to this class.** `VehicleManager`'s job, not yet implemented.
- **Any command a raw mission item can carry that our schema has no field
  for** (camera/gimbal actions, survey patterns, an embedded
  `DO_CHANGE_SPEED` item). Rejected explicitly, not approximated.

## Public API

| Member | Behavior |
|---|---|
| `explicit MissionImporter(VehicleConnection&)` | Binds to a connection. Does not create the `MissionRaw` plugin yet; that happens lazily on first use. |
| `static std::string result_name(mavsdk::MissionRaw::Result)` | Human-readable text for a `MissionRaw::Result`. |
| `std::pair<std::optional<Mission>, std::string> import(const MissionFileUpload&) const` | Parses and converts. `nullopt` plus a reason on any failure; a `Mission` plus an empty string on success. |

## Design: MAV_CMD to MissionAction, and why some things are rejected

`command.proto`'s `MissionAction` enum only has `WAYPOINT`/`TAKEOFF`/`LAND`/
`RTL`/`HOLD` — a deliberately small subset, matching what the console's own
mission editor can produce (see `vehicle-mission.md`'s `translate()` design).
A raw imported item's `command` field (a MAV_CMD, e.g.
`MAV_CMD_NAV_WAYPOINT=16`) maps onto that subset:

```cpp
MAV_CMD_NAV_WAYPOINT           -> MISSION_ACTION_WAYPOINT
MAV_CMD_NAV_TAKEOFF            -> MISSION_ACTION_TAKEOFF
MAV_CMD_NAV_LAND                -> MISSION_ACTION_LAND
MAV_CMD_NAV_RETURN_TO_LAUNCH    -> MISSION_ACTION_RTL
MAV_CMD_NAV_LOITER_UNLIM,
MAV_CMD_NAV_LOITER_TIME        -> MISSION_ACTION_HOLD
everything else                -> rejected
```

These constants come from `<mavsdk/mavlink/ardupilotmega/ardupilotmega.h>`
(the vendored MAVLink dialect this MAVSDK build generates against), not
invented literals. A QGC survey pattern, camera/gimbal actions, or a
mission-embedded `MAV_CMD_DO_CHANGE_SPEED` item all fall into "everything
else": there's no `MissionItem` field to carry them, so the whole file is
rejected with the offending command and sequence number named in the reason,
rather than silently dropped or approximated.

Coordinate frame is checked the same way: only `MAV_FRAME_GLOBAL_RELATIVE_ALT`
and its scaled variant `MAV_FRAME_GLOBAL_RELATIVE_ALT_INT` are accepted,
since `GeoPoint` only has `altitude_rel_m` for this path — accepting an MSL
frame (`MAV_FRAME_GLOBAL`/`_INT`) and reinterpreting it as relative would
silently send the vehicle to the wrong altitude, so it's rejected instead.

`hold_time_s` and `acceptance_radius_m` come from the raw item's `param1`/
`param2` uniformly; both are genuinely unused (left at `0`) by MAVSDK for
commands where they don't apply (e.g. `TAKEOFF`), so this doesn't need a
per-command special case.

## Design: mission_id is synthesized, not imported

Neither QGC `.plan` nor Mission Planner WPL files carry anything like our
schema's `mission_id`, and `MissionFileUpload` itself has no such field
either. `import()` synthesizes one (`"imported-" + vehicle_id + "-" +
epoch_ms`) — not a real UUID despite the schema commenting `mission_id` as
one, since nothing downstream validates that format and this only needs to
be unique enough for one gateway process to tell imported missions apart.

## Constraints and preconditions

- **Requires a successful `VehicleConnection::connect()` before parsing can
  run**, even though the parse itself is local text processing with no
  MAVLink round trip to the vehicle: MAVSDK's `MissionRaw` plugin must still
  be constructed against a discovered `System`. `import()` returns `nullopt`
  with a "vehicle not connected" reason otherwise, checked only after the
  format itself is confirmed valid (so a malformed request fails without
  needing a connection at all).
- **Does not rebind across reconnects**, same as `TelemetryInfo`/`VehicleMission`.
- **Only mapping-level validation.** A `Mission` returned by `import()` is
  not guaranteed to pass `VehicleMission::enqueue_upload()`'s own
  `validate_mission()` in every respect (e.g. an RTL item is only rejected
  here if unrepresentable, not if it's positioned incorrectly) — that check
  still runs downstream, on this class's output the same as on anything else.

## Automated tests

`gateway/tests/vehicle/mission_importer_test.cpp`, against the same
deliberately-unconnected-`VehicleConnection` strategy used for
`VehicleActions`/`VehicleMission`: an unspecified format rejects without
ever touching MAVSDK; both real formats fail fast at `ensure_mission_raw()`
without a connection. Actual parsing of a real QGC `.plan` or Mission
Planner WPL string, and the MAV_CMD/frame mapping logic itself, is **not**
exercised — there is no SITL harness in this repo, and `MissionRaw`'s import
functions don't need a live vehicle at all, just a constructed plugin
instance, so a future test could exercise the mapping directly with a
hand-written QGC JSON string once that's worth the investment. Same
accepted gap already documented for `VehicleActions`/`VehicleMission`.

## Manual verification

With one PX4 SITL container running and the gateway built (see
`gateway/CLAUDE.local.md`), a throwaway call to `import()` with a real QGC
`.plan` file's contents (export one from QGroundControl, or use one of its
example missions) should log:

```
[info] mission_raw plugin created
```

and return a `Mission` whose `items()` match the file's waypoints; a file
containing a survey pattern or camera action should instead return
`nullopt` with a reason naming the unsupported command and sequence number.
