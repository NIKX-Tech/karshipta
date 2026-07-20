# WardActions

`libs/ward/include/ward_actions.h`, `libs/ward/src/ward_actions.cpp`

## Overview

`WardActions` wraps MAVSDK's `Action` plugin for exactly **one**
already-connected ward. Its surface is deliberately narrow: every public
method exists because a field in `Command.action`
(`proto/karshipta/v1/command.proto`) needs it, and no method exists that a
field does not back yet. It reads through a `WardConnection&` rather than
owning a `System` itself, and lazily constructs the underlying
`mavsdk::Action` plugin the first time any method is called, mirroring
`TelemetryInfo`'s `ensure_telemetry()` pattern.

## Responsibilities

- Lazily bind a `mavsdk::Action` plugin to `WardConnection::get_system()`
  once the connection is live (`ensure_action()`), and keep the shared
  `Mavsdk` core alive for as long as that plugin exists
  (`mavsdk_keepalive_`).
- Send exactly the commands `Command.action` can carry: arm, disarm (normal
  and forced), takeoff, land, return to launch, goto, hold, and current-speed.
- Return `mavsdk::Action::Result` from every command, not a bare `bool`, so
  the reason for a rejection survives to whatever calls this class.
- Log the outcome of every command it sends (`log_result()`).

## Explicitly out of scope

- **Connection lifecycle** (connect/retry/disconnect/link state). Owned by
  `WardConnection`; `WardActions` only reads `is_connected()`,
  `get_system()`, and `get_mavsdk()` from it.
- **Telemetry** (`WardState`: position, battery, gps, flight mode, ...).
  `TelemetryInfo`'s job.
- **Anything without a `Command.action` field.** Orbit, VTOL transitions,
  reboot/shutdown/terminate, actuator/relay control, GPS origin, and the
  return-to-launch altitude getter/setter were all dropped from this class:
  none of them have a schema field or a milestone behind them yet. When a
  future milestone needs one, it starts with a schema PR
  (`proto/karshipta/v1/command.proto`), not a wrapper method added ahead of
  it.
- **Deciding whether a `DisarmCommand.force` should call `kill()` instead of
  `disarm()`.** That branch belongs to the M3 command executor that parses
  `Command` envelopes; this class only exposes both operations and documents
  which schema field each one implements (see `kill()` below).
- **Building `CommandAck`.** Translating a `mavsdk::Action::Result` into
  `CommandStatus`/`CommandAck.message` is M3 work, once `Command` envelopes
  actually arrive over the `Transport`.

## Public API

| Member | Behavior |
|---|---|
| `explicit WardActions(WardConnection&)` | Binds to a connection. Does not create the `Action` plugin yet; that happens lazily on first use. |
| `Result arm() const` | Implements `ArmCommand`. Arms the ward. |
| `Result disarm() const` | Implements `DisarmCommand{force: false}`. The autopilot rejects this while flying; only a landed ward actually disarms. |
| `pair<Result, float> get_takeoff_altitude() const` | Reads the configured takeoff altitude (meters above ground). Returns `{Result::Failed, 0.0f}` if the connection isn't up yet. |
| `Result set_takeoff_altitude(float) const` | Sets the takeoff altitude used by `takeoff()`; backs `TakeoffCommand.altitude_rel_m`. |
| `Result takeoff() const` | Implements `TakeoffCommand`. Commands takeoff to the configured altitude. Must be armed first. |
| `Result land() const` | Implements `LandCommand`. Commands landing at the current position. |
| `Result return_to_launch() const` | Implements `ReturnToLaunchCommand`. Flies back to the home position and lands. |
| `Result hold() const` | Backs `PauseMissionCommand`, once the M3 mission executor exists. Loiters at the current position and altitude. PX4-specific; not guaranteed on other autopilots. |
| `Result goto_location(double lat, double lon, float alt_m, float yaw_deg) const` | Implements `GotoCommand`. `lat`/`lon`/`alt_m` come from `GotoCommand.target`; `yaw_deg` is NED yaw. |
| `Result kill() const` | Implements `DisarmCommand{force: true}`. Disarms immediately regardless of landed state; the ward falls out of the sky if used while flying. The M3 executor must gate this on `force` and must never call it for a plain `DisarmCommand`. |
| `Result set_current_speed(float) const` | Backs `GotoCommand.speed_m_s`. Ephemeral; not stored on the ward. |

`Result` is `mavsdk::Action::Result` throughout.

## Design: Result all the way through

Every command wrapper returns the `mavsdk::Action::Result` MAVSDK gave back,
not a `bool`:

```cpp
mavsdk::Action::Result WardActions::arm() const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("arm", action_->arm());
}
```

`log_result()` logs success or failure (using MAVSDK's `operator<<` for
`Action::Result`, via `fmt::streamed()`, so the log line names the actual
failure instead of a bare integer) and passes the `Result` through
unchanged:

```cpp
mavsdk::Action::Result WardActions::log_result(const std::string& label,
                                                const mavsdk::Action::Result result) {
    if (result != mavsdk::Action::Result::Success) {
        spdlog::error("{} failed: {}", label, fmt::streamed(result));
        return result;
    }
    spdlog::info("{} succeeded", label);
    return result;
}
```

Root `gateway/CLAUDE.md` rule 5 is that every rejected command produces a
`CommandAck` with a human-readable reason. A `bool` return would erase that
reason at the exact layer that knows it (MAVSDK's `Result` enum already
distinguishes, e.g., `CommandDenied` from `Timeout` from `Unsupported`).
Returning `Result` here means the M3 executor can build `CommandAck.message`
directly from what this class already has, instead of adding a second query
just to recover a reason a `bool` had already thrown away.

## `kill()` versus `disarm()`: the schema decides, not this class

`DisarmCommand` carries one field, `force`. This class exposes both MAVSDK
operations that field can select between, `disarm()` and `kill()`, but does
not itself decide which one to call: that branch belongs to the M3 command
executor reading `DisarmCommand.force` off the wire. Documenting the mapping
here, rather than only in a comment at the M3 call site, is deliberate:
`kill()` is dangerous enough (the ward free-falls if this is sent while
airborne) that its one legitimate caller should be traceable from this class
alone, without needing to go find the executor first.

## RAII and ownership rules

Same shape as `TelemetryInfo`: no default construction, no copy/move
(implicitly deleted by the `connection_` reference member), does not own
`connection_`, and the `Action` plugin plus its `Mavsdk` keepalive are
created lazily and never rebuilt across a reconnect.

## Constraints and preconditions

- **Requires a successful `WardConnection::connect()` before any method
  does real work.** Every public method calls `ensure_action()` first; if the
  connection isn't up yet, methods return `Result::Failed` (or
  `{Result::Failed, 0.0f}` for the pair-returning getter) rather than throw.
- **Not thread-safe**, consistent with `WardConnection` and
  `TelemetryInfo`.
- **Not wired to the `Transport` yet.** Nothing in `main.cpp` constructs a
  `WardActions` or calls it; it becomes reachable only once the M3
  executor parses incoming `Command` envelopes and calls into this class.
  This is also why this PR is rebased on top of the websocket transport PR
  rather than the other way around.

## Automated tests

None yet. Coverage worth adding against the same fake-autopilot pattern used
by `gateway/tests/ward/ward_connection_test.cpp`: `arm()`/`takeoff()`/
`land()` return `Result::Success` against a healthy fake autopilot, and every
method returns `Result::Failed` (not a thrown exception) when called before
`connect()` succeeds.

## Manual verification

With one PX4 SITL container running:

```
docker run --rm -it -p 14550:14550/udp -p 14540:14540/udp px4io/px4-sitl:latest
```

This class is not yet reachable from `main.cpp` (see Constraints above), so
it was exercised with a small standalone harness linked against the
`ward` library: connect, `TelemetryInfo::check_drone_health()`, then
`arm()` / `takeoff()` / (wait for `check_current_takeoff_process(2.5f)`) /
`land()` / `landing_state()`. Observed output against real SITL:

```
[info] connected to udp://:14540 (system_id=1)
[info] waiting for pre-arm health checks...
[info] ward not ready to arm, waiting on:
[info]   - GPS fix
[info]   - local position estimate
[info] == arm ==
[info] action plugin created
[info] arm succeeded
[info] arm() returned 1
[info] == takeoff ==
[info] takeoff succeeded
[info] takeoff() returned 1
[info] takeoff altitude: -0.1m
[info] takeoff altitude: 0.6m
[info] takeoff altitude: 1.6m
[info] takeoff altitude: 2.2m
[info] takeoff altitude: 2.5m
[info] == land ==
[info] land succeeded
[info] land() returned 1
[info] landing altitude: 2.7m
[info] landing altitude: 1.0m
[info] landing altitude: 0.1m
[info] landing altitude: -0.4m
[info] disarmed, exiting
```

(`1` is `mavsdk::Action::Result::Success`; altitude lines are
`TelemetryInfo::check_current_takeoff_process()` /
`landing_state()` polling, trimmed here for length.) The ward armed,
climbed to the default 2.5m takeoff altitude, landed, and disarmed on its
own, with no manual intervention between `arm()` and the process exiting.
