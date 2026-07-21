# Herald ingestion

`libs/herald/include/herald_ward_manager.h`, `libs/herald/src/herald_ward_manager.cpp`,
`libs/herald/include/herald_http_server.h`, `libs/herald/src/herald_http_server.cpp`

## Overview

Accepts a [Herald](https://github.com/NIKX-Tech/herald) message over HTTP and
reports it as a ward, the same as any MAVLink ward: one `WardInfo` the first
time an `entity_id` is seen, one `WardState` per message after that,
broadcast over the same `Transport` every other ward uses. `HeraldWardManager`
owns the mapping and broadcast; `HeraldHttpServer` is a thin listener that
decodes an HTTP request body into a `herald::v0::Herald` and calls
`HeraldWardManager::ingest()`.

Herald is intentionally the normalization point for every non-MAVLink
source, not just its own native wire format: a future CoT bridge or
SensorThings bridge (github issues #105/#106) translates its own protocol
into a Herald message and calls `ingest()` here too, rather than getting its
own manager.

## Responsibilities

- Decode a Herald message (binary protobuf, or JSON via
  `Content-Type: application/json`) posted to `POST /herald`.
- Map it onto `karshipta::v1::WardState` and, on first sight of an
  `entity_id`, a `karshipta::v1::WardInfo` (`ward_class` mapped from
  `entity_class`, `origin = WARD_ORIGIN_HARDWARE`, `autopilot`/
  `firmware_version` empty, `mavlink_system_id = 0`).
- Reject a message whose `entity_id` collides with an existing MAVLink
  `ward_id` (`WardManager::has_ward`), logging the rejection and returning
  HTTP 409, broadcasting nothing.
- Replay known Herald wards' `WardInfo` to a newly-connected client
  (`send_known_wards`), wired the same way as `WardManager::send_ward_info`.

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
| `HeraldWardManager(Transport&, WardManager&)` | `WardManager&` is used read-only, via `has_ward()`, to detect an `entity_id` collision. |
| `HeraldIngestResult ingest(const herald::v0::Herald&)` | Builds and broadcasts `WardState` (and, on first sight, `WardInfo`). Returns `kWardIdCollision` and broadcasts nothing if `entity_id` is already a MAVLink `ward_id`. |
| `void send_known_wards(Transport::ClientId) const` | Sends one `WardInfo` per known Herald `entity_id` to exactly this client. Wire to `Transport::on_connect`. |

### `HeraldHttpServer`

| Member | Behavior |
|---|---|
| `HeraldHttpServer(HeraldWardManager&, string host, uint16_t port)` | Direct construction; prefer `from_config()`. |
| `static unique_ptr<HeraldHttpServer> from_config(const string& config_path, HeraldWardManager&)` | Reads `herald.host`/`herald.http_port`/`herald.allow_lan_bind` from the gateway YAML config, same safe-loopback-by-default policy as `WebsocketTransport::from_config`. Falls back to `127.0.0.1:8766` on a missing or malformed file. |
| `void start()` | Starts listening on its own thread. Idempotent. |
| `void stop()` | Stops the server and joins its thread. Idempotent. |

## Verifying manually

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
