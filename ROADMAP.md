# Karshipta Roadmap

## The open-core promise

Everything a single operator needs to fly their own fleet safely on their own infrastructure is open source and free, forever: connectivity, live telemetry, commands, missions, airspace awareness, the demo environment. A commercial hosted edition (by NIKX Technologies B.V.) will cover what goes beyond that: hosting operated for you, sharing fleets beyond your team, organizational scale, compliance, and managed data services. Features never move from the open side to the commercial side.

## v0.1 (first public release)

- Gateway: multi-ward MAVLink connectivity via MAVSDK (PX4 SITL), telemetry at 2 to 10 Hz, commands (arm, disarm, takeoff, land, RTL, goto), mission upload and progress. Milestones M1 to M6 in [gateway/BRIEF.md](gateway/BRIEF.md).
- Console: milestones C1 to C6 below.
- One-command demo: docker compose with 3 PX4 SITL wards.
- Optional E2E-encrypted relay transport for remote operations.

### Console milestones

| # | Milestone | State |
|---|---|---|
| C1 | Map + fake fleet: shell, FleetStore, transport abstraction, simulated wards | done |
| C2 | Commands: selection, command panel with ack lifecycle, events feed, instrument-panel shell ([docs/console-ux.md](docs/console-ux.md)) | done |
| C3 | Missions: click-to-plan waypoints, upload, start/pause, looping, live progress (pairs with gateway M5) | done |
| C4 | Live gateway: verified against the real gateway as M2 to M4 land; reconnect UX, link states, multi-ward, read-only viewer mode | in progress: telemetry and commands verified live, viewer mode shipped |
| C5 | Airspace layer: worldwide geo-zones (OpenAIP) on the map, goto/waypoint warnings | |
| C6 | Demo polish: console Dockerfile, compose service, demo GIF | |
| C7 | Onboarding and ward management: empty-state console, add demo/simulated/real wards from the UI, gateway connection panel (pairs with gateway M4) | next up |
| C8 | console-core packaging: the console's lib published as a reusable npm package | |

### Ward rename

Renamed the core entity from Vehicle to Ward across proto, gateway, and console, and generalized it to represent non-flight tracked entities (livestock GPS tags, generic trackers) alongside flight vehicles. Landed as one combined PR before v0.1's first public release, since it changed the wire vocabulary and was much cheaper to do before release than after. See `docs/glossary.md` for the current vocabulary.

### Fleet, Zone, and mission assignment

New Fleet and Zone entities (named, persistent, shared objects, not live telemetry), a deliberately simple FleetMissionAssignment (one mission template applied to a chosen subset of a Fleet's Wards, no per-ward variation, no multi-agent task allocation), a narrow SQLite persistence layer in the gateway for just Fleet and Zone, and a collapsible right-panel console UI (Ward / Fleet / Zones tabs). Organization/multi-tenancy stays out of this repo entirely, Enterprise-only. See [docs/fleet-mission-model.md](docs/fleet-mission-model.md) for the full design.

## v0.2

- ArduPilot support.
- Telemetry persistence and flight log review.
- Multi-operator sessions.

## Later

- Video feeds, plugin/API surface for third-party integrations, hardware reference builds, additional ward domains (ground, surface).

Suggestions and use cases: open a GitHub issue.
