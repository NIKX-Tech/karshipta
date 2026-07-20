# KARSHIPTA Gateway. Development Brief

**Component:** `gateway/` inside the `NIKX-Tech/karshipta` monorepo
**Owner:** you
**Language:** C++20, CMake, MAVSDK
**License:** AGPL-3.0 (repo-wide)

## What Karshipta is

An open-source, self-hosted, web-based command and control platform for fleets of drones and other tracked entities. Users connect real or simulated wards (PX4/ArduPilot over MAVLink) and monitor and task them from a browser. The flagship demo: `docker compose up` starts 3 PX4 SITL wards plus the gateway plus the web console, and you watch a simulated fleet fly missions in your browser within 60 seconds.

The gateway is the heart of the system and it is yours.

## What the gateway does

A single C++ service that:

1. Connects to N wards over MAVLink (UDP for SITL, serial/TCP later) using MAVSDK.
2. Assigns each ward a stable `ward_id` and tracks its state.
3. Publishes telemetry as protobuf `Envelope` frames over a WebSocket server (binary frames, one Envelope per frame). Target rate: 2 to 10 Hz per ward.
4. Receives `Command` and `Mission` envelopes from clients, executes them via MAVSDK, and replies with `CommandAck` / `MissionProgress`.
5. Emits `Event` messages for anything a human should see (link lost, low battery, rejected command).

The protobuf files in `proto/karshipta/v1/` are the single source of truth. Never invent a payload shape; if something is missing, propose a schema change first. The web console is generated from the same files, so the schema is our contract.

## Architecture rules

- **Transport abstraction from day one.** Define a small `Transport` interface (start, stop, send(bytes), on_receive(callback)). Implementation 1: plain WebSocket server (use IXWebSocket or uWebSockets, your pick). Implementation 2 comes in week 4 and will connect outward to a relay service instead of listening; your code must not care which transport is active.
- One `WardManager` owning N `Ward` objects, each wrapping a MAVSDK `System`.
- Wards are discovered dynamically: the gateway takes a list of connection URLs from a YAML/JSON config file (e.g. `udp://:14540`, `udp://:14541`).
- Reconnect forever: SITL containers restart, links drop. `connected=false` in WardState, keep trying, emit Events.
- No global state, no singletons. Everything owned by a `Gateway` object created in `main`.
- Log with spdlog. Errors that reject a command must produce a CommandAck with a reason, never a silent drop.

## Milestones (each one is a PR with a short demo GIF or asciinema in the description)

**M1 (week 1): Hello MAVSDK.**
Run `docker run --rm -it -p 14550:14550/udp -p 14540:14540/udp px4io/px4-sitl:latest`. A minimal C++ program connects via MAVSDK to `udp://:14540` and prints position + battery at 1 Hz. CMake project builds in the repo CI.

**M2 (week 2): Telemetry over WebSocket.**
Gateway serves `ws://localhost:8765`. Any WebSocket client receives binary Envelope frames with WardInfo on connect and WardState at ~5 Hz. Include a tiny Python test client in `gateway/tools/` for verification.

**M3 (week 3): Commands.**
Arm, disarm, takeoff, land, RTL, goto. Incoming Command envelopes executed via MAVSDK Action plugin, every command answered with a CommandAck. Rejections carry a human-readable reason.

**M4 (week 4): Multi-ward + relay transport.**
Config file lists 3 SITL endpoints; gateway manages all three concurrently. Second Transport implementation added (details and credentials come from Erfan that week).

**M5 (week 5): Missions.**
Mission upload via MAVSDK Mission plugin, start/pause, MissionProgress published as the ward advances.

**M6 (week 6): Dockerfile + hardening.**
Multi-stage Dockerfile, gateway joins the root docker-compose. Reconnect behavior verified by killing/restarting SITL containers mid-flight.

## Definition of done, always

- Builds warning-clean in CI (gcc + clang, -Wall -Wextra).
- clang-format applied (config in repo).
- No raw new/delete, RAII everywhere, std::jthread over manual threads.
- A human can run it from the README instructions alone.

## Setup

- Ubuntu 22.04+ or macOS. Docker required.
- MAVSDK: build from source or install prebuilt (v2.x). Docs: mavsdk.mavlink.io, examples at github.com/mavlink/MAVSDK/tree/main/examples
- protobuf + CMake integration: protoc generates into `gateway/gen/` at build time.
- PX4 SITL: the docker image above; later `make px4_sitl` from PX4-Autopilot source if deeper debugging is needed.

## Ways of working

- Everything in English, in GitHub issues/PRs on `NIKX-Tech/karshipta`.
- Small PRs, reviewed by Erfan. Weekly 30 min sync, demo-driven.
- Claude-assisted development is expected and encouraged: keep `gateway/CLAUDE.md` (provided) in the repo and start your sessions from it. You review and must be able to explain every line that gets merged.
