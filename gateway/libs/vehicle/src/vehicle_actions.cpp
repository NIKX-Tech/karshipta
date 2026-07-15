#include "vehicle_actions.h"

#include <spdlog/spdlog.h>

#include <sstream>

// MAVSDK only offers operator<< for Action::Result, and fmt::streamed needs
// fmt 9 (Ubuntu 22.04's spdlog bundles fmt 8); one stringstream keeps the
// human-readable reason portable.
std::string VehicleActions::result_name(const mavsdk::Action::Result result) {
    std::ostringstream stream;
    stream << result;
    return stream.str();
}

VehicleActions::VehicleActions(VehicleConnection& connection) : connection_(connection) {}

bool VehicleActions::ensure_action() const {
    std::lock_guard lock(init_mutex_);
    if (action_) return true;
    if (!connection_.is_connected()) return false;
    mavsdk_keepalive_ = connection_.get_mavsdk();
    action_ = std::make_unique<mavsdk::Action>(connection_.get_system());
    spdlog::info("action plugin created");
    return true;
}

mavsdk::Action::Result VehicleActions::log_result(const std::string& label,
                                                  const mavsdk::Action::Result result) {
    if (result != mavsdk::Action::Result::Success) {
        spdlog::error("{} failed: {}", label, result_name(result));
        return result;
    }
    spdlog::info("{} succeeded", label);
    return result;
}

mavsdk::Action::Result VehicleActions::arm() const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("arm", action_->arm());
}

std::pair<mavsdk::Action::Result, float> VehicleActions::get_takeoff_altitude() const {
    if (ensure_action()) {
        return action_->get_takeoff_altitude();
    }
    spdlog::error("get takeoff altitude failed: action not available");
    return {mavsdk::Action::Result::Failed, 0.0f};
}

mavsdk::Action::Result VehicleActions::set_takeoff_altitude(const float new_altitude) const {
    if (ensure_action()) {
        const auto result = action_->set_takeoff_altitude(new_altitude);
        spdlog::info("set takeoff altitude to {:.1f}m", new_altitude);
        return result;
    }
    spdlog::error("set takeoff altitude failed: action not available");
    return mavsdk::Action::Result::Failed;
}

mavsdk::Action::Result VehicleActions::takeoff() const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("takeoff", action_->takeoff());
}

mavsdk::Action::Result VehicleActions::land() const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("land", action_->land());
}

mavsdk::Action::Result VehicleActions::return_to_launch() const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("return to launch", action_->return_to_launch());
}

mavsdk::Action::Result VehicleActions::disarm() const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("disarm", action_->disarm());
}

mavsdk::Action::Result VehicleActions::hold() const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("hold", action_->hold());
}

mavsdk::Action::Result VehicleActions::goto_location(const double latitude_deg,
                                                     const double longitude_deg,
                                                     const float absolute_altitude_m,
                                                     const float yaw_deg) const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("goto location", action_->goto_location(latitude_deg, longitude_deg,
                                                              absolute_altitude_m, yaw_deg));
}

mavsdk::Action::Result VehicleActions::kill() const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("kill", action_->kill());
}

mavsdk::Action::Result VehicleActions::set_current_speed(const float speed_m_s) const {
    if (ensure_action()) {
        const auto result = action_->set_current_speed(speed_m_s);
        spdlog::info("set current speed to {:.1f}m/s", speed_m_s);
        return result;
    }
    spdlog::error("set current speed failed: action not available");
    return mavsdk::Action::Result::Failed;
}
