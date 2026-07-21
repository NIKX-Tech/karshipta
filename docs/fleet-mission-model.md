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
feature, not to be built now. Assignment does have its own console tab
(Mission, on the right panel - see "Console layout" below): pick a Fleet
*or* an ad-hoc set of individual Wards, plan a route, assign. fleet_id is
optional on the wire for exactly this reason: an ad-hoc selection isn't
tied to any saved Fleet at all.

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

Two collapsible vertical icon rails, one on each side of the map - not the
single right-side panel originally drafted in this section (see "What
shipped differently" below for why that changed).

```
+------------------------------------------------------------------------+
| Top bar: Karshipta logo, WARDS count, connection status, theme         |
+----+-------------------+-----------------------------+-------------+---+
| =  | + Add Ward         |                             | WARD status | o |
|    | + New Fleet          |         Map (MapLibre)      | strip       |->|
|    |                        |         center stage        |-------------|[]|
|    | Fleet groups            |                             | Ward /      |  |
|    | (rename/delete/           |                            | Mission /   |  |
|    | membership), then          |                           | Zones tabs  |  |
|    | Unassigned                  |                          |             |  |
+----+-------------------------------+----------------------------------+---+
 icon    left rail content                map              right panel   icon
 rail    (ward directory, Fleet                            content       rail
         CRUD lives here)
```

**Left rail:** the ward directory, always reachable via a collapsible icon
rail (a directory-toggle icon, plus quick-add for wards and Fleets). Ward
cards grouped by Fleet - a Ward in more than one Fleet appears under each,
an always-present Unassigned group covers the rest. Fleet create/rename/
delete and membership editing live here too, next to the wards they group,
not in a separate tab on the other side of the screen.

**Right panel, Ward tab:** the current selection's detail (telemetry,
commands for flight-capable wards, tags for all wards) - what
ward-detail.svelte showed as an always-visible panel before this reached
implementation (ward-tab.svelte now), plus a small always-visible status
strip (id, mode, armed, link, battery) shown above the tabs whenever a
ward is selected, regardless of which tab is active - this is also this
section's answer to the persistent-telemetry-strip question the original
draft left open.

**Right panel, Mission tab:** pick a Fleet *or* an ad-hoc set of
individual Wards, plan a route by clicking the map, assign - see
"Fleet-level missions" above.

**Right panel, Zones tab:** list, create, edit Zones; the polygon-drawing
tool lives here.

**Map:** always center stage, always shows all Wards and all Zones
regardless of which tab or rail is open; the panels filter what is
highlighted or editable, never what is visible.

### What shipped differently

This section's original draft (single right-side panel, Ward/Fleet/Zones
tabs, mission-assignment folded into the Fleet tab) is superseded by two
changes, both decided directly against the running console rather than on
paper:

- **Two rails, not one.** A single right panel put Fleet CRUD and ward
  browsing in unrelated places - a left aside for browsing wards, a right
  tab for managing the Fleets grouping them. Moving Fleet management into
  the left rail, next to the ward list it groups, removed that split.
- **A Mission tab, not a Fleet tab.** With Fleet CRUD moved left, the
  right side's second tab became "Mission": specifically for planning and
  assigning a mission to a Fleet or an ad-hoc set of wards, not fleet
  management. This is also why FleetMissionAssignment.fleet_id is optional
  on the wire (see "Fleet-level missions" above) - assignment was never
  meant to require a saved Fleet to exist first.

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
