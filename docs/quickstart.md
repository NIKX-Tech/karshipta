# Quickstart: run the console against a real simulated ward

As of gateway M6, `docker compose -f deploy/docker-compose.yml up` starts
three PX4 SITL wards and the gateway in one step (see the README's
"one-command demo"). This page is the alternative: running the same pieces
by hand, useful for gateway development itself or when you don't want
Docker in the loop. Fifteen minutes the first time, since the gateway needs
a build.

If you only want the console with a simulated fleet and no real ward,
the README's quickstart is enough; skip this page.

## 1. Start a simulated ward (PX4 SITL)

```sh
docker run --rm -it -p 14550:14550/udp px4io/px4-sitl:latest
```

Leave this running. Only 14550 is published: SITL's own MAVLink stream to
the gateway is outbound from the container to the host machine's fixed
gateway address, not something the host needs to reach in via a published
port. Do not also publish 14540 - on Docker Desktop (macOS/Windows) its
proxy binds that host port for itself, so a natively run gateway (which
needs to bind that same host port to listen for SITL's stream) fails with
"Address already in use" if it's published. Wait for
`INFO [px4] Startup script returned successfully` before moving on.

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

**macOS with Xcode 15.4 or older**: this codebase uses `std::jthread`/
`std::stop_token` throughout, which Xcode 15.4's bundled libc++ does not
implement (confirmed directly: GitHub's own `macos-14` runner ships Xcode
15.4 and fails with `no member named 'jthread' in namespace 'std'` until
worked around). `clang --version` tells you which Xcode you
have. If you hit this, install a newer compiler instead of upgrading Xcode:

```sh
brew install llvm
export CC=/opt/homebrew/opt/llvm/bin/clang
export CXX=/opt/homebrew/opt/llvm/bin/clang++
export CXXFLAGS="-stdlib=libc++ -isysroot $(xcrun --sdk macosx --show-sdk-path)"
export LDFLAGS="-L/opt/homebrew/opt/llvm/lib/c++ -Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++"
```
before the `cmake -S gateway -B gateway/build` step below. The `-isysroot`
flag matters: Homebrew's `clang` binary is a plain LLVM clang, not Apple's
`/usr/bin/clang` wrapper, so it does not auto-detect the active SDK the way
the wrapper does - without it, clang can pick up the platform's C headers in
the wrong search-path order relative to libc++'s own wrapper headers and
fail with a confusing `tried including <X.h> but didn't find libc++'s <X.h>
header` error instead.

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
cross-machine access is meant to go through the relay transport instead
(see `gateway/docs/relay-transport.md`, verified end to end). If you
understand the risk and need a bare-LAN bind anyway (no
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

The top bar reads `WARDS 1` with a `LIVE` link (not `SIM`), the fake fleet is
gone, and `sitl-1` is the real SITL ward. Select it and try Arm, then
Takeoff: the command tracker shows ACCEPTED then SUCCESS, and the ward
climbs in SITL's own console output too. `PUBLIC_READONLY=true` opens a
read-only viewer session instead (see [console-ux.md](console-ux.md)).

## Optional: report a non-MAVLink ward (Herald)

The gateway also accepts [Herald](https://github.com/NIKX-Tech/herald)
messages over HTTP, for sources with no MAVLink autopilot at all (a
livestock tag, a generic tracker). With the gateway from step 2 still
running:

```sh
curl -X POST http://127.0.0.1:8766/herald -H "Content-Type: application/json" -d '{
  "entity_id": "tag-1",
  "timestamp_ms": 1700000000000,
  "entity_class": "ENTITY_CLASS_LIVESTOCK_TAG",
  "position": {"latitude_deg": 52.37, "longitude_deg": 4.90},
  "health_ok": true
}'
```

A connected console shows `tag-1` alongside `sitl-1`, as a livestock-tag
ward with no flight-mode UI (no arm/takeoff/land: there is no autopilot
behind it to send those to). Same loopback-only-by-default bind policy as
the websocket port above, controlled by `herald.host`/`herald.allow_lan_bind`
in `gateway/config/gateway.yaml`. See
[`gateway/docs/herald-ingest.md`](../gateway/docs/herald-ingest.md) for the
full mapping and the HTTP status codes a POST can come back with.

## Multiple wards

The gateway manages any number of wards concurrently (`WardManager`, see
`gateway/docs/ward-manager.md`), each independently reconnecting. It seeds
one default (`sitl-1`, `udp://:14540`) on first run with nothing persisted
yet; add more with an `AddWard` envelope (the console's "Add real ward"
dialog, or by hand with `gateway/tools/ws_client.py`) naming a distinct
`ward_id` and `connection_url` (e.g. `udp://:14541` for a second SITL
instance started the same way as step 1, on a different port). Every
successful add/remove persists to `gateway/config/fleet_state.yaml`, so the
fleet survives a gateway restart.

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
