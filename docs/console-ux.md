# Console UX direction

The console is an instrument panel, not a website: dark surfaces, 1px borders, one amber accent, functional colors only for meaning (green armed, red critical, blue selected). This document fixes the shell layout and where upcoming features slot in, so every milestone lands in a predictable place.

## Shell layout

```
+------------------------------------------------------------------+
| # KARSHIPTA                              FLEET 3      * LIVE     |  top bar
+-----------+---------------------------------------+--------------+
| sitl-1  * |                                       | sitl-1       |
| sitl-2  o |                                       | telemetry    |
| sitl-3  o |                MAP                    | (full)       |
|           |             (markers)                 |--------------|
| fleet     |                                       | COMMANDS     |
| list,     |                                       | arm takeoff  |
| compact   |                                       | goto land    |
|           |                    +---------------+  | rtl disarm   |
|           |                    |    EVENTS     |  | trackers     |
|           |                    +---------------+  |              |
+-----------+---------------------------------------+--------------+
```

- **Top bar**: identity, fleet count, a clickable gateway status button (LIVE / CONNECTING / DOWN) that opens the connection panel. The one place the amber pulse always lives.
- **Left rail (16rem)**: the whole fleet at a glance. Compact cards: id, DEMO badge when applicable, armed, link dot, remove action, one mono line of mode/alt/battery. Click selects; selection is blue everywhere (card, marker, ring).
- **Map (center)**: fills all remaining space. Markers are amber arrows rotating with heading; labels stay upright. Goto targeting arms a crosshair. An empty-state overlay with the three onboarding actions renders here when the fleet is empty.
- **Right detail panel (18rem, only when a vehicle is selected)**: full telemetry of the selected vehicle, then the command dock, then command trackers. Everything about ONE vehicle lives here; the left rail never grows detail.
- **Events (overlay, bottom right of map)**: fleet-wide feed with severity dots and mono timestamps.

## Where the next features go

| Milestone | Slot |
|---|---|
| Mission editor (shipped) | right panel MISSION section below COMMANDS; waypoints draw on the map as a dashed blue route with numbered points |
| Multi-vehicle actions (select-all RTL) | left rail header, later milestone |
| Telemetry charts / log review (v0.2) | bottom drawer under the map, collapsed by default |
| Video feeds (later) | detail panel top, above telemetry |

## Vehicle onboarding (C7)

The console opens empty: no automatic fleet, no vehicle until an explicit UI action. Two independent channels feed the fleet, and every vehicle in `fleet.vehicles` carries which one it came from (`source: 'demo' | 'gateway'`):

- **Demo engine**: always available, pure client-side (`FakeGateway`), zero network. "Add demo vehicle" spawns one instantly with a procedurally varied patrol; ids are `demo-1`, `demo-2`, ... Removing one is instant and local. This is also the console's test harness; it is not a toy to delete, only to stop treating as the default.
- **Gateway channel**: opt-in. The connection panel (opened from the top bar's status button) holds the WebSocket URL (remembered in `localStorage`, never auto-connected) and a Connect/Disconnect action. Both "Add simulated vehicle" (a PX4 SITL preset, prefilled `udpin://0.0.0.0:14540`) and "Connect real vehicle" (blank form: id, name, type, connection URL, MAVLink system id) send the same `AddVehicle` envelope to the connected gateway and track the `VehicleConfigAck` lifecycle (pending / rejected with reason / accepted). The gateway sees no difference between "simulated" and "real"; the distinction is console-side framing only. Adding a second or later simulated vehicle in one session shows a resource-warning confirm first (each is a full autopilot build).
- Disconnecting the gateway removes only `source: 'gateway'` vehicles; demo vehicles are untouched. Removing a vehicle is guarded: disabled while armed or airborne, on both channels.
- `PUBLIC_GATEWAY_WS_URL` and `PUBLIC_READONLY` are automation overrides only (docker, CI), not the default experience: when set, the console auto-connects on load; when unset, connecting is always a click.

## Viewer mode

`PUBLIC_READONLY=true` makes the session read-only: a VIEWER badge in the top bar, no command dock, no mission panel, telemetry untouched. Client-side only; the gateway-side rejection policy is issue #20.

## Airspace zones

`PUBLIC_OPENAIP_KEY` turns on the geozone layer: translucent zone polygons under the mission route (red prohibited, amber restricted, blue other), a legend at bottom-left of the map, and a non-blocking warning line in the goto and mission-start confirm dialogs when the target crosses a zone. Absent key means no layer, no requests, no legend: silent by design. `src/lib/geozones/` is built behind a `GeozoneSource` interface so an official ED-269 per-country feed can slot in later without touching the map or the dialogs.

## Interaction rules (do not regress)

- Flight-critical or destructive actions (land, RTL, force disarm, goto) always confirm first; Escape cancels without sending.
- Safety commands (land, RTL, disarm) are never disabled by an in-flight command; only same-kind commands debounce.
- Every command shows its lifecycle where it was issued: EXECUTING (amber pulse), SUCCESS (green), REJECTED/TIMEOUT (red with the gateway reason).
- Color is information: amber = brand/live, green = armed/healthy, red = critical, blue = selected. Nothing else is colored.
