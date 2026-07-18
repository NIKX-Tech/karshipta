# RelayTransport

`libs/transport/include/relay_transport.h`, `libs/transport/src/relay_transport.cpp`,
`tests/transport/relay_transport_test.cpp`

## Overview

`RelayTransport` is the second `Transport` implementation (BRIEF.md M4):
instead of listening for inbound connections like `WebsocketTransport`, it
connects outward to a relay service, built on
[relayly](https://github.com/NIKX-Tech/relayly). Code above `Transport` talks
to the same interface either way and cannot tell which implementation is
active.

## Scaffold status: not yet functional against a real relay

This class is a scaffold, not a finished relay transport. relayly
authenticates a device to the relay server with a static identity
(`device_id` plus an X25519 private key) and then pairs that device with one
or more peers, each pairing established through a Noise Protocol XX
handshake authenticated by a pairing token (short code or QR code) exchanged
out of band; the relay server itself only ever sees opaque encrypted frames
once paired. None of the handshake or pairing is implemented here yet:

- **No Noise XX handshake.** Frames sent and received today are plain
  WebSocket binary frames, unauthenticated and unencrypted.
- **No pairing wiring.** `RelayCredentials` (device_id, private_key_path) can
  be supplied, but nothing is sent over the wire yet; relayly's connect URL
  shape is `ws://<host>/ws?device_id=<id>&token=<pair-token>` and neither the
  per-peer pairing token nor that query string is constructed here.
- **No TLS.** The gateway's `ixwebsocket` dependency is built with
  `USE_TLS OFF` (see root `gateway/CMakeLists.txt`); a `wss://` relay URL will
  fail to connect until that is turned on.

BRIEF.md M4 expected relayly's actual pairing spec and device credentials to
come from Erfan; they have not landed in this repo yet. Do not point
`RelayTransport` at a real relayly server expecting a paired peer: it will
either fail the handshake or, worse, exchange frames a real relayly server
would treat as valid ciphertext.

## Credentials: supplied separately from construction

`RelayCredentials` (device_id, private_key_path) mirrors relayly's own Go SDK
`Options` struct so the field names track a real upstream shape rather than
a guess. A `RelayTransport` can be built two ways:

- `RelayTransport(relay_url, credentials)`: pass an already-built
  `RelayCredentials` directly.
- `RelayTransport::from_config(relay_url, config_path)`: load
  `device_id`/`private_key_path` from a YAML file at `config_path` (same
  shape and error-handling pattern as `VehicleManager`'s fleet persistence:
  missing file or fields are logged and treated as empty, never fatal).

Either way, an all-empty `RelayCredentials` is valid: it keeps
`RelayTransport` in the same unauthenticated scaffold mode described above.
This exists so callers (`main.cpp`, tests) can wire up a `RelayTransport`
today without real credentials existing yet; once Erfan's spec lands, only
the config file's contents need to change, not the call site.

The per-peer pairing token is deliberately not part of `RelayCredentials`:
unlike `device_id`/`private_key`, which are fixed for the device's lifetime,
a pairing token is obtained per relationship through `RequestPairCode`/
`AcceptPair`, which requires the still-unimplemented Noise XX handshake.
There is nothing meaningful to load from static config for it yet.

## Responsibilities (as implemented today)

- Open one outbound WebSocket connection to a configured `relay_url` and
  assign it a `Transport::ClientId` on connect, mirroring
  `WebsocketTransport`'s id scheme but for a single underlying link instead
  of N inbound sockets (see "Design" below for why this id is not yet a real
  relayly peer id).
- Fan `on_connect`/`on_receive`/`on_disconnect` out to whoever registered
  them, with that `ClientId`, never an `ix::` type.
- Be safe to call `send`/`broadcast` from a different thread than the one
  that called `start()`: the ix client delivers connect/receive/disconnect
  events on its own thread.

## Public API

| Member | Behavior |
|---|---|
| `RelayTransport(std::string relay_url, RelayCredentials credentials)` | Configures the relay endpoint and this gateway's relay identity; does not connect yet. An empty `RelayCredentials` is valid. |
| `RelayTransport::from_config(relay_url, config_path)` | Static factory: builds credentials from a YAML file, defaulting to empty on a missing/unparsable one. |
| `~RelayTransport()` | Calls `stop()` if still running. |
| `void start() override` | Opens the outbound WebSocket connection. Logs a warning that no handshake/encryption is implemented yet. |
| `void stop() override` | Closes the connection if open. |
| `void send(ClientId, const std::vector<uint8_t>&) override` | No-op unless `client` matches the current connection id. |
| `void broadcast(const std::vector<uint8_t>&) override` | Equivalent to `send` to that id: there is only ever one today. |
| `const RelayCredentials& credentials() const` | Read-only accessor; no setter, credentials are fixed for the instance's lifetime. |
| `ClientRole role(ClientId) const override` | Always `kOperator` (gateway issue #20): relayly pairing has no per-peer role concept yet, so there is nothing to mark a peer viewer with. Revisit once peer identity exists past the Noise XX handshake. |

## Design: one link today, not yet N relayly peers

relayly itself is not a single-peer protocol: one device (one `device_id` and
private key) can pair with several peers at once, each identified separately
and each reachable through `Send(peerID, ...)` (confirmed against relayly's
Go SDK). That maps naturally onto `Transport::ClientId`, the same way
`WebsocketTransport` maps each inbound socket to its own id.

`RelayTransport` does not implement that yet, and deliberately does not fake
it. Peer identity in relayly only exists once a peer completes pairing
(`AcceptPair`), which depends on the Noise XX handshake this class does not
have. So today `peer_id_` (0 when disconnected) represents "the single
outbound WebSocket link to the relay server is up or down", not any specific
paired peer. Extending this to real per-peer ids is expected to fall out
naturally once the pairing layer exists: `on_connect`/`ClientId` assignment
moves from "on WebSocket Open" to "on `AcceptPair` completing for a given
peer", and `peer_id_` becomes a map keyed by relayly peer id, the same shape
`WebsocketTransport` already uses for its `clients_` map.

`socket_` is a `shared_ptr<ix::WebSocket>` rather than `unique_ptr` for the
same reason `WebsocketTransport` stores its clients as `shared_ptr`:
`send()`/`broadcast()` copy it out from under `peer_mutex_` before calling
`sendBinary()`, so a concurrent `stop()` clearing `socket_`/`peer_id_` under
the same lock cannot free the socket out from under an in-flight send.

## Thread safety

`peer_mutex_` guards `socket_` and `peer_id_` for every read and write,
including inside `send()`/`broadcast()`/`stop()`, the same pattern
`WebsocketTransport` uses for `clients_`/`client_ids_`.

`on_receive`/`on_connect`/`on_disconnect` are not synchronized: they must be
called before `start()`, never while the connection is running (asserted in
debug builds).

## Automated tests

`gateway/tests/transport/relay_transport_test.cpp`:

- `ConnectsOutSendsAndReceivesFrames`: stands a plain `WebsocketTransport` in
  for a real relayly server on loopback (there is no Noise handshake to
  fake, since none exists on either side yet). `RelayTransport` connects
  out, a `send()` reaches the stand-in relay, and a reply reaches
  `on_receive`.
- `StopIsIdempotentAndStartAfterStopWorks`: lifecycle safety, mirroring the
  equivalent `WebsocketTransport` test.
- `FromConfigMissingFileDefaultsToEmptyCredentials`: a nonexistent config
  path logs a warning and still produces a usable, empty-credentialed
  instance.
- `FromConfigLoadsFieldsFromFile`: a YAML file with `device_id`/
  `private_key_path` is loaded into the resulting `credentials()`.
- `RoleIsAlwaysOperator`: `role()` reads `kOperator` for any client id,
  connected or not.

## Next steps (tracked, not started)

- Get the pairing spec and credentials from Erfan (BRIEF.md M4).
- Implement the Noise XX handshake, or vendor relayly's protocol library if
  one becomes available for C++; note relayly's official SDKs today are Go,
  TypeScript, Python, and Rust, none of which this repo may introduce as a
  service (root `CLAUDE.md`: "Two languages only: C++ at the edge, TypeScript
  everywhere else").
- Once pairing exists, generalize `peer_id_` from a single id to a map of
  relayly peer id to connection state, so multiple paired peers (e.g.
  multiple console sessions) are each their own `Transport::ClientId`.
- Turn on `USE_TLS` for `ixwebsocket` if the relay endpoint requires `wss://`.
- Decide how `main.cpp` selects between `WebsocketTransport` and
  `RelayTransport` (config-driven, per `gateway/config/`, most likely).
