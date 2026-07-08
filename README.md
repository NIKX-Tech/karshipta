# Karshipta

**Open-source, self-hosted command and control for drone fleets. In your browser.**

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

Karshipta connects to real or simulated vehicles over MAVLink (PX4, ArduPilot) and gives you a live web console to monitor and task the whole fleet: map, telemetry, commands, missions. Self-hosted, no cloud dependency, one binary protobuf contract end to end.

> Named after Karshipta, the Avestan bird said to be the chief of all birds, the messenger that kept an isolated refuge connected to the outside world.

![Karshipta console showing three simulated vehicles on a dark map](docs/images/console.png)

*The console today: three simulated multirotors flying over the PX4 SITL home position.*

---

## Status

Early development, pre-release. The roadmap to v0.1 is in [ROADMAP.md](ROADMAP.md). Star the repo to follow the launch.

| Component | State |
|---|---|
| `proto/` protobuf contract | ✅ v1 schema defined |
| `console/` web dashboard | 🚧 live map with simulated fleet |
| `gateway/` MAVLink edge service | 🚧 in development (M1 to M6 in [gateway/BRIEF.md](gateway/BRIEF.md)) |
| `deploy/` one-command demo | 🚧 SITL containers ready, services land with their Dockerfiles |

## The 60-second demo (coming with v0.1)

```bash
git clone https://github.com/NIKX-Tech/karshipta
cd karshipta
docker compose up
# open http://localhost:5173 and watch 3 simulated vehicles fly
```

Until then, the console runs standalone with a simulated fleet:

```bash
cd console
npm install && npm run proto:gen
npm run dev -- --open
```

## Architecture

```
 PX4 / ArduPilot vehicles (real or SITL)
        |  MAVLink
   [ gateway ]        C++20 + MAVSDK edge service
        |  protobuf Envelopes over WebSocket (E2E-encrypted relay optional)
   [ console ]        SvelteKit web dashboard
```

The protobuf schema in [`proto/karshipta/v1/`](proto/karshipta/v1/) is the contract everything is generated from: C++ types at gateway build time, TypeScript types via ts-proto. One binary Envelope per WebSocket frame, in both directions. See [docs/architecture.md](docs/architecture.md).

## Contributing

Contributions are welcome; the surface is kept deliberately small until the v0.1 launch. Read [CONTRIBUTING.md](CONTRIBUTING.md) for the branching model (`dev` for integration, `main` for releases), the ground rules, and local setup. All contributors sign the lightweight CLA in [CLA.md](CLA.md).

## Safety disclaimer

Karshipta is simulation-first software under active development. It is not certified for controlling real aircraft in operational settings. If you connect real hardware, you do so at your own risk, in compliance with your local aviation regulations, and with a safety pilot in control at all times.

## License

[AGPL-3.0](LICENSE). Commercial licensing and enterprise features: contact NIKX Technologies B.V. via [karshipta.com](https://karshipta.com).
