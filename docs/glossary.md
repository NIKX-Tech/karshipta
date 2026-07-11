# Glossary

Locked vocabulary for code, schema, docs, and reviews. Marketing copy (README pitch, website) may say "drone" for its audience; identifiers and technical docs never do.

| Term | Meaning |
|---|---|
| **vehicle** | Any controlled unit: multirotor, fixed wing, VTOL, helicopter, ground. The only word for this in code and schema (`VehicleInfo`, `vehicle_id`, `VehicleConnection`). Never "drone" in identifiers. |
| **fleet** | The set of vehicles one gateway manages, defined by its config. The console's `FleetStore` mirrors exactly this. |
| **vehicle_id** | Stable, human-readable string identity assigned in gateway config (e.g. `sitl-1`). Chosen by the operator, never derived from MAVLink ids at runtime. |
| **system id** | The numeric MAVLink identity a vehicle broadcasts. Config maps `vehicle_id -> system_id`; used only to bind connections to the right airframe. |
| **connection URL** | The MAVLink endpoint for one vehicle (`udpin://0.0.0.0:14540`, serial later). Field name: `connection_url`. |
| **gateway** | The C++ edge service speaking MAVLink to vehicles and Envelopes to consoles. |
| **console** | The web dashboard. One console session = one transport client. |
| **transport** | The abstraction carrying Envelope bytes between gateway and console. Implementations: WebSocket server (local), relay (remote). Code above it never knows which is active. |
| **client** | One connected console session as the transport sees it (`ClientId`). Not a vehicle, not a person. |
| **operator** | A human commanding vehicles through a console. |
| **viewer** | A read-only console session: receives telemetry, cannot command. |
| **envelope** | The single wire unit: one binary protobuf `Envelope` per WebSocket frame, both directions. |
| **command / ack** | A `Command` (uuid `command_id`) sent by a console; always answered by a `CommandAck`, with a human-readable reason on rejection. |
| **event** | A human-relevant occurrence (link lost, low battery, rejected command) published by the gateway. |
| **mission / waypoint** | A `Mission` is an ordered list of `MissionItem`s; a waypoint is one item. `repeat_count` = extra passes after the first. |
| **home** | The launch position a vehicle returns to on RTL. |
| **vehicle link** | MAVLink connectivity between gateway and a vehicle (`VehicleState.connected`). |
| **console link** | Transport connectivity between console and gateway (LIVE / SIM / CONNECTING / DOWN in the console top bar). |
| **SITL** | A simulated vehicle (PX4 software-in-the-loop). The default development fleet. |
