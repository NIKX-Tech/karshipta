# Glossary

Locked vocabulary for code, schema, docs, and reviews. Marketing copy (README pitch, website) may say "drone" for its audience; identifiers and technical docs never do.

| Term | Meaning |
|---|---|
| **ward** | Any tracked, controlled unit: a flight vehicle (multirotor, fixed wing, VTOL, helicopter) or a non-flight tracked entity (ground/underwater/surface vessel, a livestock GPS tag, a generic tracker). The only word for this in code and schema (`WardInfo`, `ward_id`, `WardConnection`). Never "vehicle" or "drone" in identifiers. |
| **ward class** | What kind of ward it is (`WardClass`: multirotor, fixed wing, VTOL, helicopter, ground, underwater, surface vessel, livestock tag, generic tracker). Field name: `ward_class`. Independent of whether the ward is flight-capable or MAVLink-connected; a ward class alone does not imply an autopilot. |
| **fleet** | The set of wards one gateway manages, defined by its config. The console's `FleetStore` mirrors exactly this. |
| **ward_id** | Stable, human-readable string identity assigned in gateway config (e.g. `sitl-1`). Chosen by the operator, never derived from MAVLink ids at runtime. |
| **system id** | The numeric MAVLink identity a ward broadcasts. Config maps `ward_id -> system_id`; used only to bind connections to the right MAVLink endpoint. |
| **connection URL** | The MAVLink endpoint for one ward (`udpin://0.0.0.0:14540`, serial later). Field name: `connection_url`. Any MAVLink-speaking ward connects this way, flight-capable or not. |
| **gateway** | The C++ edge service speaking MAVLink to wards and Envelopes to consoles. |
| **console** | The web dashboard. One console session = one transport client. |
| **transport** | The abstraction carrying Envelope bytes between gateway and console. Implementations: WebSocket server (local), relay (remote). Code above it never knows which is active. |
| **client** | One connected console session as the transport sees it (`ClientId`). Not a ward, not a person. |
| **operator** | A human commanding wards through a console. |
| **viewer** | A read-only console session: receives telemetry, cannot command. |
| **envelope** | The single wire unit: one binary protobuf `Envelope` per WebSocket frame, both directions. |
| **command / ack** | A `Command` (uuid `command_id`) sent by a console; always answered by a `CommandAck`, with a human-readable reason on rejection. Flight-only: meaningless for a ward with no `flight` state. |
| **event** | A human-relevant occurrence (link lost, low battery, rejected command) published by the gateway. |
| **mission / waypoint** | A `Mission` is an ordered list of `MissionItem`s; a waypoint is one item. `repeat_count` = extra passes after the first. Flight-only. |
| **flight state** | `FlightState`, nested on `WardState.flight`, unset for non-flight wards: `flight_mode`, `armed`, `in_air`. Genuinely requires an autopilot state machine to mean anything, unlike velocity or heading, which apply to any moving ward. |
| **home** | The launch position a flight ward returns to on RTL. |
| **tags** | Free-form operator-assigned labels on `WardState.tags` (e.g. a livestock tag's herd or paddock). No schema meaning beyond display/filtering. |
| **ward link** | MAVLink connectivity between gateway and a ward (`WardState.connected`). |
| **console link** | Transport connectivity between console and gateway (LIVE / SIM / CONNECTING / DOWN in the console top bar). |
| **SITL** | A simulated flight ward (PX4 software-in-the-loop). The default development fleet. |
