# Console UX direction

The console is an instrument panel, not a website: dark surfaces, 1px borders, one amber accent, functional colors only for meaning (green armed, red critical, blue selected). This document fixes the shell layout and where upcoming features slot in, so every milestone lands in a predictable place.

## Shell layout

Two collapsible vertical icon rails, one on each side of the map. Each
rail stays pinned to its own screen edge whether collapsed or expanded;
its content panel grows on the inboard side only, never displacing the
rail itself.

```
+--------------------------------------------------------------------------+
| # KARSHIPTA                                        WARDS 3     * LIVE    |  top bar
+----+------------------+-------------------------------+------------------+----+
| =  | + Add Ward        |                               | sitl-1 status    | o  |
|    | + New Fleet         |                             |------------------| -> |
|    |                      |            MAP               | COMMANDS         | [] |
|    | v FLEET.Alpha (2)      |         (markers)            | arm takeoff goto |    |
|    |   sitl-1  sitl-2         |                            | land rtl disarm  |    |
|    |                            |              +-------+   | MISSION          |    |
|    | v UNASSIGNED (1)             |            | EVENTS|   |                  |    |
|    |   sitl-3                       |           +-------+   |                  |    |
+----+----------------------------------+---------------------+------------------+----+
 icon    left rail content                     map              right panel      icon
 rail    (ward directory, Fleet                                content            rail
         CRUD lives here too)                                  (Ward / Mission /
                                                                 Zones tabs)
```

- **Top bar**: identity, ward count (`WARDS n` - not fleet count; "Fleet" is reserved for the grouping feature below, see "Fleet grouping"), a clickable gateway status button (LIVE / CONNECTING / DOWN) that opens the connection panel. The one place the amber pulse always lives.
- **Left rail**: a directory-toggle icon expands/collapses the ward list (default expanded). Ward cards grouped by Fleet - a Ward in more than one Fleet appears under each group, an always-present Unassigned group covers the rest, hidden entirely once nothing is left in it. Fleet create/rename/delete and membership editing live here too, next to the wards they group. Click a card selects; selection is blue everywhere (card, marker, ring).
- **Map (center)**: fills all remaining space. Markers are amber arrows rotating with heading; labels stay upright. Heading applies to any moving ward, not just flight ones. Goto targeting, mission-waypoint placement, and zone-vertex placement all arm a crosshair, mutually exclusive with each other (zone drawing takes priority over every other click-driven tool). An empty-state overlay with the three onboarding actions renders here when the fleet is empty, except while a zone is being drawn (drawing a safety zone doesn't require any ward to exist first).
- **Right panel**: an icon rail (Ward / Mission / Zones) stays reachable regardless of whether a ward is selected. Starts collapsed; selecting a ward auto-expands to the Ward tab. A small always-visible status strip (id, mode, armed, link, battery) sits above the tabs whenever a ward is selected, regardless of which tab is active, so critical status is never hidden behind Mission or Zones.
  - **Ward tab**: full telemetry of the selected ward, then the command dock, then command trackers, then the mission panel. The command dock and mission panel only render for a ward with a `flight` state; a non-flight ward (e.g. a tag) shows telemetry only.
  - **Mission tab**: pick a Fleet or an ad-hoc set of individual wards, plan a route by clicking the map, assign. See "Fleet-level missions" in `fleet-mission-model.md`.
  - **Zones tab**: draw, list, and edit keep-in/keep-out safety polygons. See "Zones" below.
- **Events (overlay, bottom right of map)**: fleet-wide feed with severity dots and mono timestamps.

## Where the next features go

| Milestone | Slot |
|---|---|
| Fleet grouping (shipped) | left rail, next to the ward list it groups |
| Mission assignment (shipped) | right panel MISSION tab; waypoints draw on the map as a dashed blue route with numbered points |
| Zones (shipped) | right panel ZONES tab; saved zones and the in-progress draft each get their own map source/layer, distinct from the read-only airspace layer |
| Telemetry charts / log review (v0.2) | bottom drawer under the map, collapsed by default |
| Video feeds (later) | Ward tab top, above telemetry |

## Ward onboarding (C7)

The console opens empty: no automatic fleet, no ward until an explicit UI action. Two independent channels feed the fleet, and every ward in `fleet.wards` carries which one it came from (`source: 'demo' | 'gateway'`):

- **Demo engine**: always available, pure client-side (`FakeGateway`), zero network. "Add demo ward" spawns one instantly with a procedurally varied patrol; ids are `demo-1`, `demo-2`, ... Removing one is instant and local. This is also the console's test harness; it is not a toy to delete, only to stop treating as the default. Every demo ward is a flight-capable multirotor today; there is no non-flight demo path yet.
- **Gateway channel**: opt-in. The connection panel (opened from the top bar's status button) holds the WebSocket URL (remembered in `localStorage`, never auto-connected) and a Connect/Disconnect action. Both "Add simulated ward" (a PX4 SITL preset, prefilled `udpin://0.0.0.0:14540`) and "Connect real ward" (blank form: id, name, ward class, connection URL, MAVLink system id) send the same `AddWard` envelope to the connected gateway and track the `WardConfigAck` lifecycle (pending / rejected with reason / accepted). The ward class dropdown lists every `WardClass`, not just flight-capable ones: the connect mechanism is MAVLink, which isn't flight-exclusive, so a MAVLink-speaking tracker or tag connects through this exact same dialog. The gateway sees no difference between "simulated" and "real"; the distinction is console-side framing only. Adding a second or later simulated ward in one session shows a resource-warning confirm first (each is a full autopilot build).
- Disconnecting the gateway removes only `source: 'gateway'` wards; demo wards are untouched. Removing a ward is guarded: disabled while armed or airborne, on both channels.
- `PUBLIC_GATEWAY_WS_URL` and `PUBLIC_READONLY` are automation overrides only (docker, CI), not the default experience: when set, the console auto-connects on load; when unset, connecting is always a click.

## Viewer mode

`PUBLIC_READONLY=true` makes the session read-only: a VIEWER badge in the top bar, no command dock, no mission panel, telemetry untouched. Client-side only; the gateway-side rejection policy is issue #20.

## Fleet grouping

A Fleet is a named, persistent group of Wards, many-to-many (see
`fleet-mission-model.md`). Managed from the left rail: an inline "+ New
Fleet" popover (name + description, styled and positioned like "+ Add
Ward" right next to it) creates one; each Fleet's row supports
click-to-rename (name and description together, one form, one request -
`RenameFleet` updates both), delete behind a confirm dialog, and a
floating "manage members" popover (a checklist icon, not a plain "+" -
membership can shrink as well as grow) with a checkbox per ward. The
popover overlays the row instead of swapping the ward-card list in place,
which used to shift every row below it as the section's height changed.

## Fleet-wide mission assignment

The right panel's Mission tab: pick an existing Fleet (radio) or check off
individual wards ad hoc - the two are mutually exclusive, picking one
clears the other. "Plan route" arms the same click-to-add-waypoint flow a
solo mission uses, drawn on the same dashed-route map layer (mutually
exclusive with a solo ward's own draft). "Assign" sends one
`FleetMissionAssignment`; the gateway fans it out as an independent
upload-plus-start per ward, the console never loops per-ward uploads
itself.

## Zones

An operator-drawn keep-in/keep-out polygon, distinct at every level from
the read-only airspace layer below - separate store, separate map source/
layer, separate legend, never merged. Right panel's Zones tab: "+ Draw
Zone" arms vertex placement (click the map; a growing dashed line until
the 3rd vertex, then a fillable closed shape), "Finish" once >= 3 vertices
opens an inline name/type/altitude-bounds form. Saved zones list with
inline edit (name/type/altitude only - geometry is fixed at creation) and
delete behind a confirm dialog. Zone-drawing clicks take priority over
every other click-driven tool on the map (goto, mission planning,
measuring): the most "modal" of them, since a half-drawn polygon
interrupted by another tool stealing the click would be awkward to
recover from. v0 is advisory only, same as airspace zones below: a
mission waypoint inside a keep-out Zone, or outside every keep-in Zone
(only checked once at least one exists), warns in the confirm dialog
before upload or assignment, never blocks it.

## Airspace zones

`PUBLIC_OPENAIP_KEY` turns on the geozone layer: translucent zone polygons under the mission route (red prohibited, amber restricted, blue other), a legend at bottom-left of the map, and a non-blocking warning line in the goto and mission-start confirm dialogs when the target crosses a zone. Absent key means no layer, no requests, no legend: silent by design. `src/lib/geozones/` is built behind a `GeozoneSource` interface so an official ED-269 per-country feed can slot in later without touching the map or the dialogs.

## Interaction rules (do not regress)

- Flight-critical or destructive actions (land, RTL, force disarm, goto) always confirm first; Escape cancels without sending.
- Safety commands (land, RTL, disarm) are never disabled by an in-flight command; only same-kind commands debounce.
- Every command shows its lifecycle where it was issued: EXECUTING (amber pulse), SUCCESS (green), REJECTED/TIMEOUT (red with the gateway reason).
- Color is information: amber = brand/live, green = armed/healthy, red = critical, blue = selected. Nothing else is colored.
