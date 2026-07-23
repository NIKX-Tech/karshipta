# RelayTransport

`libs/transport/include/relay_transport.h`, `libs/transport/src/relay_transport.cpp`,
`tests/transport/relay_transport_test.cpp`

## Overview

`RelayTransport` is the second `Transport` implementation (BRIEF.md M4):
instead of listening for inbound connections like `WebsocketTransport`, it
connects outward to a relay service, wrapping
[relayly](https://github.com/NIKX-Tech/relayly)'s own C++ SDK
(`sdk/cpp`, `relayly::Client`) rather than a raw WebSocket. Code above
`Transport` talks to the same interface either way and cannot tell which
implementation is active.

Only built when `KARSHIPTA_GATEWAY_ENABLE_RELAY` is `ON` (root
`gateway/CMakeLists.txt`; `OFF` by default everywhere, and a hard CMake error
on Windows) - see "Build gating" below.

## What relayly handles internally

relayly's C++ SDK owns the Noise XX handshake, pairing, peer key pinning, and
reconnection; `RelayTransport` never touches wire bytes or crypto itself:

- **Auth**: `device_id` + `device_token` (a bearer credential from relayly's
  `POST /api/v1/devices` or the `relayly pair <name>` CLI, `docs/PROTOCOL.md`
  §2) authenticate the WebSocket upgrade to the relay server itself.
- **Identity**: this device's X25519 static keypair (`private_key_path`,
  loaded or generated via `relayly::PrivateKey::LoadOrGenerate`) is the Noise
  XX static key, separate from `device_token`.
- **Pairing**: a 6-digit code (`request_pair_code`/`accept_pair`, both thin
  wrappers over `relayly::Client`). **v1 links exactly one peer per device**
  (relayly's own constraint); pairing again replaces whatever was linked
  before. The relay server remembers the pairing server-side
  (`docs/PROTOCOL.md` §5.3) and replays it via `welcome`/`peer_status` on
  every future connect, so pairing is a one-time setup action, not something
  `start()` does automatically or something a restarted gateway needs to
  redo.
- **Peer key pinning**: relayly pins each peer's authenticated static key on
  first pairing (`~/.relayly/peers.json` by default) and hard-fails a later
  handshake presenting a different key for the same peer id. `RelayTransport`
  does not touch this; it is entirely internal to `relayly::Client`.

## Build gating

`KARSHIPTA_GATEWAY_ENABLE_RELAY` (root `gateway/CMakeLists.txt`) defaults to
`OFF` everywhere, not just on Windows:

- relayly's C++ SDK is a new dependency tree on top of `ixwebsocket` (already
  used elsewhere in this repo): libsodium (via libsodium-cmake) and
  nlohmann/json, fetched automatically by relayly's own CMake. Defaulting
  this on would silently grow every build's (and CI's) dependency footprint
  and build time for a transport the M4 milestone explicitly marks optional
  for v0.1 (`ROADMAP.md`).
- relayly's C++ SDK v1 does not support Windows at all (its own
  `sdk/cpp/README.md` Requirements section). Setting
  `KARSHIPTA_GATEWAY_ENABLE_RELAY=ON` on Windows is a CMake `FATAL_ERROR`,
  not a silent no-op, so a Windows developer who tries to turn it on gets a
  clear reason instead of a confusing missing target.

When off, `libs/transport/src/relay_transport.cpp` is not compiled into the
`transport` library and `tests/transport/relay_transport_test.cpp` is not
added to the test binary at all (see the `if(KARSHIPTA_GATEWAY_ENABLE_RELAY)`
guards in both `gateway/CMakeLists.txt` and
`gateway/libs/transport/CMakeLists.txt`); only `relay_transport.h` still
exists on disk, unused.

## Credentials

`RelayCredentials` (`device_id`, `private_key_path`, `device_token`) mirrors
`relayly::Options`'s corresponding fields. A `RelayTransport` can be built
two ways:

- `RelayTransport(relay_url, credentials)`: pass an already-built
  `RelayCredentials` directly.
- `RelayTransport::from_config(relay_url, config_path)`: load all three
  fields from a YAML file at `config_path` (same shape and error-handling
  pattern as `WardManager`'s fleet persistence: missing file or fields are
  logged and treated as empty, never fatal).

An all-empty `RelayCredentials` is valid to construct with, but `start()`
refuses to connect (logged, not thrown) unless both `device_id` and
`device_token` are set; `private_key_path` empty falls back to relayly's own
default (`~/.relayly/device.key`).

`device_id`/`device_token` are provisioned out of band, against a real
relayly server, before the gateway can use them - there is nothing in this
repo that mints them.

## Public API

| Member | Behavior |
|---|---|
| `RelayTransport(std::string relay_url, RelayCredentials credentials)` | Configures the relay endpoint and this gateway's relay identity; does not connect yet. An empty `RelayCredentials` is valid. |
| `RelayTransport::from_config(relay_url, config_path)` | Static factory: builds credentials from a YAML file, defaulting to empty on a missing/unparsable one. |
| `~RelayTransport()` | Calls `stop()` if still running. |
| `void start() override` | Blocks until `relayly::Client::Connect` completes the control-channel handshake or throws (see "Threading and blocking" below). No-op, logged, if `device_id`/`device_token` are missing or already running. |
| `void stop() override` | Closes the `relayly::Client` if open. |
| `void send(ClientId, const std::vector<uint8_t>&) override` | No-op unless `client` matches the currently paired-and-ready peer. Logs and swallows a `relayly::Error` from `Client::Send` (e.g. `kNotReady` mid-reconnect) rather than propagating it. |
| `void broadcast(const std::vector<uint8_t>&) override` | Equivalent to `send` to the current peer: there is only ever one (v1 constraint). |
| `const RelayCredentials& credentials() const` | Read-only accessor; no setter, credentials are fixed for the instance's lifetime. |
| `ClientRole role(ClientId) const override` | Always `kOperator`: relayly pairing has no per-peer role concept. |
| `relayly::PairCode request_pair_code()` | Wraps `Client::RequestPairCode()`. Throws `relayly::Error(kClosed)` if called before `start()` has connected. |
| `std::future<relayly::Peer> accept_pair(const std::string& code)` | Wraps `Client::AcceptPair(code)`. Throws `relayly::Error(kClosed)` if called before `start()` has connected. |

`request_pair_code`/`accept_pair` are not part of the `Transport` interface:
pairing is relay-specific setup, the same way `WebsocketTransport`'s viewer
query-param handling is WebSocket-specific and not part of `Transport`
either. Nothing calls them today (see "Not yet wired" below).

## Mapping relayly's single peer onto Transport::ClientId

`Transport::ClientId` is a plain integer scoped to one `Transport` instance.
relayly identifies peers by string id. `RelayTransport` keeps a single
optional `(relayly peer id, ClientId)` pair (`current_peer_id_`/
`assigned_id_`, guarded by `peer_mutex_`), not a map: v1 links exactly one
peer per device, so there is never more than one to track.

- `on_ready(peer_id)`, not `on_peer_status(peer_id, true)`, is what assigns a
  `ClientId` and fires `Transport::on_connect`: a peer can go online
  (`peer_status`) before its Noise session is actually ready to `Send()` to,
  and `on_ready` is relayly's signal for the latter.
- `on_peer_status(peer_id, false)` clears the assignment and fires
  `Transport::on_disconnect`.
- The same peer reconnecting keeps the same `ClientId` (`handle_peer_ready`
  only allocates a new one when `current_peer_id_` differs from the
  incoming `peer_id`), matching relayly's own "re-handshake keeps the
  session logically continuous" model (`docs/PROTOCOL.md` §6).
- Incoming messages (`on_message`) are only forwarded to
  `Transport::on_receive` if `message.from` matches the currently assigned
  peer; a message from any other id (should not happen given the one-peer
  constraint, but not asserted away) is silently dropped.

## Threading and blocking

`start()` calls `relayly::Client::Connect`, which blocks the calling thread
until the initial control-channel handshake succeeds or throws
`relayly::Error` (relayly's own documented contract, matching how its quick
start calls it synchronously from `main`). This differs from
`WebsocketTransport::start()`, which only begins listening and returns
immediately - `RelayTransport::start()` is not the same kind of "start", and
callers (`main.cpp`, once wired) should expect it to take as long as one
network round trip to the relay server.

All `relayly::Options` callbacks (`on_ready`, `on_peer_status`, `on_message`,
`on_disconnect`, `on_reconnect`) fire on relayly's internal IXWebSocket I/O
thread, never the thread that called `start()` - `peer_mutex_` guards
`current_peer_id_`/`assigned_id_` against that thread racing `send()`/
`broadcast()`/`stop()` called from elsewhere, the same pattern
`WebsocketTransport` uses for its own client map. `on_receive`/`on_connect`/
`on_disconnect` registration itself is not synchronized: callbacks must be
registered before `start()`, never while running (asserted in debug builds).

`Client::Send`, `RequestPairCode`, and `AcceptPair` are safe to call from any
thread (relayly serializes concurrent writes internally).

## Automated tests

`gateway/tests/transport/relay_transport_test.cpp`, only built when
`KARSHIPTA_GATEWAY_ENABLE_RELAY` is on. Exercising a live connect or pairing
flow needs a running relayly server, which this hermetic unit suite
deliberately does not stand up (the old scaffold's approach of faking the
relay with a bare `WebsocketTransport` no longer applies now that a real
protocol - Noise XX plus a JSON control channel - runs on the wire). What is
covered instead:

- `RoleIsAlwaysOperator`: `role()` reads `kOperator` for any client id.
- `StartWithoutDeviceTokenDoesNotConnect` / `StartWithoutDeviceIdDoesNotConnect`:
  `start()` with incomplete credentials logs and leaves `is_running()` false,
  without ever reaching the network.
- `StopWithoutStartIsIdempotent`: lifecycle safety when `stop()` is called
  without a prior `start()`.
- `PairingBeforeStartThrows`: `request_pair_code`/`accept_pair` throw
  `relayly::Error(kClosed)` before `start()` has connected.
- `FromConfigMissingFileDefaultsToEmptyCredentials` /
  `FromConfigLoadsFieldsFromFile`: YAML credential loading, including the new
  `device_token` field.

## Manual verification (requires a live relayly server)

Not covered by the automated suite above:

1. Run a relayly server (`docker-compose.yml` in the relayly repo, or
   `cmd/relayly`).
2. Register a device and obtain a `device_token`:
   `relayly pair gateway-1` (or `POST /api/v1/devices`) against that server.
3. Write `device_id`/`device_token`/`private_key_path` into a YAML file and
   build a `RelayTransport` from it (`RelayTransport::from_config`), or
   construct one directly with a `RelayCredentials`.
4. Call `start()`; confirm it returns once connected (or logs and returns
   promptly on a deliberately wrong `device_token`, which should surface as
   a `relayly::Error` with an auth-related message).
5. From a second device (another `relayly::Client`, or the SDK's own CLI/
   examples under `examples/` in the relayly repo), call
   `RequestPairCode`/`AcceptPair` against this gateway's device, confirm
   `Transport::on_connect` fires here with a `ClientId`, and that
   `send`/`broadcast` reach the other side and `on_receive` fires for
   replies.

## Wiring: how `main.cpp` picks a transport

`gateway.yaml`'s top-level `transport` field selects the implementation
(`build_transport()` in `main.cpp`):

```yaml
transport: relay   # "websocket" (default) or "relay"

relay:
  url: wss://relay.example.com/ws          # not a secret, safe to commit
  credentials_path: gateway/config/relay_credentials.yaml   # gitignored, see the .example file
```

`transport: relay` only actually constructs a `RelayTransport` when this
binary was built with `-DKARSHIPTA_GATEWAY_ENABLE_RELAY=ON` *and*
`relay.url` is set; either gap logs a clear error and falls back to
`WebsocketTransport` rather than failing to start (verified: a
relay-enabled binary given `transport: relay` with an empty `relay.url`
logs the fallback and binds the websocket server normally). `main.cpp`
wraps `transport->start()` in a try/catch since `RelayTransport::start()`
can throw a `relayly::Error` (auth failure, bad handshake) where
`WebsocketTransport::start()` never throws - logs and exits with a
non-zero status rather than an unhandled-exception crash.

Credentials (`device_id`, `device_token`, `private_key_path`) are
deliberately **not** in `gateway.yaml` itself, unlike `relay.url` - see
`relay_credentials.yaml.example` for the format. `gateway.yaml` is
documented as safe to commit; device credentials are secrets and stay
gitignored.

`request_pair_code`/`accept_pair` still have no operational trigger (a CLI
flag, `gateway/tools/`, or similar) - pairing today only happens through
whatever calls `RelayTransport`'s methods directly (tests, or a manual tool
not yet written). Separate follow-up work.

## Still open

- **A CLI/tool to actually trigger pairing** (`request_pair_code`/
  `accept_pair`) outside of unit tests - see above.
- **Console-side pairing UI and the relayly TypeScript SDK as an alternative
  `Transport`** on the console (`console/src/lib/transport/`) is TypeScript
  work, tracked separately from this C++ class.
- **End-to-end verification** (a console commanding a SITL ward through a
  real relay, with the gateway's own WebSocket bound to `localhost` only) is
  blocked on both of the above.
