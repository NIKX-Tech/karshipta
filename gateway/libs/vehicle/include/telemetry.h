
#ifndef KARSHIPTA_GATEWAY_TELEMETRY_H
#define KARSHIPTA_GATEWAY_TELEMETRY_H

#include <mavsdk/plugins/telemetry/telemetry.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

#include "vehicle_connection.h"

class TelemetryInfo {
   public:
    // Binds this wrapper to a connection; does not create the Telemetry plugin yet
    // (that happens lazily in ensure_telemetry() on first use).
    explicit TelemetryInfo(VehicleConnection& connection);
    // Unsubscribes every callback this instance registered, so none of them can
    // fire against a destroyed TelemetryInfo (subscribe_flight_mode()'s
    // callback captures `this`).
    ~TelemetryInfo();

    // Subscribes a callback that prints altitude/latitude/longitude each time a
    // position update arrives. Fires asynchronously on a background thread.
    void subscribe_position();
    // Clears the position subscription set by subscribe_position().
    void unsubscribe_position();
    // Subscribes a callback that prints the flight mode whenever it changes.
    void subscribe_flight_mode();
    // Clears the flight-mode subscription set by subscribe_flight_mode().
    void unsubscribe_flight_mode();
    // Subscribes a callback that prints remaining battery percentage whenever it
    // is reported.
    void subscribe_battery();
    // Clears the battery subscription set by subscribe_battery().
    void unsubscribe_battery();
    // Requests the autopilot send position updates at the given rate (Hz).
    void set_telemetry_rate(float rate);
    // Checks gyro/accelerometer/magnetometer calibration status. Returns false
    // (and logs which sensor) if any calibration is required.
    [[nodiscard]] bool check_for_calibration() const;
    // Returns the vehicle's current altitude relative to takeoff, in meters.
    [[nodiscard]] float get_relative_altitude_m() const;
    // Returns the vehicle's current estimated battery remaining, in percent (0 to 100).
    [[nodiscard]] float get_battery_remaining_percent() const;
    // Returns the vehicle's current global position (lat/lon/altitude). Default-constructed
    // (NaN fields) if the Telemetry plugin isn't available yet.
    [[nodiscard]] mavsdk::Telemetry::Position get_position() const;
    // Returns the vehicle's current velocity in the NED frame, in m/s.
    [[nodiscard]] mavsdk::Telemetry::VelocityNed get_velocity_ned() const;
    // Returns the vehicle's current heading, in degrees true north (0 to 360).
    [[nodiscard]] float get_heading_deg() const;
    // Returns the vehicle's current full battery reading (voltage + remaining percent).
    [[nodiscard]] mavsdk::Telemetry::Battery get_battery() const;
    // Returns the vehicle's current GPS fix type and satellite count.
    [[nodiscard]] mavsdk::Telemetry::GpsInfo get_gps_info() const;
    // Returns the vehicle's current raw GPS reading (adds hdop/vdop over get_gps_info()).
    [[nodiscard]] mavsdk::Telemetry::RawGps get_raw_gps() const;
    // Returns the vehicle's current flight mode.
    [[nodiscard]] mavsdk::Telemetry::FlightMode get_flight_mode() const;
    // True if the vehicle is currently armed.
    [[nodiscard]] bool is_armed() const;
    // True if the vehicle is currently airborne.
    [[nodiscard]] bool is_in_air() const;
    // True if every MAVSDK health check currently passes.
    [[nodiscard]] bool is_health_ok() const;

   private:
    // Guards telemetry_/mavsdk_keepalive_ construction in ensure_telemetry(), plus
    // position_handle_/flight_mode_handle_/battery_handle_ (set by subscribe_*(),
    // cleared by unsubscribe_*() and the destructor). Mirrors VehicleActions::init_mutex_.
    // Not held by the frequently-polled getters (get_position(), is_armed(), ...): once
    // telemetry_ is published under the lock, reading it from those is safe without
    // taking the lock on every tick.
    mutable std::mutex mutex_;
    // Owned copy of the Mavsdk core, grabbed in ensure_telemetry(). Keeps the core alive
    // for as long as `telemetry_` exists, independent of `connection_`'s own lifetime.
    mutable std::shared_ptr<mavsdk::Mavsdk> mavsdk_keepalive_;
    // The Telemetry plugin bound to the connected System. Null until ensure_telemetry()
    // lazily constructs it on first use.
    mutable std::unique_ptr<mavsdk::Telemetry> telemetry_;
    // The connection this telemetry wrapper reads from. Must outlive this object.
    VehicleConnection& connection_;
    // Last flight mode seen by subscribe_flight_mode()'s callback. Atomic because
    // MAVSDK invokes the callback on its own thread, not necessarily the caller's.
    mutable std::atomic<mavsdk::Telemetry::FlightMode> last_flight_mode_ =
        mavsdk::Telemetry::FlightMode::Unknown;

    // Handles for the long-lived subscriptions, set when the matching subscribe_*()
    // is called and cleared by unsubscribe_*(). Needed because MAVSDK's
    // unsubscribe_*() takes the exact handle subscribe_*() returned; passing
    // nullptr (or any other value) does not cancel the original subscription.
    // Guarded by mutex_.
    std::optional<mavsdk::Telemetry::PositionHandle> position_handle_;
    std::optional<mavsdk::Telemetry::FlightModeHandle> flight_mode_handle_;
    std::optional<mavsdk::Telemetry::BatteryHandle> battery_handle_;

    // Lazily creates `telemetry_` the first time it's needed. Returns false if
    // `connection_` isn't connected yet; returns true immediately if already created.
    // Locks mutex_ itself; callers must not hold it.
    bool ensure_telemetry() const;
};

#endif  // KARSHIPTA_GATEWAY_TELEMETRY_H
