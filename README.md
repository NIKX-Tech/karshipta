<p>
  <img src="docs/images/logo-mark.svg" alt="Karshipta" height="64">
</p>

# Karshipta

**Open-source, self-hosted command and control for unmanned vehicle and tracker fleets. In your browser.**

[![CI](https://img.shields.io/github/actions/workflow/status/NIKX-Tech/karshipta/ci.yml?branch=main&style=flat-square&label=build)](https://github.com/NIKX-Tech/karshipta/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/NIKX-Tech/karshipta?style=flat-square&color=blue)](https://www.gnu.org/licenses/agpl-3.0)
[![C++ Standard](https://img.shields.io/badge/gateway-C%2B%2B20-00599C?style=flat-square&logo=cplusplus&logoColor=white)](gateway/)
[![Svelte](https://img.shields.io/badge/console-Svelte%205-FF3E00?style=flat-square&logo=svelte&logoColor=white)](console/)
[![MAVLink](https://img.shields.io/badge/protocol-MAVLink-2C2255?style=flat-square)](https://mavlink.io)
<br>
[![Repo Size](https://img.shields.io/github/repo-size/NIKX-Tech/karshipta?style=flat-square)](https://github.com/NIKX-Tech/karshipta)
[![GitHub Stars](https://img.shields.io/github/stars/NIKX-Tech/karshipta?style=flat-square&color=yellow)](https://github.com/NIKX-Tech/karshipta/stargazers)
[![Dependabot](https://img.shields.io/badge/dependabot-enabled-025e8a?logo=dependabot&style=flat-square)](.github/dependabot.yml)
[![Website](https://img.shields.io/badge/website-karshipta.com-F5A623?style=flat-square&logo=google-chrome&logoColor=white)](https://karshipta.com)

<!--
Uncomment as they go live:
[![Latest Release](https://img.shields.io/github/v/release/NIKX-Tech/karshipta?style=flat-square)](https://github.com/NIKX-Tech/karshipta/releases)
[![CodeQL](https://img.shields.io/github/actions/workflow/status/NIKX-Tech/karshipta/codeql.yml?branch=main&label=codeql&style=flat-square)](https://github.com/NIKX-Tech/karshipta/actions/workflows/codeql.yml)
[![OpenSSF Scorecard](https://img.shields.io/ossf-scorecard/github.com/NIKX-Tech/karshipta?label=openssf%20scorecard&style=flat-square)](https://securityscorecards.dev/projects/github.com/NIKX-Tech/karshipta)
[![Sponsor GitHub](https://img.shields.io/badge/sponsor-GitHub-EA4AAA?style=flat-square&logo=github-sponsors)](https://github.com/sponsors/NIKX-Tech)
-->

Karshipta connects to real or simulated wards over MAVLink (PX4, ArduPilot) and gives you a live web console to monitor and task the whole fleet: map, telemetry, commands, missions. A ward is any tracked, controlled unit: a flight vehicle today, and any other MAVLink-speaking tracked entity (a livestock tag, a generic tracker) tomorrow. Self-hosted, no cloud dependency, one binary protobuf contract end to end.

> Named after Karshipta, the Avestan bird said to be the chief of all birds, the messenger that kept an isolated refuge connected to the outside world.

![Karshipta console showing three simulated wards on a dark map](docs/images/console.png)

*The console flying three demo wards over the PX4 SITL home position.*

---

## Status

Early development, pre-release. The roadmap to v0.1 is in [ROADMAP.md](ROADMAP.md). Star the repo to follow the launch.

| Component | State |
|---|---|
| `proto/` protobuf contract | ✅ v1 schema defined |
| `console/` web dashboard | ✅ map, commands, missions, simulated fleet; demo GIF and a fresh-eyes quickstart pass still open (C6, [#22](https://github.com/NIKX-Tech/karshipta/issues/22)/[#32](https://github.com/NIKX-Tech/karshipta/issues/32)) |
| `gateway/` MAVLink edge service | ✅ connect, telemetry, commands, multi-ward, relay transport, missions, Docker demo (M1 to M6 in [gateway/BRIEF.md](gateway/BRIEF.md)) |
| `deploy/` one-command demo | ✅ SITL + gateway + console all wired up (M6, C6) |
| Prebuilt gateway binaries | ✅ macOS (arm64), Linux, Windows, published to [GitHub Releases](https://github.com/NIKX-Tech/karshipta/releases) on each tagged release |

## Try the console

```bash
git clone https://github.com/NIKX-Tech/karshipta
cd karshipta/console
npm install && npm run proto:gen
npm run dev -- --open
```

The console opens empty, on purpose: no auto-started fleet pretending to be
your hardware. Everything from there is one click:

- **Add demo ward**: instant, no setup, nothing to connect. Good for a
  first look.
- **Add simulated ward** or **Connect real ward**: both need a running
  gateway (the C++ service that speaks MAVLink); the console's connection
  panel (top bar) walks you to it, or see [docs/quickstart.md](docs/quickstart.md)
  to run one against PX4 SITL.

## The one-command demo

```bash
docker compose -f deploy/docker-compose.yml up
```

This starts three PX4 SITL wards, the gateway (serving over
`ws://localhost:8765`, the same port a natively run gateway uses), and the
console itself, built as a static site and served by nginx. Open
[http://localhost:5173](http://localhost:5173): `sitl-1` is live on the map
instead of the fake fleet, no extra setup.

Optionally, set `PUBLIC_OPENAIP_KEY` (a free key from
[openaip.net](https://www.openaip.net/)) before the `up` command to turn on
the airspace layer (docs/console-ux.md); without it the map works the same,
just without that layer.

## Architecture

One shared wire contract, one gateway process, and two ways in for a ward.

```mermaid
flowchart TD
    proto["<b>proto/karshipta/v1</b><br/><span style='font-size:11px'>the wire contract</span>"]:::contractNode
    console["<b>Console</b><br/><span style='font-size:11px'>SvelteKit + MapLibre</span>"]:::consoleNode
    gateway["<b>Gateway</b> <span style='font-size:11px'>C++20</span><br/><span style='font-size:11px'>WardManager &middot; HeraldWardManager &middot; FleetManager</span>"]:::gatewayNode

    proto -. "protoc: C++" .-> gateway
    proto -. "ts-proto: TS" .-> console

    console == "WebSocket, LAN direct" ==> gateway
    console -- "Relay (relayly), NAT traversal" --> gateway

    subgraph flight ["<b>FLIGHT</b> &middot; MAVLink vehicles"]
        mavsdk["<b>MAVSDK</b>"]:::flightNode -- "MAVLink, UDP 14540+" --> vehicle["<b>PX4 / ArduPilot</b><br/><span style='font-size:11px'>SITL or real autopilot</span>"]:::flightNode
    end

    subgraph herald ["<b>HERALD</b> &middot; anything without an autopilot"]
        native["<b>Native</b><br/><span style='font-size:11px'>POST /herald</span>"]:::heraldNode --> tags["<b>Tags &amp; generic<br/>trackers</b>"]:::heraldNode
        mapped["<b>Mapped</b><br/><span style='font-size:11px'>POST /herald/mapped/&lt;source&gt;</span>"]:::heraldNode --> tags
        gt06["<b>GT06</b><br/><span style='font-size:11px'>TCP :5023</span>"]:::heraldNode --> tags
    end

    gateway --> flight
    gateway --> herald

    classDef contractNode fill:none,stroke:#f5a623,stroke-width:2px
    classDef gatewayNode fill:none,stroke:#f5a623,stroke-width:2px
    classDef consoleNode fill:none,stroke:#3b9eff,stroke-width:2px
    classDef flightNode fill:none,stroke:#2ecc71,stroke-width:1.5px
    classDef heraldNode fill:none,stroke:#a78bfa,stroke-width:1.5px

    style flight fill:none,stroke:#2ecc71,stroke-width:1.5px
    style herald fill:none,stroke:#a78bfa,stroke-width:1.5px
```

The protobuf schema in [`proto/karshipta/v1/`](proto/karshipta/v1/) is the contract everything is generated from: C++ types at gateway build time, TypeScript types via ts-proto. One binary `Envelope` per WebSocket frame, in both directions.

| Path | Protocol | Carries |
|---|---|---|
| MAVLink | MAVSDK over UDP, port 14540+ | Flight vehicles: PX4, ArduPilot, SITL or real |
| Herald, native | HTTP `POST /herald` | A payload that is already a Herald message |
| Herald, mapped | HTTP `POST /herald/mapped/<source>` | A vendor's own JSON, translated by a declarative YAML field mapping |
| Herald, GT06 | Raw TCP, port 5023 | Cheap GT06-family GPS trackers, hundreds of models |

MAVLink and Herald are two independent domains, not stages of one pipeline: `karshipta-gateway` links both, `karshipta-herald` links MAVSDK to neither and never sees a "MAVLink mode" toggle. See [gateway/docs/herald-ingest.md](gateway/docs/herald-ingest.md) for the full ingestion writeup.

> [!NOTE]
> **On the roadmap, not built yet:** a DJI-to-MAVLink bridge, a Cursor-on-Target bridge ([#105](https://github.com/NIKX-Tech/karshipta/issues/105)), an OGC SensorThings bridge ([#106](https://github.com/NIKX-Tech/karshipta/issues/106)), and org scoping for Herald's `org_id` field ([#104](https://github.com/NIKX-Tech/karshipta/issues/104)).

> [!TIP]
> **Distribution beyond this repo:** `karshipta-desktop` wraps this exact gateway binary as a signed Tauri sidecar, for customers running MAVLink hardware who would rather not touch a terminal. `karshipta-cloud` (private) is the hosted console and backend: org and billing, a persistent relay peer per site, a viewport-bounded public guest feed. Neither forks the gateway; both reach it over the same relay transport shown above.

See [docs/architecture.md](docs/architecture.md) for the full breakdown.

## Contributing

Contributions are welcome; the surface is kept deliberately small until the v0.1 launch. Read [CONTRIBUTING.md](CONTRIBUTING.md) for the branching model (`dev` for integration, `main` for releases), the ground rules, and local setup. All contributors sign the lightweight CLA in [CLA.md](CLA.md).

## Safety disclaimer

Karshipta is simulation-first software under active development. It is not certified for controlling real aircraft in operational settings. If you connect real hardware, you do so at your own risk, in compliance with your local aviation regulations, and with a safety pilot in control at all times.

## License

[AGPL-3.0](LICENSE). Commercial licensing and enterprise features: contact NIKX Technologies B.V. via [karshipta.com](https://karshipta.com).
