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

- **Top bar**: identity, fleet count, gateway link state (LIVE / SIM / CONNECTING / DOWN). The one place the amber pulse always lives.
- **Left rail (16rem)**: the whole fleet at a glance. Compact cards: id, armed, link dot, one mono line of mode/alt/battery. Click selects; selection is blue everywhere (card, marker, ring).
- **Map (center)**: fills all remaining space. Markers are amber arrows rotating with heading; labels stay upright. Goto targeting arms a crosshair.
- **Right detail panel (18rem, only when a vehicle is selected)**: full telemetry of the selected vehicle, then the command dock, then command trackers. Everything about ONE vehicle lives here; the left rail never grows detail.
- **Events (overlay, bottom right of map)**: fleet-wide feed with severity dots and mono timestamps.

## Where the next features go

| Milestone | Slot |
|---|---|
| Mission editor (shipped) | right panel MISSION section below COMMANDS; waypoints draw on the map as a dashed blue route with numbered points |
| Multi-vehicle actions (select-all RTL) | left rail header, later milestone |
| Telemetry charts / log review (v0.2) | bottom drawer under the map, collapsed by default |
| Video feeds (later) | detail panel top, above telemetry |

## Viewer mode

`PUBLIC_READONLY=true` makes the session read-only: a VIEWER badge in the top bar, no command dock, no mission panel, telemetry untouched. Client-side only; the gateway-side rejection policy is issue #20.

## Interaction rules (do not regress)

- Flight-critical or destructive actions (land, RTL, force disarm, goto) always confirm first; Escape cancels without sending.
- Safety commands (land, RTL, disarm) are never disabled by an in-flight command; only same-kind commands debounce.
- Every command shows its lifecycle where it was issued: EXECUTING (amber pulse), SUCCESS (green), REJECTED/TIMEOUT (red with the gateway reason).
- Color is information: amber = brand/live, green = armed/healthy, red = critical, blue = selected. Nothing else is colored.
