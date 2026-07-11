# VehicleConnection

`libs/vehicle/include/vehicle_connection.h`, `libs/vehicle/src/vehicle_connection.cpp`

## Overview

`VehicleConnection` owns the MAVLink connection lifecycle for exactly **one**
vehicle: opening it, retrying it, reporting live link state, and closing it.
It is a thin, RAII-safe handle to a `mavsdk::System`, built on top of a
`mavsdk::Mavsdk` core that is **shared** across every vehicle in the fleet.
It does not perform telemetry or send commands; it hands out the `System` it
discovered so other classes can do that.

## Responsibilities

- Register/deregister one vehicle's connection URL on a shared `Mavsdk` core.
- Discover that vehicle's `mavsdk::System` by MAVLink system id and hold it
  for the duration of the connection.
- Report live link state (`is_connected()`, `subscribe_connection_state()`).
- Retry connecting with a cancellable backoff loop
  (`connect_with_retry()`).
- Guarantee cleanup on every teardown path (RAII), including cancelling every
  subscription it handed out.

## Explicitly out of scope

- **Telemetry** (`VehicleState`: position, battery, gps, flight mode, ...).
  A separate class consumes `get_system()` for this.
- **Commands** (`Command`/`CommandAck`, arm/disarm/takeoff/land/rtl/goto). A
  separate class, using MAVSDK's `Action` plugin against `get_system()`.
- **Vehicle identity beyond the MAVLink system id** (`vehicle_id`,
  human-facing labels like `"sitl-1"`, and the `vehicle_id -> system_id`
  mapping itself). Assigned and owned by whoever constructs a fleet of these
  (`VehicleManager`), which passes the resolved system id into the
  constructor.

## Public API

| Member | Behavior |
|---|---|
| `VehicleConnection(shared_ptr<Mavsdk>, const string& connection_url, optional<uint32_t> expected_system_id = nullopt)` | Stores the shared core and validates `connection_url`. Does not open a connection. `expected_system_id` is the MAVLink system id (`VehicleInfo.mavlink_system_id`) this connection must bind to; omit only for the single-vehicle M1 case. |
| `static shared_ptr<Mavsdk> create_shared_core()` | Builds a `Mavsdk` core configured `ComponentType::GroundStation`. Call once per fleet; pass the result to every `VehicleConnection`. |
| `set_connection_url(const string&)` / `get_connection_url() const` | Get/replace the URL used by the next `connect()`. |
| `ConnectResult connect()` | Single attempt. Registers `connection_url` on the shared core (only on the first call), then waits up to `kAutopilotDiscoveryTimeoutS` (3s) for the expected system id (or, if none was configured, any autopilot via `first_autopilot()`). Returns `kSuccess`, `kSocketFailure` (adding the connection itself failed), or `kDiscoveryTimeout` (nothing matching answered in time), so callers can distinguish the two failure modes instead of a bare `false`. |
| `bool connect_with_retry(stop_token, retry_interval = 2s)` | Calls `connect()` in a loop, sleeping in 100ms ticks (so cancellation is responsive), until it succeeds or `stop_token` is cancelled. |
| `void disconnect()` | Cancels every `subscribe_connection_state()` handle this instance issued, drops the `System` handle, and removes `connection_url` from the shared core. No-op if already disconnected: safe to call unconditionally. |
| `bool is_connected() const` | `true` only while a `System` was discovered **and** MAVSDK currently reports its heartbeat as live. Reflects current state, not "was ever connected." |
| `shared_ptr<System> get_system() const` | The discovered vehicle handle; `nullptr` if not connected. |
| `shared_ptr<Mavsdk> get_mavsdk() const` | The shared core this connection uses. |
| `IsConnectedHandle subscribe_connection_state(callback)` | Live link up/down notifications on the discovered `System`. Throws `std::logic_error` if called before a successful `connect()`. Tracked internally so `disconnect()` can cancel it. |
| `void unsubscribe_connection_state(handle)` | Cancels a subscription early. No-op if already disconnected. |

## Ownership model: one shared core, many connections

The constructor does not create the `Mavsdk` core it uses: it receives one,
plus the MAVLink system id it is responsible for:

```cpp
auto core = VehicleConnection::create_shared_core();

VehicleConnection a(core, "udp://:14540", 1);
VehicleConnection b(core, "udp://:14541", 2);
VehicleConnection c(core, "udp://:14542", 3);
```

All three `VehicleConnection`s' `mavsdk_` members point at the same
`Mavsdk` object; the `shared_ptr` reference count, not any code in this
class, is what keeps that object alive until every connection using it has
been destroyed. Because each instance waits for its own system id rather
than "whichever autopilot showed up first", `a`, `b`, and `c` can call
`connect()`/`connect_with_retry()` concurrently and from independent
`std::jthread`s without risk of binding to the wrong airframe.

**Why a shared core, not one core per connection:** MAVLink is designed for
many vehicles on one link: every message carries a `system_id`/
`component_id` in its header specifically so one listener can distinguish
many vehicles. MAVSDK's `Mavsdk::systems()` returns a *vector*, and
`gateway/BRIEF.md`'s `VehicleManager` is specified as owning N `Vehicle`
objects, "each wrapping a MAVSDK `System`": not each owning its own core. A
core per connection would mean N independent thread pools and sockets for an
N-vehicle fleet, for no benefit MAVSDK's own design already gives you for
free.

## Identity: system id, not discovery order

`connect()` never relies on "whichever `System` the shared core happens to
have seen" to decide which vehicle it just bound to. If `expected_system_id`
was given to the constructor:

- `find_system()` first checks `Mavsdk::systems()` for an already-discovered
  match (covers reconnects, where the System may already exist on the shared
  core from before the link dropped).
- Otherwise `wait_for_system()` subscribes via `subscribe_on_new_system()`
  and checks each newly discovered `System::get_system_id()` against the
  expected id, resolving a promise the first time it matches, up to
  `kAutopilotDiscoveryTimeoutS`.

`expected_system_id` is optional so the single-vehicle M1 program (one
`VehicleConnection`, no config file yet) can keep using bare
`first_autopilot()`. As soon as a config file exists mapping
`vehicle_id -> mavlink_system_id`, every constructed `VehicleConnection`
should be given its id.

## RAII and ownership rules

```cpp
VehicleConnection() = delete;
VehicleConnection(const VehicleConnection&) = delete;
VehicleConnection& operator=(const VehicleConnection&) = delete;
VehicleConnection(VehicleConnection&&) = delete;
VehicleConnection& operator=(VehicleConnection&&) = delete;
~VehicleConnection();  // calls disconnect()
```

- **No default construction**: cannot exist without a URL; no
  half-configured state.
- **Copy deleted**: the class represents exclusive ownership of one
  connection's lifecycle. Copying would let two objects believe they each
  own the same `System`, each independently disconnecting/logging for what
  is really one connection. Mirrors `std::unique_ptr`, `std::thread`,
  `std::fstream`.
- **Move deleted**: `subscribe_connection_state()` callbacks can capture
  `this`; moving the object out from under a live callback would leave it
  pointing at a stale address. Nothing in the manager design needs to move a
  live connection (build it in place, e.g. in a `std::vector` via
  `emplace_back` with a fleet-sized reservation, or hold it through a
  `unique_ptr`).
- **Destructor calls `disconnect()`**: guarantees teardown regardless of how
  the object's scope ends (normal return, early return, exception
  unwinding), without relying on callers to clean up manually. `disconnect()`
  never throws, so this is safe destructor behavior, and it cancels every
  subscription this instance handed out before the object goes away.

## Constraints and preconditions

- **`subscribe_connection_state()` requires a prior successful `connect()`.**
  Calling it beforehand throws `std::logic_error`.
- **Not thread-safe.** No internal synchronization guards `system_`,
  `connection_added_`, or `connection_handle_`. A single `VehicleConnection`
  instance must not be accessed from more than one thread without external
  synchronization.
- **The caller must keep the shared `Mavsdk` core alive** at least as long as
  every `VehicleConnection` built on top of it: the class holds a
  `shared_ptr` but does not create or extend the core's lifetime beyond
  normal reference counting.

## Automated tests

`gateway/tests/vehicle/vehicle_connection_test.cpp` (GoogleTest, built with
`-DKARSHIPTA_GATEWAY_BUILD_TESTS=ON`):

- `RejectsEmptyDroneUrl`: constructor throws `std::invalid_argument` on an
  empty URL.
- `ConnectTimesOutWhenNothingListens`: `connect()` against a loopback port
  with no sender returns `kDiscoveryTimeout` (takes ~3s, the discovery
  timeout).
- `DoesNotBindToMismatchedSystemId`: a fake autopilot heartbeats as one
  system id while the connection expects a different one; `connect()` times
  out rather than binding to the wrong system.
- `BindsToMatchingSystemId`: a fake autopilot heartbeats as the expected
  system id; `connect()` succeeds and `get_system()->get_system_id()`
  matches.
- `DisconnectIsIdempotentAndSafeBeforeConnect` /
  `DisconnectIsIdempotentAfterConnect`: calling `disconnect()` more than once,
  with or without a prior successful `connect()`, never throws.
- `SubscribeBeforeConnectThrows`: `subscribe_connection_state()` before a
  successful `connect()` throws `std::logic_error`.

The fake autopilot is a second, independent `mavsdk::Mavsdk` core configured
with `Configuration(system_id, component_id, always_send_heartbeats=true)`,
standing in for PX4 SITL so the system-id tests do not depend on Docker or a
real vehicle.

## Manual verification

With one PX4 SITL container running:

```
docker run --rm -it -p 14550:14550/udp -p 14540:14540/udp px4io/px4-sitl:latest
```

Build and run the gateway (`gateway/CLAUDE.local.md` has the full command
list):

```
cmake -S gateway -B gateway/build
cmake --build gateway/build
./gateway/build/src/karshipta_gateway
```

`main.cpp` connects with no `expected_system_id`, so it falls back to
`first_autopilot()`; you should see:

```
[info] connected to udp://:14540 (system_id=1)
[info] connected to udp://:14540 (is_connected=true)
[info] disconnected from udp://:14540
[info] is_connected after disconnect: false
```

To exercise the system-id path directly, construct a `VehicleConnection`
with the SITL's system id (PX4 SITL defaults to system id 1):

```cpp
VehicleConnection vehicle(VehicleConnection::create_shared_core(), "udp://:14540", 1);
```

and confirm the same `connected to ... (system_id=1)` log line appears. To
see the mismatch path, pass any other id (e.g. `2`): `connect()` should
return `kDiscoveryTimeout` and log `no matching autopilot discovered on
udp://:14540 within 3s` after roughly 3 seconds, even though SITL is running
and answering heartbeats.
