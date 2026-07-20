# Fleet, Zone, and Console Layout: v0 Design

This document is the missing piece between the schema and the console UI.
It captures decisions already made so implementation work builds toward
the same design instead of improvising. It describes Phase 2 of the Ward
rename plan: none of this exists yet, none of it is part of the Ward
rename itself. Fleet, Zone, and Organization are new entities, not a
rename of something that already exists.

## Fleet

A Fleet is a named, persistent group of Wards. Membership is many to many,
a Ward can belong to more than one Fleet (e.g. "Inspection Team" and
"North Region" at the same time).

v0 scope: create, rename, delete a Fleet, add/remove Wards from it, view
a Fleet's Wards together on the map. Fleet is not itself a physical thing,
it is a saved selection.

Display label: the word "Fleet" shown in the UI must come from a single
configurable string (default "Fleet"), not be hard-coded, so a deployment
serving a non-drone customer (e.g. livestock) can relabel it "Herd"
without touching the schema. The underlying field names (fleet_id,
Fleet message) never change. This part already ships with the Ward rename
itself (PUBLIC_FLEET_LABEL), ahead of the rest of this document.

## Zone

A Zone is a named polygon: keep-in (operational or safe area) or keep-out
(no-fly). v0 scope: draw a polygon on the map (click to add vertices,
close the shape), name it, set its type, save it. Zones are visible on
the map at all times, independent of Fleet or Ward selection.

v0 behavior is advisory, not enforced: if a mission waypoint lands inside
a keep-out Zone or outside a keep-in Zone, the console shows a warning
before upload, but does not block it. Enforcement on the ward itself
(pushing the geofence to the autopilot via MAVSDK) is a later item, tracked
separately, because it requires per-ward-class handling (a flight ward can
receive a MAVLink geofence, a non-flight ward cannot).

## Fleet-level missions

FleetMissionAssignment (proto/karshipta/v1/command.proto, not yet added)
assigns one mission template to a chosen subset of a Fleet's Wards. v0
semantics, confirmed and kept deliberately simple: the gateway uploads an
independent copy of the same mission to each selected Ward and starts them
together. There is no automatic area splitting, role assignment, or
coordination between Wards in v0, and no per-Ward mission variation within
one assignment. A richer version (a different mission per Ward, composed
across multiple Fleets in one assignment) was considered and rejected:
that is real multi-agent task allocation, an explicitly separate, later
feature, not to be built now. Because assignment stays this simple, it
does not need its own tab in the console; it is an action inside the
Fleet tab (pick a Fleet, pick a mission template, pick which of its Wards
receive it).

## Organization and multi-tenancy

Organization stays Enterprise-only (karshipta-cloud), not part of OSS.
Organization-level access scoping is meaningless without user accounts and
login, and OSS is not getting login (matches the project's existing
OSS/Enterprise split, where auth, accounts, and RBAC are already
Enterprise-only). Adding an Organization concept to OSS would exist for no
reason and just be extra surface area. OSS stays single-tenant, with no
Organization concept at all, not even an implicit default row.

karshipta-cloud already has a clean, non-conflicting seam for this: its
existing fleet.ownerId (a direct Firebase-UID foreign key, single owner,
no sharing) generalizes into an orgId plus a membership table. No
Organization or Team concept exists there today; this is new territory,
not a rework of anything already built.

## OSS persistence

Fleet and Zone are named, saved, shared-within-one-operator objects, not
live telemetry, so keeping them in memory only means they vanish on every
gateway restart. OSS gateway gets a narrow SQLite store, specifically for
Fleet and Zone, not a general backend, and no Organization table (per
above, OSS has no Organization concept to persist). This narrowly reopens
the project's "no backend, no database in the MVP" rule, with explicit
sign-off, scoped to exactly these two saved-object types. Live telemetry
keeps flowing gateway to console directly, unchanged.

## Console layout

A single collapsible vertical panel on the right, three tabs:

```
+----------------------------------------------------------+
| Top bar: Karshipta logo, connection status, global actions|
+------------------------------------------------+---------+
|                                                  |  Ward   |
|              Map (MapLibre)                     |  Fleet  |
|              center stage                       |  Zones  |
|                                                  | (tabs,  |
|                                                  | collap- |
|                                                  | sible)  |
+------------------------------------------------+---------+
```

**Ward tab:** the current selection's detail (telemetry, commands for
flight-capable wards, tags for all wards) - what ward-detail.svelte
already shows today, surfaced here instead of a separate always-visible
panel.

**Fleet tab:** list/tree of Fleets, create/rename/delete, add/remove
Wards, and the mission-assignment action described above. No separate
Missions tab.

**Zones tab:** list, create, edit Zones; the polygon-drawing tool lives
here.

**Map:** always center stage, always shows all Wards and all Zones
regardless of which tab is open; the panel filters what is highlighted or
editable, never what is visible.

This supersedes an earlier three-region draft (left panel tree, right
sidebar with separate Missions/Zones/Info tabs, bottom telemetry strip).
Open question, to resolve when this is actually planned in file-level
detail: what happens to a persistent telemetry strip, if anything, given
the Ward tab now covers that role contextually instead.

## What is explicitly out of scope for this phase

- Real multi-agent task allocation or area splitting across a Fleet.
- Geofence enforcement pushed to the autopilot.
- User accounts or login in OSS, and therefore Team membership in OSS
  (meaningless without login).
- Any military-specific classification taxonomy (friendly/hostile/etc).
  Wards carry a generic, operator-defined `tags` list instead; see
  telemetry.proto.
- Livestock tag ingestion or any non-MAVLink data source. A ward that
  happens to be a MAVLink-capable tracker can be connected exactly like a
  flight ward (see the Ward rename plan's "Connect real ward" dialog
  scope); a genuinely non-MAVLink data source is a different, later
  problem.
