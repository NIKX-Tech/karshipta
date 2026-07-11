# TelemetryInfo

`libs/vehicle/include/telemetry.h`, `libs/vehicle/src/telemetry.cpp`

## Overview

`TelemetryInfo` wraps MAVSDK's `Telemetry` plugin for exactly **one**
already-connected vehicle: position, battery, flight mode, and pre-arm
health. It reads through a `VehicleConnection&` rather than owning a
`System` itself, and lazily constructs the underlying `mavsdk::Telemetry`
plugin the first time any method is called, once that connection has
succeeded. It does not perform discovery, retries, or link-state tracking
(`VehicleConnection`'s job); it does not send commands (a future
`Action`-plugin class's job).

## Responsibilities

- Lazily bind a `mavsdk::Telemetry` plugin to `VehicleConnection::get_system()`
  once the connection is live (`ensure_telemetry()`), and keep the shared
  `Mavsdk` core alive for as long as that plugin exists
  (`mavsdk_keepalive_`).
- Subscribe/unsubscribe to position, flight mode, and battery updates, each
  as an independent, individually cancellable subscription.
- Poll one-shot values on demand: relative altitude, battery remaining
  percent.
- Guarantee every subscription this instance registered is cancelled on
  destruction (RAII), so no MAVSDK callback can fire against a destroyed
  `TelemetryInfo`.

## Explicitly out of scope

- **Connection lifecycle** (connect/retry/disconnect/link state). Owned by
  `VehicleConnection`; `TelemetryInfo` only reads `is_connected()`,
  `get_system()`, and `get_mavsdk()` from it.
- **Commands** (arm/disarm/takeoff/land/rtl/goto). A separate class, using
  MAVSDK's `Action` plugin against the same `VehicleConnection`.
- **Publishing to the wire.** `TelemetryInfo` currently only logs via
  spdlog; translating its data into `VehicleState` protobuf `Envelope`
  frames is M2 work, not this class's job.

## Public API

| Member | Behavior |
|---|---|
| `explicit TelemetryInfo(VehicleConnection&)` | Binds to a connection. Does not create the `Telemetry` plugin yet; that happens lazily on first use. |
| `~TelemetryInfo()` | Unsubscribes every position/flight-mode/battery subscription this instance still holds. |
| `void subscribe_position() const` | Logs `altitude=... latitude=... longitude=...` on every position update. Stores the returned `PositionHandle`. |
| `void unsubscribe_position() const` | Cancels the stored `PositionHandle`, if any. No-op if never subscribed. |
| `void subscribe_flight_mode() const` | Logs `flight mode changed: N` only when the mode actually changes (tracked via `last_flight_mode_`). Stores the returned `FlightModeHandle`. |
| `void unsubscribe_flight_mode() const` | Cancels the stored `FlightModeHandle`, if any. |
| `void subscribe_battery() const` | Logs `battery remaining=...% voltage=...V` on every battery update. Stores the returned `BatteryHandle`. |
| `void unsubscribe_battery() const` | Cancels the stored `BatteryHandle`, if any. |
| `void set_telemetry_rate(float rate) const` | Requests the autopilot send position updates at `rate` Hz via `set_rate_position()`. Logs and returns on failure. |
| `bool check_for_calibration() const` | One-shot check of gyro/accelerometer/magnetometer calibration. Logs and returns `false` for each sensor that still needs calibration. |
| `float get_relative_altitude_m() const` | Polls `telemetry_->position()` once; returns `relative_altitude_m`. |
| `float get_battery_remaining_percent() const` | Polls `telemetry_->battery()` once; returns `remaining_percent` (0 to 100). |

## Design: lazy plugin construction against a live connection

Unlike `VehicleConnection`, `TelemetryInfo` can be constructed before its
vehicle is connected:

```cpp
VehicleConnection vehicle(core, "udp://:14540");
TelemetryInfo telemetry(vehicle);       // fine, even before connect()

vehicle.connect();
telemetry.subscribe_position();   // ensure_telemetry() lazily builds
                                         // the Telemetry plugin here
```

Every public method funnels through `ensure_telemetry() const`, which:

1. Returns `true` immediately if `telemetry_` already exists.
2. Returns `false` if `connection_.is_connected()` is false (nothing to bind
   to yet); callers log an error and return without touching MAVSDK.
3. Otherwise takes a `shared_ptr<Mavsdk>` from `connection_.get_mavsdk()`
   (kept in `mavsdk_keepalive_`) and constructs `telemetry_` against
   `connection_.get_system()`.

This mirrors `VehicleConnection`'s own "receive a shared core, don't build
one" pattern: `TelemetryInfo` does not extend the `Mavsdk` core's lifetime
beyond normal `shared_ptr` reference counting, but it does guarantee the
core stays alive for as long as `telemetry_` does, independent of whether
`connection_` itself is later destroyed or disconnected (see
`VehicleConnection::disconnect()`'s note that `shared_ptr` holders like this
one are unaffected by the owning `VehicleConnection` going away).

`ensure_telemetry()` never re-binds once `telemetry_` exists, even across a
disconnect/reconnect of the underlying `VehicleConnection`. If the link
drops and comes back, callers must construct a new `TelemetryInfo` (or add
explicit rebind support) rather than expect this one to notice.

## Subscription lifecycle: handles, not `nullptr`

MAVSDK's `Telemetry::subscribe_*()` methods are handle-based: each call
returns an opaque `Handle` (e.g. `PositionHandle`), and only the matching
`unsubscribe_*(handle)` overload cancels that specific subscription.
Passing `nullptr` to `subscribe_*()` registers an additional, empty
subscription; it does not cancel anything already registered.

`TelemetryInfo` stores each handle in a `mutable std::optional<...Handle>`
member (`position_handle_`, `flight_mode_handle_`, `battery_handle_`) set
by the matching `subscribe_*()` and cleared by the matching
`unsubscribe_*()`:

```cpp
void TelemetryInfo::subscribe_position() const {
    if (!ensure_telemetry()) { ... return; }
    position_handle_ = telemetry_->subscribe_position([](const auto& position) { ... });
}

void TelemetryInfo::unsubscribe_position() const {
    if (!ensure_telemetry() || !position_handle_) return;
    telemetry_->unsubscribe_position(*position_handle_);
    position_handle_.reset();
}
```

`~TelemetryInfo()` walks the same three optionals and unsubscribes whichever
are still set, so a `TelemetryInfo` that goes out of scope while
subscribed never leaves a callback capturing a dangling `this` registered
on the `Telemetry` plugin.

## Thread safety of callback state

`subscribe_flight_mode()`'s callback runs on a MAVSDK-internal
thread, not necessarily the thread that called `subscribe_flight_mode()`.
The only piece of state it touches, `last_flight_mode_`, is
`std::atomic<mavsdk::Telemetry::FlightMode>` for exactly this reason:

```cpp
mutable std::atomic<mavsdk::Telemetry::FlightMode> last_flight_mode_;
```

Everything else on `TelemetryInfo` (`telemetry_`, `mavsdk_keepalive_`, the
handle members) is written only from `ensure_telemetry()` and the
`subscribe_*()`/`unsubscribe_*()` pairs, which callers are expected to
invoke from a single thread, consistent with `VehicleConnection` not being
thread-safe either (see its docs' Constraints section).

## RAII and ownership rules

```cpp
explicit TelemetryInfo(VehicleConnection& connection);
~TelemetryInfo();  // unsubscribes position/flight-mode/battery if still active
```

- **No default construction, no copy/move declared** (implicitly deleted by
  the reference member `connection_`): a `TelemetryInfo` cannot exist
  without a `VehicleConnection` to read from, and cannot be copied or moved
  out from under a live subscription whose callback may capture `this`.
  Mirrors `VehicleConnection`'s own move-deleted rationale.
- **Does not own `connection_`.** The caller must keep the referenced
  `VehicleConnection` alive for at least as long as this `TelemetryInfo`.
- **Destructor never throws** and unconditionally unsubscribes whatever is
  still active, so normal return, early return, and exception unwinding all
  leave no dangling MAVSDK subscription behind.

## Constraints and preconditions

- **Requires a successful `VehicleConnection::connect()` before any method
  does real work.** Every public method calls `ensure_telemetry()` first;
  if the connection isn't up yet, methods either return a default value
  (`0.0f`, `false`) or log an error and return, never throw.
- **Does not rebind across reconnects.** Once `telemetry_` is constructed
  against a `System`, it stays bound to that `System` even if the owning
  `VehicleConnection` disconnects and reconnects.
- **Not thread-safe** for concurrent calls to its own methods (subscribe/
  unsubscribe pairs, `ensure_telemetry()`). MAVSDK's own callback delivery
  runs on its internal threads regardless; only `last_flight_mode_` is
  built to tolerate that.

## Automated tests

None yet. `gateway/tests/vehicle/vehicle_connection_test.cpp` covers
`VehicleConnection`; there is no `telemetry_test.cpp`. Coverage worth adding
against the same fake-autopilot pattern used there: subscribe/unsubscribe
does not leave a dangling handle, and the destructor unsubscribes an active
position subscription.

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

`main.cpp` connects with no `expected_system_id`, sets the position rate to
1 Hz, and subscribes to both position and battery. You should see, roughly
once per second, interleaved lines like:

```
[info] connected to udp://:14540
[info] telemetry object created
[info] subscribed to position
[info] subscribed to battery
[info] altitude=0.0m latitude=47.397742 longitude=8.545594
[info] battery remaining=100% voltage=12.59V
```

until SITL is stopped, at which point `vehicle.is_connected()` goes false
and the program logs `link lost, exiting`.

## Removed on review: blocking waits

The first draft had blocking helpers (health/calibration gates, takeoff
progress, landing wait). They were M3 workflow scaffolding, could not run
inside the gateway's publish loop, and one of them looped forever after a
link loss. The M3 command executor implements those flows against the
one-shot accessors above, cancellably.
