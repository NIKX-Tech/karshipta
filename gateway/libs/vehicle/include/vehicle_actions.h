//
// Created by amir abkhoshk on 10/07/2026.
//

#ifndef KARSHIPTA_GATEWAY_VEHICLE_ACTIONS_H
#define KARSHIPTA_GATEWAY_VEHICLE_ACTIONS_H

#include "vehicle.h"
#include <mavsdk/plugins/action/action.h>
#include <memory>
#include <string>
#include <utility>

class DroneActions {
public:
    // Binds this wrapper to a connection; does not create the Action plugin yet
    // (that happens lazily in ensure_action() on first use).
    explicit DroneActions(VehicleConnection& d_system);

    // Arms the vehicle (motors become live). Returns false on failure
    // (e.g. failsafe active, pre-arm checks not passed).
    [[nodiscard]] bool arm() const;
    // Disarms the vehicle. The autopilot rejects this while flying;
    // only a landed vehicle actually disarms.
    [[nodiscard]] bool disarm() const;
    // Reads the configured takeoff altitude (meters above ground). Lazily
    // creates the Action plugin via ensure_action() if needed; returns
    // Result::Failed with 0.0f if the connection isn't connected yet.
    [[nodiscard]] std::pair<mavsdk::Action::Result, float> get_takeoff_altitude() const;
    // Sets the takeoff altitude (meters above ground) used by takeoff().
    [[nodiscard]] mavsdk::Action::Result set_takeoff_altitude(float new_altitude) const;
    // Commands takeoff to the configured takeoff altitude. Must be armed first.
    [[nodiscard]] bool takeoff() const;
    // Commands landing at the current position.
    [[nodiscard]] bool land() const;
    // Commands return-to-launch: fly back to the home position and land.
    [[nodiscard]] bool return_to_launch() const;
    // Commands hold (loiter) at the current GPS position and altitude.
    // PX4-specific flight mode; not guaranteed on other autopilots.
    [[nodiscard]] bool hold() const;
    // Commands the vehicle to a specific global position (WGS84 lat/lon,
    // AMSL altitude in meters, NED yaw in degrees).
    [[nodiscard]] bool goto_location(double latitude_deg, double longitude_deg,
                                      float absolute_altitude_m, float yaw_deg) const;
    // Runs the orbit routine around a global position with the given radius,
    // speed and yaw behavior.
    [[nodiscard]] bool do_orbit(float radius_m, float velocity_ms,
                                 mavsdk::Action::OrbitYawBehavior yaw_behavior,
                                 double latitude_deg, double longitude_deg,
                                 double absolute_altitude_m) const;
    // Transitions a VTOL vehicle to fixed-wing flight. Fails on non-VTOL vehicles.
    [[nodiscard]] bool transition_to_fixedwing() const;
    // Transitions a VTOL vehicle to multicopter flight. Fails on non-VTOL vehicles.
    [[nodiscard]] bool transition_to_multicopter() const;
    // Reboots the autopilot, companion computer, camera and gimbal.
    [[nodiscard]] bool reboot() const;
    // Shuts down the autopilot, onboard computer, camera and gimbal. The
    // autopilot commonly rejects this unless already disarmed.
    [[nodiscard]] bool shutdown() const;
    // Runs the configured terminate routine (e.g. disarm and deploy parachute).
    [[nodiscard]] bool terminate() const;
    // Disarms the vehicle immediately regardless of landed state. The vehicle
    // will fall out of the sky if this is used while flying; emergency use only.
    [[nodiscard]] bool kill() const;

    // Sets the value of an actuator (index starts at 1, value in [-1, 1]).
    [[nodiscard]] mavsdk::Action::Result set_actuator(int32_t index, float value) const;
    // Sets the value of a relay (index starts at 0).
    [[nodiscard]] mavsdk::Action::Result set_relay(int32_t index,
                                                     mavsdk::Action::RelayCommand setting) const;
    // Reads the minimum return-to-launch altitude (meters).
    [[nodiscard]] std::pair<mavsdk::Action::Result, float> get_return_to_launch_altitude() const;
    // Sets the minimum return-to-launch altitude (meters).
    [[nodiscard]] mavsdk::Action::Result set_return_to_launch_altitude(
        float relative_altitude_m) const;
    // Sets the current speed (m/s) used for missions/repositioning. Ephemeral,
    // not stored on the vehicle.
    [[nodiscard]] mavsdk::Action::Result set_current_speed(float speed_m_s) const;
    // Sets the GPS coordinates of the vehicle's local origin (0,0,0) position.
    [[nodiscard]] mavsdk::Action::Result set_gps_global_origin(
        double latitude_deg, double longitude_deg, float absolute_altitude_m) const;

private:
    // The connection this action wrapper sends commands through. Must outlive this object.
    VehicleConnection& connection_;
    // Owned copy of the Mavsdk core, grabbed in ensure_action(). Keeps the core alive
    // for as long as `action_` exists, independent of `connection_`'s own lifetime.
    mutable std::shared_ptr<mavsdk::Mavsdk> mavsdk_keepalive_;
    // The Action plugin bound to the connected System. Null until ensure_action()
    // lazily constructs it on first use.
    mutable std::unique_ptr<mavsdk::Action> action_;

    // Lazily creates `action_` the first time it's needed. Returns false if
    // `connection_` isn't connected yet; returns true immediately if already created.
    bool ensure_action() const;
    // Logs the outcome of a command already sent to `action_` and reports whether
    // it succeeded. Shared by every bool-returning command wrapper below.
    static bool log_result(const std::string& label, mavsdk::Action::Result result);
};

#endif  // KARSHIPTA_GATEWAY_VEHICLE_ACTIONS_H
