# Transport / WebsocketTransport

`libs/transport/include/transport.h`, `libs/transport/include/websocket_transport.h`,
`libs/transport/src/websocket_transport.cpp`

## Overview

`Transport` is the pure-virtual interface everything above the wire talks
to: start/stop, send bytes to one client, broadcast bytes to all clients,
and register callbacks for connect/disconnect/receive. It carries opaque
`std::vector<uint8_t>` payloads only; it never parses them (that is the
Envelope layer's job, in `main.cpp` today) and no MAVSDK type may cross it.

`WebsocketTransport` is the first implementation (BRIEF.md M2): a plain
WebSocket server built on IXWebSocket, binary frames only, no TLS. A second
implementation (an outbound relay transport, BRIEF.md M4) is expected to
implement the same interface later; code above `Transport` must not be able
to tell which one is active.

## Responsibilities

- Listen for WebSocket connections on a configured host/port and accept
  binary frames only (`send`/`broadcast`/`on_receive`).
- Assign every connected client a stable `Transport::ClientId` (a
  `uint64_t`, unrelated to IXWebSocket's own internal id) for the lifetime
  of that connection, and forget it on disconnect.
- Fan callbacks out to whoever registered them (`on_connect`, `on_receive`,
  `on_disconnect`) with that `ClientId`, never an `ix::` type.
- Be safe to call from multiple threads for `send`/`broadcast`: IXWebSocket
  delivers each connection's events on its own thread from an internal pool.

## Explicitly out of scope

- **Envelope encoding/decoding.** `Transport` carries raw bytes; building or
  parsing a `karshipta::v1::Envelope` from those bytes is the caller's job
  (currently `main.cpp`; will move into a `Gateway`/`VehicleManager` type
  later).
- **Acting on received frames.** `on_receive` fires with the raw bytes; as
  of M2 the gateway only logs them. Turning a `Command` envelope into a
  `DroneActions` call and replying with a `CommandAck` is BRIEF.md M3.
- **The relay transport.** A second `Transport` implementation that connects
  outward instead of listening; BRIEF.md M4, not started.

## Public API

### `Transport` (interface)

| Member | Behavior |
|---|---|
| `using ClientId = uint64_t` | Identifies one connected client, scoped to a single `Transport` instance. |
| `using ReceiveCallback = std::function<void(ClientId, const std::vector<uint8_t>&)>` | Fired once per received binary frame. |
| `using ConnectCallback = std::function<void(ClientId)>` | Fired when a client connects. |
| `using DisconnectCallback = std::function<void(ClientId)>` | Fired when a client disconnects. |
| `virtual void start()` | Starts listening (or, for a relay transport, connecting out). Idempotent. |
| `virtual void stop()` | Stops and drops every client. Idempotent. |
| `virtual bool is_running() const` | |
| `virtual void send(ClientId, const std::vector<uint8_t>&)` | Sends to one client. No-op if that client is no longer connected. |
| `virtual void broadcast(const std::vector<uint8_t>&)` | Sends to every currently connected client. |
| `virtual void on_receive(ReceiveCallback)` | Replaces the current receive callback. |
| `virtual void on_connect(ConnectCallback)` | Replaces the current connect callback. |
| `virtual void on_disconnect(DisconnectCallback)` | Replaces the current disconnect callback. |

### `WebsocketTransport` (implementation)

| Member | Behavior |
|---|---|
| `WebsocketTransport(std::string host, uint16_t port)` | Configures the listen address; does not start listening yet. Applies no safe-bind policy: prefer `from_config()` at call sites. |
| `WebsocketTransport::from_config(config_path)` | Static factory: loads `websocket.host`/`websocket.port`/`websocket.allow_lan_bind` from a YAML file, enforcing the safe-bind policy below. |
| `~WebsocketTransport()` | Calls `stop()` if still running. |
| `void start() override` | Constructs the `ix::WebSocketServer`, registers the internal message-dispatch callback, and calls `listenAndStart()`. Logs and returns without setting `running_` if the bind fails. |
| `void stop() override` | Stops the server, clears the client maps, and calls `ix::uninitNetSystem()`. |
| `void send(ClientId, const std::vector<uint8_t>&) override` | Looks up and copies the `shared_ptr<ix::WebSocket>` for that id under `clients_mutex_`; no-op if not found. |
| `void broadcast(const std::vector<uint8_t>&) override` | Snapshots the current `shared_ptr<ix::WebSocket>` list under the lock, then sends outside it. |
| `const std::string& host() const` / `uint16_t port() const` | Read-only accessors for the address this instance was built with. |

## Design: safe-bind config and the LAN escape hatch

The gateway has no authentication (gateway hardening issue #16): whoever can
open a WebSocket to `host:port` can command every connected vehicle.
`WebsocketTransport::from_config()` exists so that fact drives a safe
default instead of an opt-in one, loading `gateway/config/gateway.yaml`:

```yaml
websocket:
  host: 127.0.0.1
  port: 8765
  allow_lan_bind: false
```

- A missing config file, or `websocket` section, falls back to
  `(127.0.0.1, 8765)`.
- A `host` that is not exactly `127.0.0.1`, `localhost`, or `::1` (this
  includes `0.0.0.0`, and any specific LAN address) is treated as a wider
  bind and gated by `allow_lan_bind`:
  - `allow_lan_bind: false` (the default): the requested host is **ignored**
    and the transport binds to `127.0.0.1` anyway, with a logged warning
    explaining why and how to opt in.
  - `allow_lan_bind: true`: the requested host is honored, but every startup
    logs a loud `SECURITY:`-prefixed warning that the resulting server is
    unauthenticated and reachable from other machines.
- **Cross-machine access is meant to go through `RelayTransport` instead**
  (`gateway/docs/relay-transport.md`), not a LAN-exposed plain websocket.
  `allow_lan_bind` exists for cases (LAN testing, a trusted isolated
  network) where that is not yet practical, not as the recommended path.

This policy lives in `from_config()`, not the plain constructor: the
constructor still binds wherever it is told, unconditionally, since tests
construct `WebsocketTransport` directly against `127.0.0.1` on ephemeral
ports and should not go through config-file loading to do it.

## Design: mapping IXWebSocket connections to `ClientId`

IXWebSocket's `WebSocketServer::setOnClientMessageCallback` delivers three
event types through one callback, keyed by an `ix::WebSocket&` reference
that stays valid for the connection's lifetime but carries no id useful to
`Transport` callers:

```cpp
server_->setOnClientMessageCallback(
    [this](const std::shared_ptr<ix::ConnectionState>& state,
           ix::WebSocket& ws,
           const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open: ...    // assign a new ClientId
            case ix::WebSocketMessageType::Close: ...   // look up and forget it
            case ix::WebSocketMessageType::Message: ... // look up and forward
        }
    });
```

`WebsocketTransport` keeps two maps under `clients_mutex_`:
`clients_` (`ClientId -> shared_ptr<ix::WebSocket>`, used by `send`/
`broadcast`) and `client_ids_` (`ix::WebSocket* -> ClientId`, the reverse
lookup the message callback needs since it only has the `WebSocket&`).
`Open` looks up the matching `shared_ptr` from `server_->getClients()` (the
server's own client set) by comparing it against `&web_socket`, then inserts
into both maps from `next_client_id_.fetch_add(1)`; `Close` erases both.
Storing the `shared_ptr` rather than the raw pointer matters: `send()` and
`broadcast()` copy it out from under the lock before calling `sendBinary()`,
so a client disconnecting concurrently and being erased from the server's
own set cannot free the socket while a send against it is in flight.
`Message` events are dropped unless `msg->binary` is true; text frames are
logged and ignored, since the wire protocol is binary Envelope frames only.

## Thread safety

IXWebSocket runs each connection's callback on its own thread from an
internal pool, so `clients_`/`client_ids_` are guarded by `clients_mutex_`
for every read and write, including inside `send()` and `broadcast()`.
`broadcast()` copies the current `shared_ptr<ix::WebSocket>` list out from
under the lock before calling `sendBinary()` on each, so a slow client can't
hold the lock (and therefore block new connections/disconnects) for the
duration of the broadcast, and the copied `shared_ptr`s keep each socket
alive even if it disconnects mid-broadcast.

`running_` is `std::atomic<bool>` since `is_running()` may be polled from a
different thread than the one that called `start()`/`stop()`.

`on_receive`/`on_connect`/`on_disconnect` are not synchronized: they must be
called before `start()`, never while the server is running (asserted in
debug builds).

## RAII and ownership rules

```cpp
WebsocketTransport(std::string host, uint16_t port);
~WebsocketTransport() override;  // calls stop() if still running
```

- **No copy or move** (deleted explicitly): a live server holds callbacks
  that capture `this`, and duplicating or relocating it would leave those
  pointing at a stale address.
- **Owns `server_` outright** (`std::unique_ptr<ix::WebSocketServer>`),
  constructed in `start()` and reset in `stop()`; no `ix::` type is ever
  returned or exposed to callers.
- **`ix::initNetSystem()` / `uninitNetSystem()`** are called in `start()` /
  `stop()` respectively (Windows socket subsystem init/teardown); paired
  1:1 since `start()`/`stop()` are themselves idempotent.

## Constraints and preconditions

- **Binary frames only.** Text frames are logged and dropped; `Transport`
  has no concept of a non-Envelope message.
- **No backpressure.** `send`/`broadcast` hand off to IXWebSocket's own
  send path; a slow or stalled client is not detected or dropped here.
- **One callback per event type.** Calling `on_receive`/`on_connect`/
  `on_disconnect` again replaces the previous callback; there is no
  multi-subscriber fan-out.
- **`ClientId` is only unique within one `WebsocketTransport` instance's
  lifetime**, not globally, and is not the same value as any MAVLink or
  vehicle id.

## M2 test client

`gateway/tools/websocket_test_client.py` connects to a running gateway,
decodes each Envelope frame, and prints one line per `VehicleInfo`/
`VehicleState`. See the setup and usage comment at the top of that file.

## Automated tests

`gateway/tests/transport/websocket_transport_test.cpp` (GoogleTest, real
server + real IXWebSocket client on loopback ports, deadline-guarded):

- `ConnectDeliversFrameAndDisconnectMatchesId`: a client connect fires
  `on_connect`, a `send()` to that id arrives as one binary frame, and the
  disconnect fires `on_disconnect` with the same `ClientId`.
- `BroadcastReachesEveryClientAndReceiveRoundTrips`: `broadcast()` reaches
  two clients; a client's binary frame reaches `on_receive` intact.
- `StopIsIdempotentAndStartAfterStopWorks`: lifecycle safety.

`WebsocketTransportFromConfig` covers the safe-bind policy above:

- `MissingFileDefaultsToLoopback`: no config file resolves to
  `(127.0.0.1, 8765)`.
- `LoadsHostAndPortFromFile`: an explicit loopback host/port round-trips.
- `LanBindWithoutEscapeHatchFallsBackToLoopback`: `host: 0.0.0.0` with
  `allow_lan_bind` left off (or `false`) is overridden back to `127.0.0.1`.
- `LanBindWithEscapeHatchIsHonored`: `host: 0.0.0.0` with
  `allow_lan_bind: true` is honored as configured.

## Manual verification

Build and run the gateway (`gateway/CLAUDE.local.md` has the full command
list):

```
cmake -S gateway -B gateway/build
cmake --build gateway/build
./gateway/build/src/karshipta_gateway.exe
```

With a vehicle connected, the log includes:

```
[info] websocket server listening on ws://127.0.0.1:8765
```

(`gateway/config/gateway.yaml`'s tracked defaults; see the safe-bind design
section above for what changes if you edit `websocket.host`.)

Connecting any WebSocket client to `ws://localhost:8765` logs, on that
client's connection:

```
[info] client 1 connected from 127.0.0.1
```

and the client receives one 19-byte binary frame immediately (`VehicleInfo`)
followed by an 88-byte binary frame roughly every 200 ms (`VehicleState` at
~5 Hz), both `karshipta::v1::Envelope` messages. Verified against the real
gateway process (connected to a live MAVLink source) using the committed
test client, `gateway/tools/ws_client.py` (setup lines in its docstring):

```
python3 gateway/tools/ws_client.py ws://localhost:8765
```

Sample session (any WebSocket client sees the same frames):

```
State: Open
frame 0: 19 bytes, type=Binary
frame 1: 88 bytes, type=Binary
frame 2: 88 bytes, type=Binary
frame 3: 88 bytes, type=Binary
```

and, in the gateway's own log:

```
[info] client 1 connected from 127.0.0.1
[info] client 1 disconnected
```

on the client closing its connection.
