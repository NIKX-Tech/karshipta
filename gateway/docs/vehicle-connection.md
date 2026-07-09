# VehicleConnection

`libs/vehicle/include/vehicle.h`, `libs/vehicle/src/vehicle.cpp`

## Overview

`VehicleConnection` owns the MAVLink connection lifecycle for exactly **one**
vehicle: opening it, retrying it, reporting live link state, and closing it.
It is a thin, RAII-safe handle to a `mavsdk::System`, built on top of a
`mavsdk::Mavsdk` core that is **shared** across every vehicle in the fleet.
It does not perform telemetry or send commands; it hands out the `System` it
discovered so other classes can do that.

## Responsibilities

- Register/deregister one vehicle's connection URL on a shared `Mavsdk` core.
- Discover that vehicle's `mavsdk::System` and hold it for the duration of
  the connection.
- Report live link state (`isConnected()`, `subscribeConnectionState()`).
- Retry connecting with a cancellable backoff loop
  (`connectWithRetry()`).
- Guarantee cleanup on every teardown path (RAII).

## Explicitly out of scope

- **Telemetry** (`VehicleState`: position, battery, gps, flight mode, ...).
  A separate class consumes `getSystem()` for this.
- **Commands** (`Command`/`CommandAck`, arm/disarm/takeoff/land/rtl/goto). A
  separate class, using MAVSDK's `Action` plugin against `getSystem()`.
- **Vehicle identity** (`vehicle_id`, human-facing labels like `"sitl-1"`).
  Assigned and owned by whoever constructs a fleet of these
  (`VehicleManager`), not by the connection itself.
- **Correlating a shared core's newly discovered `System` to a specific
  configured vehicle when connecting concurrently.** See Constraints below.

## Public API

| Member | Behavior |
|---|---|
| `VehicleConnection(shared_ptr<Mavsdk>, const string& drone_url)` | Stores the shared core and validates `drone_url`. Does not open a connection. |
| `static shared_ptr<Mavsdk> createSharedCore()` | Builds a `Mavsdk` core configured `ComponentType::GroundStation`. Call once per fleet; pass the result to every `VehicleConnection`. |
| `setDroneUrl(const string&)` / `getDroneUrl() const` | Get/replace the URL used by the next `connect()`. |
| `bool connect()` | Single attempt. Registers `drone_url` on the shared core (only on the first call), then waits up to `kAutopilotDiscoveryTimeoutS` (3s) for an autopilot heartbeat via `first_autopilot()`. Returns `false` on socket failure or discovery timeout. |
| `bool connectWithRetry(stop_token, retry_interval = 2s)` | Calls `connect()` in a loop, sleeping in 100ms ticks (so cancellation is responsive), until it succeeds or `stop_token` is cancelled. |
| `void disconnect()` | Drops the `System` handle and removes `drone_url` from the shared core. No-op if already disconnected — safe to call unconditionally, including from a moved-from object. |
| `bool isConnected() const` | `true` only while a `System` was discovered **and** MAVSDK currently reports its heartbeat as live. Reflects current state, not "was ever connected." |
| `shared_ptr<System> getSystem() const` | The discovered vehicle handle; `nullptr` if not connected. |
| `shared_ptr<Mavsdk> getMavsdk() const` | The shared core this connection uses. |
| `IsConnectedHandle subscribeConnectionState(callback)` | Live link up/down notifications on the discovered `System`. Throws `std::logic_error` if called before a successful `connect()`. |
| `void unsubscribeConnectionState(handle)` | Cancels a subscription. No-op if already disconnected. |

## Ownership model: one shared core, many connections

The constructor does not create the `Mavsdk` core it uses — it receives one:

```cpp
auto core = VehicleConnection::createSharedCore();

VehicleConnection a(core, "udp://:14540");
VehicleConnection b(core, "udp://:14541");
VehicleConnection c(core, "udp://:14542");
```

All three `VehicleConnection`s' `mavsdk_instance` members point at the same
`Mavsdk` object; the `shared_ptr` reference count, not any code in this
class, is what keeps that object alive until every connection using it has
been destroyed.

**Why a shared core, not one core per connection:** MAVLink is designed for
many vehicles on one link — every message carries a `system_id`/
`component_id` in its header specifically so one listener can distinguish
many vehicles. MAVSDK's `Mavsdk::systems()` returns a *vector*, and
`gateway/BRIEF.md`'s `VehicleManager` is specified as owning N `Vehicle`
objects, "each wrapping a MAVSDK `System`" — not each owning its own core. A
core per connection would mean N independent thread pools and sockets for an
N-vehicle fleet, for no benefit MAVSDK's own design already gives you for
free.

## RAII and ownership rules

```cpp
VehicleConnection() = delete;
VehicleConnection(const VehicleConnection&) = delete;
VehicleConnection& operator=(const VehicleConnection&) = delete;
VehicleConnection(VehicleConnection&&) = default;
VehicleConnection& operator=(VehicleConnection&&) = default;
~VehicleConnection();  // calls disconnect()
```

- **No default construction** — cannot exist without a URL; no
  half-configured state.
- **Copy deleted** — the class represents exclusive ownership of one
  connection's lifecycle. Copying would let two objects believe they each
  own the same `System`, each independently disconnecting/logging for what
  is really one connection. Mirrors `std::unique_ptr`, `std::thread`,
  `std::fstream`.
- **Move defaulted** — ownership transfer is fine (storing in a
  `std::vector`, returning from a factory). A moved-from object's `system`
  is null, which combines with `disconnect()`'s null-check to make
  moved-from destruction silent and safe.
- **Destructor calls `disconnect()`** — guarantees teardown regardless of how
  the object's scope ends (normal return, early return, exception
  unwinding), without relying on callers to clean up manually. `disconnect()`
  never throws, so this is safe destructor behavior.

## Constraints and preconditions

- **`subscribeConnectionState()` requires a prior successful `connect()`.**
  Calling it beforehand throws `std::logic_error`.
- **Concurrent `connect()` calls across `VehicleConnection`s sharing one core
  are not safe.** `first_autopilot()` returns whichever autopilot the shared
  core has seen — it is not scoped to a particular connection's URL, and
  MAVSDK does not expose an API mapping a `ConnectionHandle` to the `System`
  it produced. Callers must connect vehicles on a shared core **one at a
  time**. (A future system-id-aware registry at the `VehicleManager` level
  can lift this constraint; it does not belong in this class, which only
  ever reasons about its own single connection.)
- **Not thread-safe.** No internal synchronization guards `system`,
  `connection_added`, or `connection_handle`. A single `VehicleConnection`
  instance must not be accessed from more than one thread without external
  synchronization.
- **The caller must keep the shared `Mavsdk` core alive** at least as long as
  every `VehicleConnection` built on top of it — the class holds a
  `shared_ptr` but does not create or extend the core's lifetime beyond
  normal reference counting.
