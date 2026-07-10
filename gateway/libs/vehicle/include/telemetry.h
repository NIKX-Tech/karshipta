//
// Created by amir abkhoshk on 09/07/2026.
//


#ifndef KARSHIPTA_GATEWAY_TELEMETRY_H
#define KARSHIPTA_GATEWAY_TELEMETRY_H

#include  "vehicle.h"
#include <mavsdk/plugins/telemetry/telemetry.h>
#include <atomic>
#include <memory>
#include <chrono>
#include <optional>

class TelemetryInfo {
public:
    // Binds this wrapper to a connection; does not create the Telemetry plugin yet
    // (that happens lazily in ensure_telemetry() on first use).
    explicit TelemetryInfo(VehicleConnection& d_connection);
    // Unsubscribes every callback this instance registered, so none of them can
    // fire against a destroyed TelemetryInfo (subscribe_drone_flight_mode()'s
    // callback captures `this`).
    ~TelemetryInfo();

    // Subscribes a callback that prints altitude/latitude/longitude each time a
    // position update arrives. Fires asynchronously on a background thread.
    void subscribe_drone_position() const;
    // Clears the position subscription set by subscribe_drone_position().
    void unsubscribe_drone_position() const;
    // Subscribes a callback that prints the flight mode whenever it changes.
    void subscribe_drone_flight_mode() const;
    // Clears the flight-mode subscription set by subscribe_drone_flight_mode().
    void unsubscribe_drone_flight_mode() const;
    // Subscribes a callback that prints remaining battery percentage whenever it
    // is reported.
    void subscribe_drone_battery() const;
    // Clears the battery subscription set by subscribe_drone_battery().
    void unsubscribe_drone_battery() const;
    // Requests the autopilot send position updates at the given rate (Hz).
    void set_telemetry_rate(float rate) const;
    // Blocks, polling once per second, until all pre-arm health checks
    // (GPS fix, local position, home position) pass. Returns true once healthy.
    [[nodiscard]] bool check_drone_health() const;
    // Blocks until the health-all-ok callback fires true once, via a
    // promise/future pair instead of polling; unsubscribes itself once satisfied,
    // so it never leaves a dangling subscription behind.
    void async_check_drone_health() const;
    // Checks gyro/accelerometer/magnetometer calibration status. Returns false
    // (and logs which sensor) if any calibration is required.
    [[nodiscard]] bool check_for_calibration() const;
    // Returns the vehicle's current altitude relative to takeoff, in meters.
    [[nodiscard]] float get_relative_altitude_m() const;
    // Returns the vehicle's current estimated battery remaining, in percent (0 to 100).
    [[nodiscard]] float get_battery_remaining_percent() const;
    // Blocks, polling once per second, until relative altitude reaches target_alt.
    void check_current_takeoff_process(float target_alt) const;
    // Blocks, polling once per second and printing altitude, until the vehicle disarms.
    void landing_state() const;

private:
    // Owned copy of the Mavsdk core, grabbed in ensure_telemetry(). Keeps the core alive
    // for as long as `telemetry_` exists, independent of `connection_`'s own lifetime.
    mutable std::shared_ptr<mavsdk::Mavsdk> mavsdk_keepalive_;
    // The Telemetry plugin bound to the connected System. Null until ensure_telemetry()
    // lazily constructs it on first use.
    mutable std::unique_ptr<mavsdk::Telemetry> telemetry_;
    // The connection this telemetry wrapper reads from. Must outlive this object.
    VehicleConnection& connection_;
    // Last flight mode seen by subscribe_drone_flight_mode()'s callback. Atomic because
    // MAVSDK invokes the callback on its own thread, not necessarily the caller's.
    mutable std::atomic<mavsdk::Telemetry::FlightMode> last_flight_mode_ =
        mavsdk::Telemetry::FlightMode::Unknown;

    // Handles for the long-lived subscriptions, set when the matching subscribe_*()
    // is called and cleared by unsubscribe_*(). Needed because MAVSDK's
    // unsubscribe_*() takes the exact handle subscribe_*() returned; passing
    // nullptr (or any other value) does not cancel the original subscription.
    mutable std::optional<mavsdk::Telemetry::PositionHandle> position_handle_;
    mutable std::optional<mavsdk::Telemetry::FlightModeHandle> flight_mode_handle_;
    mutable std::optional<mavsdk::Telemetry::BatteryHandle> battery_handle_;

    // Lazily creates `telemetry_` the first time it's needed. Returns false if
    // `connection_` isn't connected yet; returns true immediately if already created.
    bool ensure_telemetry() const;
};



#endif  // KARSHIPTA_GATEWAY_TELEMETRY_H
