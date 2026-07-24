# Herald ingestion

`libs/herald/include/herald_ward_manager.h`, `libs/herald/src/herald_ward_manager.cpp`,
`libs/herald/include/herald_http_server.h`, `libs/herald/src/herald_http_server.cpp`,
`libs/herald/include/gt06_parser.h`, `libs/herald/src/gt06_parser.cpp`,
`libs/herald/include/gt06_tcp_server.h`, `libs/herald/src/gt06_tcp_server.cpp`

## Overview

Two independent ingestion paths, both normalizing onto the same
`herald::v0::Herald` message and the same `HeraldWardManager::ingest()` entry
point, so a ward reported through either looks identical downstream: one
`WardInfo` the first time an `entity_id` is seen, one `WardState` per message
after that, broadcast over the same `Transport` every other ward uses.

- **HTTP** (github issue #101): `POST /herald` with a native
  `herald::v0::Herald` body (binary protobuf or JSON). `HeraldHttpServer` is a
  thin listener that decodes the request body and calls `ingest()`.
- **GT06** (github issue #123): a raw binary TCP protocol (0x78 0x78 frame
  header, conventionally port 5023) hundreds of low-cost GPS tracker models
  speak natively (Concox/Jimi/iTrack family and countless OEM rebrands -
  "under $30 wholesale, shipped from Shenzhen" is a good heuristic for
  whether a tracker speaks it). `Gt06Parser` is pure framing/checksum/decode
  logic with no networking (unit-testable with plain byte buffers);
  `Gt06TcpServer` wraps it around a real connection and calls `ingest()`,
  the GT06 counterpart to `HeraldHttpServer`. Ported from Traccar's
  `Gt06ProtocolDecoder.java`/`Checksum.java` (github.com/traccar/traccar) and
  the public GT06 protocol spec, not reverse-engineered from nothing. Scope
  is deliberately narrow ("basic tracking" per issue #123): login (IMEI),
  standard GPS location, and heartbeat packet types only - alarm/command
  packet types still frame and checksum-validate correctly (so the
  connection doesn't desync) but come back as `kUnsupported`, silently
  acknowledged where GT06 expects an ack and otherwise ignored.

`HeraldWardManager` owns the mapping and broadcast for both paths equally -
neither listener talks to `Transport` directly.

Herald is intentionally the normalization point for every non-MAVLink
source, not just its own native wire format: a future CoT bridge or
SensorThings bridge (github issues #105/#106) translates its own protocol
into a Herald message and calls `ingest()` here too, rather than getting its
own manager.

## Two release artifacts, one codebase

`karshipta-gateway` (drones, MAVLink) and `karshipta-herald` (generic tags/GPS
devices via this Herald path, no MAVLink) are two separate, fixed products
built from this same repo - not a runtime toggle either exposes, a
build-time CMake option (`KARSHIPTA_GATEWAY_ENABLE_MAVLINK`, default `ON`,
see the root `CMakeLists.txt`) used only when producing the two artifacts.
Everything in this file - `HeraldWardManager`, `HeraldHttpServer`,
`Gt06Parser`, `Gt06TcpServer`, `FleetManager` (Fleet/Zone CRUD) - links zero
MAVSDK and is compiled into both artifacts identically. A livestock-tag-only
or generic-tracker-only user runs the lean
`karshipta-herald` build and never needs MAVSDK installed, never sees
"gateway"/relay-pairing language (that's specifically a MAVLink-behind-NAT
concern, see `relay-transport.md`), and connects the console to it exactly
the same way as any WebSocket gateway.

Build it with `-DKARSHIPTA_GATEWAY_ENABLE_MAVLINK=OFF`; CI's
`gateway-herald-only` job builds this configuration on every push (gcc and
clang, no MAVSDK installed in that job at all - the point is proving it's
genuinely not needed, not just that the flag compiles). Not yet covered:
`KARSHIPTA_GATEWAY_BUILD_TESTS=ON` requires `KARSHIPTA_GATEWAY_ENABLE_MAVLINK=ON`
(a hard CMake error otherwise) - the existing test suite constructs real
`WardManager` instances throughout and isn't adapted for this configuration
yet; only the plain build is verified today, expanding coverage is a
followup.

## Responsibilities

- Decode a Herald message (binary protobuf, or JSON via
  `Content-Type: application/json`) posted to `POST /herald`.
- Map it onto `karshipta::v1::WardState` and, on first sight of an
  `entity_id`, a `karshipta::v1::WardInfo` (`ward_class` mapped from
  `entity_class`, `origin = WARD_ORIGIN_HARDWARE`, `autopilot`/
  `firmware_version` empty, `mavlink_system_id = 0`).
- In `karshipta-gateway` builds only: reject a message whose `entity_id`
  collides with an existing MAVLink `ward_id` (`WardManager::has_ward`),
  logging the rejection and returning HTTP 409, broadcasting nothing.
  `karshipta-herald` builds have no `WardManager` to collide-check against -
  there are no MAVLink `ward_id`s in that build at all - so this check is
  skipped entirely, never a no-op call.
- Replay known Herald wards' `WardInfo` to a newly-connected client
  (`send_known_wards`), wired the same way as `WardManager::send_ward_info`.

GT06-specific mapping notes (`Gt06TcpServer`'s `build_herald_message`): GT06
carries no device-purpose field (only a device-type/model variant), so
`entity_class` is always `ENTITY_CLASS_GENERIC_TRACKER` - an honest default,
not a guessed livestock/asset classification. The base location packet has
no separate device-health signal, so `health_ok` uses the GPS fix-valid flag
as a proxy (an invalid fix is exactly the kind of "weak signal" issue
`herald.proto`'s own `health_ok` doc comment describes). `velocity` and
`battery` are left unset in this pass - GT06 exposes course/speed and a
heartbeat-only battery reading, but combining either into a location's
Herald message needs additional per-connection state or trigonometry not
required by issue #123's "basic tracking" scope; a natural future
extension, not implemented today.

## Explicitly out of scope

- **Commands.** A Herald-reporting source has no autopilot; there is nothing
  here analogous to `dispatch_command`, and `WardState.flight` is never
  populated by this path.
- **Persistence.** Known Herald wards live in memory only
  (`known_wards_`) and are forgotten on restart. A Herald ward's identity is
  entirely "it has posted a message"; there is nothing meaningful to persist
  across a process restart the way `WardManager` persists operator-added
  `WardConfig`s.
- **Vendor mapping / CoT / SensorThings bridges** (github issues #102/#105/#106).
  Those translate some other format into a Herald message; this class only
  consumes the Herald message once it exists.
- **Org scoping.** `Herald.org_id` is read but has no `WardState` destination
  yet (github issue #104); it is intentionally dropped, not stored in an
  invented field.

## Public API

### `HeraldWardManager`

| Member | Behavior |
|---|---|
| `HeraldWardManager(Transport&, WardManager&)` | `karshipta-gateway` builds. `WardManager&` is used read-only, via `has_ward()`, to detect an `entity_id` collision. |
| `HeraldWardManager(Transport&)` | `karshipta-herald` builds (`KARSHIPTA_GATEWAY_ENABLE_MAVLINK=OFF`). No `WardManager` exists to collide-check against. |
| `HeraldIngestResult ingest(const herald::v0::Herald&)` | Builds and broadcasts `WardState` (and, on first sight, `WardInfo`). In `karshipta-gateway` builds, returns `kWardIdCollision` and broadcasts nothing if `entity_id` is already a MAVLink `ward_id`. |
| `void send_known_wards(Transport::ClientId) const` | Sends one `WardInfo` per known Herald `entity_id` to exactly this client. Wire to `Transport::on_connect`. |

### `HeraldHttpServer`

| Member | Behavior |
|---|---|
| `HeraldHttpServer(HeraldWardManager&, string host, uint16_t port)` | Direct construction; prefer `from_config()`. |
| `static unique_ptr<HeraldHttpServer> from_config(const string& config_path, HeraldWardManager&)` | Reads `herald.host`/`herald.http_port`/`herald.allow_lan_bind` from the gateway YAML config, same safe-loopback-by-default policy as `WebsocketTransport::from_config`. Falls back to `127.0.0.1:8766` on a missing or malformed file. |
| `void start()` | Starts listening on its own thread. Idempotent. |
| `void stop()` | Stops the server and joins its thread. Idempotent. |

### `Gt06Parser` (pure logic, no networking)

| Member | Behavior |
|---|---|
| `static ParseResult parse_frame(span<const uint8_t>)` | Parses one frame from the start of the buffer. `FrameResult` is `kOk` (with `consumed_bytes` and the decoded `ParsedPacket`), `kIncomplete` (valid-so-far prefix, wait for more bytes from the next `recv()`), or `kInvalid` (bad start marker or checksum failure - the caller should log and drop the connection, not wait forever). |
| `static vector<uint8_t> build_ack(PacketType, uint16_t serial_number)` | The exact ack frame bytes GT06 expects back for login/heartbeat; empty for `kLocation` (GT06 does not ack locations) and `kUnsupported`. |
| `static uint16_t crc16_x25(span<const uint8_t>)` | CRC-16/X-25 (poly 0x1021, init/xorout 0xFFFF, input/output reflected) - verified against the standard catalogue check value for `"123456789"` (`0x906E`), not just self-consistency. |

### `Gt06TcpServer` (subclasses `ix::SocketServer`)

| Member | Behavior |
|---|---|
| `Gt06TcpServer(HeraldWardManager&, string host, uint16_t port)` | Direct construction; prefer `from_config()`. |
| `static unique_ptr<Gt06TcpServer> from_config(const string& config_path, HeraldWardManager&)` | Reads `herald.gt06_host`/`herald.gt06_port`/`herald.allow_lan_bind`/`herald.container_bind` (the last two shared with `HeraldHttpServer`'s own keys), same safe-bind policy. Falls back to `127.0.0.1:5023` (GT06's conventional port) on a missing or malformed file. |
| `void start()` | Binds (`ix::SocketServer::listen()`) and launches the accept-loop/GC threads, logging and returning (not throwing) on a bind failure - wraps two base-class steps into the same one-call shape `HeraldHttpServer::start()` and `WebsocketTransport::start()` already have. |
| `void stop()` (override) | Signals every in-flight connection thread to exit at its next poll timeout (up to ~1s) and stops accepting new connections. Idempotent. |

One worker thread per connection (`ix::SocketServer`'s own model). Each
remembers the IMEI from that connection's login packet locally - GT06 sends
login once per connection and implies it for every later packet - and
combines it with each decoded location to build and `ingest()` a
`herald::v0::Herald`. A location received before any login on that
connection is logged and dropped (no IMEI to attach it to yet).

## Verifying manually

HTTP path:

```
curl -X POST http://127.0.0.1:8766/herald -H "Content-Type: application/json" -d '{
  "entity_id": "tag-1",
  "timestamp_ms": 1234567890000,
  "entity_class": "ENTITY_CLASS_LIVESTOCK_TAG",
  "position": {"latitude_deg": 52.37, "longitude_deg": 4.90},
  "health_ok": true,
  "org_id": "demo"
}'
```

Expect `200`. A connected console shows `tag-1` as a livestock-tag ward with
no flight-mode UI, using the existing ward rendering with no console changes.

GT06 path (raw TCP, port 5023 by default) - a real login + location frame
pair, Python one-liner style:

```python
import socket
s = socket.create_connection(("127.0.0.1", 5023))
s.sendall(bytes.fromhex("78780f01086184504123456701020001585c0d0a"))
print(s.recv(1024).hex())  # expect an ack frame back: 787805010001d9dc0d0a
s.sendall(bytes.fromhex("7878181018030f0c1e2d0c08059e62900086952005145a00024ee00d0a"))
# no ack expected for a location frame
```

Expect the gateway log to show `GT06 device ... identified as IMEI
861845041234567` after the login frame. A connected console shows the same
IMEI as a generic-tracker ward once the location frame lands.
