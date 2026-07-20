# Quickstart: run the console against a real simulated ward

The one-command `docker compose up` demo lands with gateway M6. Until then,
this is how to run the same three pieces by hand: a PX4 SITL ward, the
C++ gateway (M1 to M3 are done: connect, telemetry, commands), and the
console. Fifteen minutes the first time, since the gateway needs a build.

If you only want the console with a simulated fleet and no real ward,
the README's quickstart is enough; skip this page.

## 1. Start a simulated ward (PX4 SITL)

```sh
docker run --rm -it -p 14550:14550/udp -p 14540:14540/udp px4io/px4-sitl:latest
```

Leave this running. It exposes MAVLink on UDP port 14540, which is where the
gateway connects. Wait for `INFO [px4] Startup script returned successfully`
before moving on.

## 2. Build and run the gateway

Requirements: C++20 compiler, CMake 3.20+, [MAVSDK](https://mavsdk.mavlink.io)
v2+, and spdlog.

```sh
# macOS
brew install mavsdk spdlog

# Ubuntu 22.04 (adjust the MAVSDK version to the latest release)
curl -fsSL -o /tmp/mavsdk.deb \
  https://github.com/mavlink/MAVSDK/releases/download/v3.17.1/libmavsdk-dev_3.17.1_ubuntu22.04_amd64.deb
sudo apt-get update && sudo apt-get install -y /tmp/mavsdk.deb libspdlog-dev
```

Build (protobuf is fetched or resolved automatically; see
[`gateway/CMakeLists.txt`](../gateway/CMakeLists.txt)):

```sh
cmake -S gateway -B gateway/build
cmake --build gateway/build -j
```

Run it, in a new terminal, with SITL still running from step 1:

```sh
./gateway/build/src/karshipta_gateway
```

Expect:

```
[info] connected to udp://:14540 (system_id=1)
[info] websocket server listening on ws://127.0.0.1:8765
```

Leave this running too. It publishes one ward, `sitl-1`, at
`ws://localhost:8765`.

The gateway binds to loopback only by default (`gateway/config/gateway.yaml`),
so it is not reachable from another machine on your network. If the console
runs on a different machine than the gateway, do not widen this bind:
cross-machine access is meant to go through the relay transport (see
`gateway/docs/relay-transport.md`), which is still scaffold-only as of M4.
If you understand the risk and need a bare-LAN bind anyway (no
authentication exists yet: anyone who can reach the address can command
every connected ward), set `websocket.allow_lan_bind: true` and
`websocket.host` to a reachable address in `gateway/config/gateway.yaml`;
the gateway logs a loud warning on every startup while that is on.

To watch the wire without the console, use the tiny Python client instead of
or alongside it: `gateway/tools/ws_client.py` (setup steps in its docstring).

## 3. Point the console at it

In a third terminal:

```sh
cd console
npm install && npm run proto:gen
PUBLIC_GATEWAY_WS_URL=ws://localhost:8765 npm run dev -- --open
```

The top bar reads `FLEET 1` with a `LIVE` link (not `SIM`), the fake fleet is
gone, and `sitl-1` is the real SITL ward. Select it and try Arm, then
Takeoff: the command tracker shows ACCEPTED then SUCCESS, and the ward
climbs in SITL's own console output too. `PUBLIC_READONLY=true` opens a
read-only viewer session instead (see [console-ux.md](console-ux.md)).

## Multiple wards

Not yet: the gateway connects to exactly one ward
(`kConnectionUrl`/`kWardId` in `gateway/src/main.cpp`) until the config
file and `WardManager` land in gateway M4
([tracking issue](https://github.com/NIKX-Tech/karshipta/issues/16)).

## Troubleshooting

- **Gateway logs `no matching autopilot discovered ... within 3s`**: SITL
  isn't up yet, or something else is already using port 14540. Confirm with
  `docker ps` and the SITL startup log.
- **Console top bar stays `DOWN`**: the gateway isn't running, or
  `PUBLIC_GATEWAY_WS_URL` doesn't match its host/port (default
  `ws://localhost:8765`).
- **Commands stay `EXECUTING` and then time out**: this was the exact
  behavior before gateway M3; if you are on a build older than that
  milestone, update.
