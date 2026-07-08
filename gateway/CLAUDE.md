# CLAUDE.md: karshipta/gateway

You are assisting development of the Karshipta gateway: a C++20 service that bridges MAVLink vehicles (via MAVSDK) to WebSocket clients using protobuf-encoded Envelope frames.

## Non-negotiable rules

1. **The protobuf schema in `proto/karshipta/v1/` is the single source of truth.** Never invent message fields, payload shapes, or JSON alternatives. If a needed field does not exist, stop and propose a schema change instead of working around it.
2. **C++20, CMake, no exceptions to the toolchain.** Dependencies allowed: MAVSDK, protobuf, spdlog, one WebSocket library (IXWebSocket or uWebSockets), yaml-cpp or nlohmann/json for config, GoogleTest. Do not introduce new dependencies without flagging it clearly.
3. **RAII everywhere.** No raw new/delete, no manual lock/unlock, std::jthread over std::thread, smart pointers with clear ownership. One owner per resource.
4. **Transport is an interface.** Code that publishes or receives Envelopes talks to the `Transport` abstraction only. Never let MAVSDK types or WebSocket types leak across that boundary.
5. **Every failure is observable.** Rejected or failed commands produce a CommandAck with a reason string. Lost links produce Events and `connected=false`. No silent catch-and-continue.
6. **Warnings are errors.** Code must compile clean under -Wall -Wextra on gcc and clang. Apply the repo clang-format.

## Style

- snake_case for functions/variables, PascalCase for types, trailing underscore for private members.
- Header/impl pairs under `gateway/src/`, tests mirror the structure under `gateway/tests/`.
- Comments explain why, not what. Keep functions short.
- Prefer std::chrono types for all time values; the wire uses unix epoch milliseconds.

## Context that saves you time

- MAVSDK v2: `Mavsdk` object discovers `System`s from connection URLs; use plugins Telemetry, Action, Mission. Subscribe-based telemetry callbacks arrive on MAVSDK threads: marshal to our own event loop before touching shared state.
- PX4 SITL via docker exposes MAVLink on udp 14540 (offboard/SDK port). Multiple instances increment ports (14540, 14541, ...).
- Telemetry publish target: 2 to 10 Hz per vehicle. Batch nothing yet; one Envelope per WebSocket binary frame.
- The web console (SvelteKit, TypeScript) consumes the exact same proto files via ts-proto. Wire compatibility is the whole game.

## Commit and text hygiene

- Never add "Co-Authored-By: Claude" or any AI attribution to commits, PRs, or code comments.
- Never use em dashes in any text, docs, commit messages, or comments.
- Commit messages: imperative mood, concise subject, body explains why. Conventional style: `feat(gateway): ...`, `fix(gateway): ...`.

## When asked to write code

Write complete, buildable code including CMake changes. After any milestone-level change, state how to verify it manually (exact commands, expected output). Do not scaffold features beyond the current milestone in GATEWAY_BRIEF.md.
