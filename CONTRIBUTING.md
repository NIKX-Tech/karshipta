# Contributing to Karshipta

Thank you for considering a contribution! Karshipta is in an MVP sprint toward its first public release (v0.1). Until that launch, the core team keeps the surface small and PRs are coordinated; the best ways to help right now are trying the demo, filing issues, and discussing use cases.

## Before you start

- Check the [issues](https://github.com/NIKX-Tech/karshipta/issues) for existing tickets before opening a new one.
- All contributors sign the lightweight CLA in [CLA.md](CLA.md).
- The roadmap lives in [ROADMAP.md](ROADMAP.md); the architecture in [docs/architecture.md](docs/architecture.md).

## Branching model

- `dev` is the integration branch. All PRs target `dev`.
- `main` is production: it only advances via reviewed merges from `dev` and carries the release tags.

## Ground rules

- **Schema first.** The protobuf files in `proto/karshipta/v1/` are the single source of truth for every payload. If a field is missing, propose a schema change in its own PR; never invent payload shapes or JSON alternatives.
- **Two languages.** C++20 at the edge (`gateway/`), TypeScript everywhere else (`console/`). No new languages or services.
- **Warnings are errors.** Gateway code compiles clean under `-Wall -Wextra` on gcc and clang; console code passes `svelte-check`, eslint, and prettier in strict mode.
- **Every failure observable.** Rejected commands produce a `CommandAck` with a reason; lost links produce Events. No silent drops.
- **Simulation-first.** Everything must work against PX4 SITL with zero hardware. Never add features that bypass autopilot safety checks.

## Local development

### Console

```sh
cd console
npm install
npm run proto:gen   # generate TypeScript types from ../proto
npm run dev -- --open
```

Verify with `npm run lint && npm run check && npm run build`.

### Gateway

```sh
cmake -S gateway -B gateway/build
cmake --build gateway/build -j
./gateway/build/src/karshipta_gateway   # needs PX4 SITL running, see docs/quickstart.md
```

Build tests with `-DKARSHIPTA_GATEWAY_BUILD_TESTS=ON`, then `ctest --test-dir gateway/build`. Milestone plan in `gateway/BRIEF.md`; running it end to end against a real simulated vehicle and the console is in [docs/quickstart.md](docs/quickstart.md).

### Schema

```sh
protoc -I proto --descriptor_set_out=/dev/null proto/karshipta/v1/*.proto
```

## Commits and PRs

- Conventional commits: `feat(console): ...`, `fix(gateway): ...`, `docs: ...`. Imperative subject, body explains why.
- Small PRs. Every change must be verifiable by a human from written instructions; say how to verify in the PR description.
- CI (schema validation, console lint/check/build) must be green.
