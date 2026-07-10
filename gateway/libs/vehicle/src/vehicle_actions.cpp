//
// Created by amir abkhoshk on 10/07/2026.
//

#include "vehicle_actions.h"

#include <spdlog/spdlog.h>

DroneActions::DroneActions(VehicleConnection& d_system) : connection_(d_system) {}

bool DroneActions::ensure_action() const {
    if (action_) return true;
    if (!connection_.is_connected()) return false;
    mavsdk_keepalive_ = connection_.get_mavsdk();
    action_ = std::make_unique<mavsdk::Action>(connection_.get_system());
    spdlog::info("action plugin created");
    return true;
}

bool DroneActions::log_result(const std::string& label, const mavsdk::Action::Result result) {
    if (result != mavsdk::Action::Result::Success) {
        spdlog::error("{} failed: result={}", label, static_cast<int>(result));
        return false;
    }
    spdlog::info("{} succeeded", label);
    return true;
}

bool DroneActions::arm() const {
    if (!ensure_action()) return false;
    return log_result("arm", action_->arm());
}

std::pair<mavsdk::Action::Result, float> DroneActions::get_takeoff_altitude() const {
    if (ensure_action()) {
        return action_->get_takeoff_altitude();
    }
    spdlog::error("get takeoff altitude failed: action not available");
    return {mavsdk::Action::Result::Failed, 0.0f};
}

mavsdk::Action::Result DroneActions::set_takeoff_altitude(const float new_altitude) const {
    if (ensure_action()) {
        const auto result = action_->set_takeoff_altitude(new_altitude);
        spdlog::info("set takeoff altitude to {:.1f}m", new_altitude);
        return result;
    }
    spdlog::error("set takeoff altitude failed: action not available");
    return mavsdk::Action::Result::Failed;
}

bool DroneActions::takeoff() const {
    if (!ensure_action()) return false;
    return log_result("takeoff", action_->takeoff());
}

bool DroneActions::land() const {
    if (!ensure_action()) return false;
    return log_result("land", action_->land());
}

bool DroneActions::return_to_launch() const {
    if (!ensure_action()) return false;
    return log_result("return to launch", action_->return_to_launch());
}

bool DroneActions::disarm() const {
    if (!ensure_action()) return false;
    return log_result("disarm", action_->disarm());
}

bool DroneActions::hold() const {
    if (!ensure_action()) return false;
    return log_result("hold", action_->hold());
}

bool DroneActions::goto_location(const double latitude_deg, const double longitude_deg,
                                  const float absolute_altitude_m, const float yaw_deg) const {
    if (!ensure_action()) return false;
    return log_result(
        "goto location",
        action_->goto_location(latitude_deg, longitude_deg, absolute_altitude_m, yaw_deg));
}

bool DroneActions::do_orbit(const float radius_m, const float velocity_ms,
                             const mavsdk::Action::OrbitYawBehavior yaw_behavior,
                             const double latitude_deg, const double longitude_deg,
                             const double absolute_altitude_m) const {
    if (!ensure_action()) return false;
    return log_result(
        "do orbit",
        action_->do_orbit(radius_m, velocity_ms, yaw_behavior, latitude_deg, longitude_deg,
                           absolute_altitude_m));
}

bool DroneActions::transition_to_fixedwing() const {
    if (!ensure_action()) return false;
    return log_result("transition to fixedwing", action_->transition_to_fixedwing());
}

bool DroneActions::transition_to_multicopter() const {
    if (!ensure_action()) return false;
    return log_result("transition to multicopter", action_->transition_to_multicopter());
}

bool DroneActions::reboot() const {
    if (!ensure_action()) return false;
    return log_result("reboot", action_->reboot());
}

bool DroneActions::shutdown() const {
    if (!ensure_action()) return false;
    return log_result("shutdown", action_->shutdown());
}

bool DroneActions::terminate() const {
    if (!ensure_action()) return false;
    return log_result("terminate", action_->terminate());
}

bool DroneActions::kill() const {
    if (!ensure_action()) return false;
    return log_result("kill", action_->kill());
}

mavsdk::Action::Result DroneActions::set_actuator(const int32_t index, const float value) const {
    if (ensure_action()) {
        const auto result = action_->set_actuator(index, value);
        spdlog::info("set actuator {} to {:.2f}", index, value);
        return result;
    }
    spdlog::error("set actuator failed: action not available");
    return mavsdk::Action::Result::Failed;
}

mavsdk::Action::Result DroneActions::set_relay(const int32_t index,
                                                const mavsdk::Action::RelayCommand setting) const {
    if (ensure_action()) {
        const auto result = action_->set_relay(index, setting);
        spdlog::info("set relay {}", index);
        return result;
    }
    spdlog::error("set relay failed: action not available");
    return mavsdk::Action::Result::Failed;
}

std::pair<mavsdk::Action::Result, float> DroneActions::get_return_to_launch_altitude() const {
    if (ensure_action()) {
        return action_->get_return_to_launch_altitude();
    }
    spdlog::error("get return-to-launch altitude failed: action not available");
    return {mavsdk::Action::Result::Failed, 0.0f};
}

mavsdk::Action::Result DroneActions::set_return_to_launch_altitude(
    const float relative_altitude_m) const {
    if (ensure_action()) {
        const auto result = action_->set_return_to_launch_altitude(relative_altitude_m);
        spdlog::info("set return-to-launch altitude to {:.1f}m", relative_altitude_m);
        return result;
    }
    spdlog::error("set return-to-launch altitude failed: action not available");
    return mavsdk::Action::Result::Failed;
}

mavsdk::Action::Result DroneActions::set_current_speed(const float speed_m_s) const {
    if (ensure_action()) {
        const auto result = action_->set_current_speed(speed_m_s);
        spdlog::info("set current speed to {:.1f}m/s", speed_m_s);
        return result;
    }
    spdlog::error("set current speed failed: action not available");
    return mavsdk::Action::Result::Failed;
}

mavsdk::Action::Result DroneActions::set_gps_global_origin(
    const double latitude_deg, const double longitude_deg, const float absolute_altitude_m) const {
    if (ensure_action()) {
        const auto result =
            action_->set_gps_global_origin(latitude_deg, longitude_deg, absolute_altitude_m);
        spdlog::info("set gps global origin to lat={} lon={} alt={:.1f}m", latitude_deg,
                     longitude_deg, absolute_altitude_m);
        return result;
    }
    spdlog::error("set gps global origin failed: action not available");
    return mavsdk::Action::Result::Failed;
}
