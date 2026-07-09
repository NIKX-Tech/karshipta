# Karshipta Roadmap

## The open-core promise

Everything a single operator needs to fly their own fleet safely on their own infrastructure is open source and free, forever: connectivity, live telemetry, commands, missions, airspace awareness, the demo environment. A commercial hosted edition (by NIKX Technologies B.V.) will cover what goes beyond that: hosting operated for you, sharing fleets beyond your team, organizational scale, compliance, and managed data services. Features never move from the open side to the commercial side.

## v0.1 (first public release)

- Gateway: multi-vehicle MAVLink connectivity via MAVSDK (PX4 SITL), telemetry at 2 to 10 Hz, commands (arm, disarm, takeoff, land, RTL, goto), mission upload and progress. Milestones M1 to M6 in [gateway/BRIEF.md](gateway/BRIEF.md).
- Console: milestones C1 to C6 below.
- One-command demo: docker compose with 3 PX4 SITL vehicles.
- Optional E2E-encrypted relay transport for remote operations.

### Console milestones

| # | Milestone | State |
|---|---|---|
| C1 | Map + fake fleet: shell, FleetStore, transport abstraction, simulated vehicles | done |
| C2 | Commands: selection, command panel with ack lifecycle, events feed, instrument-panel shell ([docs/console-ux.md](docs/console-ux.md)) | done |
| C3 | Missions: click-to-plan waypoints, upload, start/pause, looping, live progress (pairs with gateway M5) | in progress |
| C4 | Live gateway: verified against the real gateway as M2 to M4 land; reconnect UX, link states, multi-vehicle, read-only viewer mode | |
| C5 | Airspace layer: worldwide geo-zones (OpenAIP) on the map, goto/waypoint warnings | |
| C6 | Demo polish: console Dockerfile, compose service, onboarding, demo GIF | |

## v0.2

- ArduPilot support.
- Telemetry persistence and flight log review.
- Multi-operator sessions.

## Later

- Video feeds, plugin/API surface for third-party integrations, hardware reference builds, additional vehicle domains (ground, surface).

Suggestions and use cases: open a GitHub issue.
