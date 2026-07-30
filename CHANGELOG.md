# Changelog

Notable changes to Karshipta, most recent first. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning
follows [Semantic Versioning](https://semver.org/) from the first tagged
release onward.

## [0.1.1]

### Fixed

- Gateway: the disarm RPC issued when removing or stopping a ward had no
  timeout of its own, so an autopilot that never acknowledged the command
  could hang the call indefinitely. Now bounded to 5 seconds; a timeout is
  treated as a failed disarm instead of left to hang.

## [0.1.0] - first public release

The first public release: gateway milestones M1 to M10, console milestones
C1 to C8 (see [ROADMAP.md](ROADMAP.md) for the full list). Summarized here
retroactively, since no changelog existed before this file did.

### Added

**Gateway**

- Multi-ward MAVLink connectivity via MAVSDK (PX4, ArduPilot SITL), one
  shared Mavsdk core per process.
- Live telemetry publish at 2 to 10 Hz per ward: position, battery, flight
  mode, armed/in-air state, link status.
- Commands - arm, disarm (including forced), takeoff, land, RTL, goto -
  with a full accepted/executing/success/rejected/timeout ack lifecycle.
- Missions: click-to-plan-equivalent upload, start/pause, repeat count,
  live progress, QGC `.plan` and Mission Planner `.wpl` file import.
- Ward add/remove with persisted fleet state, reconnect handling, and a
  read-only viewer client mode.
- Fleet: named, persistent ward groups, many-to-many membership.
- Zone: named keep-in/keep-out polygons, advisory (warns before upload,
  does not block).
- Fleet Mission: per-ward independently-planned routes submitted as one
  trackable unit, with per-ward and aggregate status through upload,
  active flight, and stop (RTL/Hold/Land, operator's choice); remove and
  edit are safety-gated on every ward having actually stopped. Replaces an
  earlier design that broadcast one shared route to every selected ward
  simultaneously - see
  [docs/fleet-mission-model.md](docs/fleet-mission-model.md).
- Herald ingestion for trackers with no autopilot: native HTTP, a
  declarative YAML field-mapping HTTP path for arbitrary vendor JSON, and
  raw GT06 TCP - see
  [gateway/docs/herald-ingest.md](gateway/docs/herald-ingest.md).
- Optional E2E-encrypted relay transport (relayly) for remote operations
  without a reachable inbound port, verified end to end against a real
  relay server.
- SQLite persistence for Fleet, Zone, and Fleet Mission; YAML persistence
  for ward connection state.
- Two build flavors from one codebase: `karshipta-gateway` (MAVLink +
  Herald) and `karshipta-herald` (Herald only, no MAVSDK dependency).
- Concurrency and crash-safety hardening across the ward/fleet managers
  (two-level locking, transactional persistence writes).

**Console**

- Live fleet map (MapLibre): ward markers, telemetry, trails, mission
  routes, zones, worldwide airspace overlay (OpenAIP, opt-in).
- Command panel with confirm dialogs on flight-critical actions, and an
  events feed with severity.
- Mission planning: click-to-plan waypoints, upload, start/pause, looping,
  live progress.
- Fleet and Zone management UI, and a Fleet Mission list/wizard: pick a
  Fleet or ad-hoc wards, plan an independent route per ward, track status,
  stop/remove/edit per mission.
- Onboarding: empty-state console, add demo/simulated/real wards from the
  UI, gateway connection panel (direct WebSocket or relay pairing).
- Read-only viewer mode.
- Published as an embeddable npm package (`@nikx-tech/karshipta-console-core`).

**Other**

- `proto/karshipta/v1/` protobuf schema as the single source of truth for
  the wire contract, generating both the gateway's C++ types and the
  console's TypeScript types.
- One-command demo: `docker compose up` brings up 3 PX4 SITL wards, the
  gateway, and the console together.
- Ward rename: the core entity renamed from Vehicle to Ward across the
  whole stack, generalized to represent non-flight tracked entities
  (livestock GPS tags, generic trackers) alongside flight vehicles.
- Real release automation (`release-gateway.yml`): builds and publishes
  gateway/herald binaries for macOS (arm64), Linux, and Windows on every
  `v*.*.*` tag.
