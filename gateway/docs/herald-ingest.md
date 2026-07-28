# Herald ingestion

`libs/herald/include/herald_ward_manager.h`, `libs/herald/src/herald_ward_manager.cpp`,
`libs/herald/include/herald_http_server.h`, `libs/herald/src/herald_http_server.cpp`,
`libs/herald/include/herald_field_mapper.h`, `libs/herald/src/herald_field_mapper.cpp`,
`libs/herald/include/gt06_parser.h`, `libs/herald/src/gt06_parser.cpp`,
`libs/herald/include/gt06_tcp_server.h`, `libs/herald/src/gt06_tcp_server.cpp`

## Overview

Three independent ingestion paths, all normalizing onto the same
`herald::v0::Herald` message and the same `HeraldWardManager::ingest()` entry
point, so a ward reported through any of them looks identical downstream: one
`WardInfo` the first time an `entity_id` is seen, one `WardState` per message
after that, broadcast over the same `Transport` every other ward uses.

- **HTTP, native** (github issue #101): `POST /herald` with a body that's
  already a `herald::v0::Herald` message (binary protobuf or JSON).
  `HeraldHttpServer` is a thin listener that decodes the request body and
  calls `ingest()`.
- **HTTP, mapped** (github issue #102, the Herald spec's "Mapped" adoption
  path): `POST /herald/mapped/<source_name>` with a vendor's own JSON
  payload, translated via a declarative field-mapping config
  (`HeraldFieldMapper`) instead of a bespoke per-vendor integration. See
  "The Mapped path" below.
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

`HeraldWardManager` owns the mapping and broadcast for all three paths
equally - none of the three listeners talk to `Transport` directly.

Herald is intentionally the normalization point for every non-MAVLink
source, not just its own native wire format: a future CoT bridge or
SensorThings bridge (github issues #105/#106) translates its own protocol
into a Herald message and calls `ingest()` here too, rather than getting its
own manager.

## Two release artifacts, one codebase

`karshipta-gateway` (MAVLink flight vehicles) and `karshipta-herald` (generic
tags/GPS devices via this Herald path, no MAVLink) are two separate, fixed products
built from this same repo - not a runtime toggle either exposes, a
build-time CMake option (`KARSHIPTA_GATEWAY_ENABLE_MAVLINK`, default `ON`,
see the root `CMakeLists.txt`) used only when producing the two artifacts.
Everything in this file - `HeraldWardManager`, `HeraldHttpServer`,
`HeraldFieldMapper`, `Gt06Parser`, `Gt06TcpServer`, `FleetManager` (Fleet/Zone
CRUD) - links zero MAVSDK and is compiled into both artifacts identically. A
livestock-tag-only or generic-tracker-only user runs the lean
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

## The Mapped path

`POST /herald/mapped/<source_name>` - for a vendor source that won't (or
can't) be rewritten to send Herald natively. Instead of a bespoke
integration per vendor, a declarative YAML config says where each Herald
field lives in that vendor's own JSON field names; `HeraldFieldMapper`
reads the config once at startup and applies it to every request against
that route.

Every `*.yaml` file in `gateway.yaml`'s `herald.mapping_config_dir`
(default `gateway/config/herald_mappings`) is loaded as one
`HeraldFieldMapping`, keyed by its own `source_name`. A file that fails to
parse is logged and skipped, not fatal - one broken mapping config must not
take down the native or GT06 paths, or any other mapping. See
`gateway/config/herald_mappings/example-generic.yaml` for the one
clearly-documented generic worked example this scope calls for - a
template to copy and adapt, not a specific closed vendor's integration.

Config shape:

```yaml
source_name: example-generic       # also the URL segment: /herald/mapped/example-generic
entity_class: ENTITY_CLASS_GENERIC_TRACKER   # fixed per mapping, see below
timestamp_unit: unix_ms            # or unix_seconds
fields:                            # Herald field name -> dot-path into the vendor payload
  entity_id: device_id
  timestamp_ms: timestamp
  latitude_deg: location.lat
  longitude_deg: location.lon
  altitude_msl_m: location.alt     # optional
  battery_voltage_v: battery.voltage       # optional
  battery_remaining_pct: battery.percent   # optional
  num_satellites: gps.satellites   # optional
  health_ok: status.ok             # optional; defaults true if unmapped
```

`entity_id`/`timestamp_ms`/`latitude_deg`/`longitude_deg` are required -
both at load time (the mapping config must map them to *some* path) and at
request time (that path must resolve to a real value in the payload, or the
whole request is rejected with 400). Every other field is optional at both
levels: omit it from the config, or have it simply be absent from a given
payload, and that part of the Herald message is left unset rather than
rejecting the request. `entity_id` accepts a JSON string or number
(coerced to its string form); the four numeric fields accept a JSON number
or a numeric string (some vendors send coordinates as strings).

Deliberate simplifications, not oversights (see `herald_field_mapper.h` for
the full reasoning): `entity_class` is one fixed value per mapping config,
not a further per-message value-to-`EntityClass` lookup table - a source
reporting more than one device type needs a separate mapping config per
type, which the existing one-config-per-source model already supports.
`velocity`, `hdop`, `tags`, and `metadata` have no mapping keys at all in
this pass - not fields a "generic worked example" needs to demonstrate the
mechanism. ISO 8601 string timestamps aren't supported, only numeric
`unix_ms`/`unix_seconds` - real timezone/format complexity not needed for
one worked example.

## Explicitly out of scope

- **Commands.** A Herald-reporting source has no autopilot; there is nothing
  here analogous to `dispatch_command`, and `WardState.flight` is never
  populated by this path.
- **Persistence.** Known Herald wards live in memory only
  (`known_wards_`) and are forgotten on restart. A Herald ward's identity is
  entirely "it has posted a message"; there is nothing meaningful to persist
  across a process restart the way `WardManager` persists operator-added
  `WardConfig`s.
- **CoT / SensorThings bridges** (github issues #105/#106). Those translate
  some other protocol into a Herald message; this class only consumes the
  Herald message once it exists.
- **Value-to-EntityClass mapping tables, per-message overrides, everything
  in the Mapped path beyond the narrow scope in "The Mapped path" below**
  (github issue #102's original, broader milestone scope).
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
| `HeraldHttpServer(HeraldWardManager&, string host, uint16_t port, map<string, HeraldFieldMapping> mappings = {})` | Direct construction; prefer `from_config()`. |
| `static unique_ptr<HeraldHttpServer> from_config(const string& config_path, HeraldWardManager&)` | Reads `herald.host`/`herald.http_port`/`herald.allow_lan_bind`/`herald.mapping_config_dir` from the gateway YAML config, same safe-loopback-by-default policy as `WebsocketTransport::from_config`. Falls back to `127.0.0.1:8766` on a missing or malformed file; loads every `*.yaml` in `herald.mapping_config_dir` (default `gateway/config/herald_mappings`) as a `HeraldFieldMapping`. |
| `void start()` | Starts listening on its own thread. Idempotent. |
| `void stop()` | Stops the server and joins its thread. Idempotent. |

### `HeraldFieldMapper` (pure logic, no networking)

| Member | Behavior |
|---|---|
| `static optional<HeraldFieldMapping> load_mapping(const string& yaml_path)` | Parses one mapping config file. `nullopt` (logging why) on a missing file, malformed YAML, or a config missing `source_name` or any required field's path. |
| `static optional<herald::v0::Herald> apply(const HeraldFieldMapping&, const nlohmann::json& payload)` | Applies a mapping to an already-parsed vendor JSON payload. `nullopt` (logging why) if a required field's path is missing from the payload, resolves to the wrong JSON type, or `timestamp_ms` can't be read as a number. |

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

Mapped path, using the bundled `example-generic` config:

```
curl -X POST http://127.0.0.1:8766/herald/mapped/example-generic \
  -H "Content-Type: application/json" -d '{
    "device_id": "tracker-42",
    "timestamp": 1710505845000,
    "location": {"lat": 52.37, "lon": 4.90, "alt": 12.5},
    "battery": {"voltage": 3.98, "percent": 76},
    "gps": {"satellites": 9},
    "status": {"ok": true}
  }'
```

Expect `200` and a gateway log line `loaded Herald mapping
'example-generic' from '...'` at startup. A connected console shows
`tracker-42` as a generic-tracker ward. `POST` the same body to
`/herald/mapped/does-not-exist` and expect `404`; drop `"location"` from
the body and expect `400` with a gateway log line naming the missing field
and path.

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
