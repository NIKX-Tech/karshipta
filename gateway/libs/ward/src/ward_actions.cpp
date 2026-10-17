#include "ward_actions.h"

#include <spdlog/spdlog.h>

#include <sstream>

// MAVSDK only offers operator<< for Action::Result, and fmt::streamed needs
// fmt 9 (Ubuntu 22.04's spdlog bundles fmt 8); one stringstream keeps the
// human-readable reason portable.
std::string WardActions::result_name(const mavsdk::Action::Result result) {
    std::ostringstream stream;
    stream << result;
    return stream.str();
}

// No ostream operator<< for this Result type exists in the linked MAVSDK
// build (confirmed: undefined symbol at link time, unlike Action::Result)
// - this plugin's operator<< is declared in the header but never actually
// exported. An explicit switch instead of the result_name(Action::Result)
// pattern above.
std::string WardActions::result_name(const mavsdk::MavlinkPassthrough::Result result) {
    switch (result) {
        case mavsdk::MavlinkPassthrough::Result::Success:
            return "Success";
        case mavsdk::MavlinkPassthrough::Result::ConnectionError:
            return "ConnectionError";
        case mavsdk::MavlinkPassthrough::Result::CommandNoSystem:
            return "CommandNoSystem";
        case mavsdk::MavlinkPassthrough::Result::CommandBusy:
            return "CommandBusy";
        case mavsdk::MavlinkPassthrough::Result::CommandDenied:
            return "CommandDenied";
        case mavsdk::MavlinkPassthrough::Result::CommandUnsupported:
            return "CommandUnsupported";
        case mavsdk::MavlinkPassthrough::Result::CommandTimeout:
            return "CommandTimeout";
        case mavsdk::MavlinkPassthrough::Result::CommandTemporarilyRejected:
            return "CommandTemporarilyRejected";
        case mavsdk::MavlinkPassthrough::Result::CommandFailed:
            return "CommandFailed";
        case mavsdk::MavlinkPassthrough::Result::ParamWrongType:
            return "ParamWrongType";
        case mavsdk::MavlinkPassthrough::Result::ParamNameTooLong:
            return "ParamNameTooLong";
        case mavsdk::MavlinkPassthrough::Result::ParamValueTooLong:
            return "ParamValueTooLong";
        case mavsdk::MavlinkPassthrough::Result::ParamNotFound:
            return "ParamNotFound";
        case mavsdk::MavlinkPassthrough::Result::ParamValueUnsupported:
            return "ParamValueUnsupported";
        case mavsdk::MavlinkPassthrough::Result::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

WardActions::WardActions(WardConnection& connection) : connection_(connection) {}

bool WardActions::ensure_action() const {
    std::lock_guard lock(init_mutex_);
    if (action_) return true;
    if (!connection_.is_connected()) return false;
    mavsdk_keepalive_ = connection_.get_mavsdk();
    action_ = std::make_unique<mavsdk::Action>(connection_.get_system());
    spdlog::info("action plugin created");
    return true;
}

bool WardActions::ensure_mavlink_passthrough() const {
    std::lock_guard lock(init_mutex_);
    if (mavlink_passthrough_) return true;
    if (!connection_.is_connected()) return false;
    mavsdk_keepalive_ = connection_.get_mavsdk();
    mavlink_passthrough_ = std::make_unique<mavsdk::MavlinkPassthrough>(connection_.get_system());
    spdlog::info("mavlink passthrough plugin created");
    return true;
}

mavsdk::Action::Result WardActions::log_result(const std::string& label,
                                                  const mavsdk::Action::Result result) {
    if (result != mavsdk::Action::Result::Success) {
        spdlog::error("{} failed: {}", label, result_name(result));
        return result;
    }
    spdlog::info("{} succeeded", label);
    return result;
}

mavsdk::MavlinkPassthrough::Result WardActions::log_result(
    const std::string& label, const mavsdk::MavlinkPassthrough::Result result) {
    if (result != mavsdk::MavlinkPassthrough::Result::Success) {
        spdlog::error("{} failed: {}", label, result_name(result));
        return result;
    }
    spdlog::info("{} succeeded", label);
    return result;
}

mavsdk::Action::Result WardActions::arm() const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("arm", action_->arm());
}

std::pair<mavsdk::Action::Result, float> WardActions::get_takeoff_altitude() const {
    if (ensure_action()) {
        return action_->get_takeoff_altitude();
    }
    spdlog::error("get takeoff altitude failed: action not available");
    return {mavsdk::Action::Result::Failed, 0.0f};
}

mavsdk::Action::Result WardActions::set_takeoff_altitude(const float new_altitude) const {
    if (ensure_action()) {
        const auto result = action_->set_takeoff_altitude(new_altitude);
        spdlog::info("set takeoff altitude to {:.1f}m", new_altitude);
        return result;
    }
    spdlog::error("set takeoff altitude failed: action not available");
    return mavsdk::Action::Result::Failed;
}

mavsdk::Action::Result WardActions::takeoff() const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("takeoff", action_->takeoff());
}

mavsdk::Action::Result WardActions::land() const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("land", action_->land());
}

mavsdk::Action::Result WardActions::set_return_to_launch_altitude(const float new_altitude) const {
    if (ensure_action()) {
        const auto result = action_->set_return_to_launch_altitude(new_altitude);
        spdlog::info("set return to launch altitude to {:.1f}m", new_altitude);
        return result;
    }
    spdlog::error("set return to launch altitude failed: action not available");
    return mavsdk::Action::Result::Failed;
}

mavsdk::Action::Result WardActions::return_to_launch() const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("return to launch", action_->return_to_launch());
}

mavsdk::Action::Result WardActions::disarm() const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("disarm", action_->disarm());
}

mavsdk::Action::Result WardActions::hold() const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("hold", action_->hold());
}

mavsdk::Action::Result WardActions::goto_location(const double latitude_deg,
                                                     const double longitude_deg,
                                                     const float absolute_altitude_m,
                                                     const float yaw_deg) const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("goto location", action_->goto_location(latitude_deg, longitude_deg,
                                                              absolute_altitude_m, yaw_deg));
}

mavsdk::Action::Result WardActions::kill() const {
    if (!ensure_action()) return mavsdk::Action::Result::Failed;
    return log_result("kill", action_->kill());
}

mavsdk::Action::Result WardActions::set_current_speed(const float speed_m_s) const {
    if (ensure_action()) {
        const auto result = action_->set_current_speed(speed_m_s);
        spdlog::info("set current speed to {:.1f}m/s", speed_m_s);
        return result;
    }
    spdlog::error("set current speed failed: action not available");
    return mavsdk::Action::Result::Failed;
}

mavsdk::MavlinkPassthrough::Result WardActions::set_home_position(
    const double latitude_deg, const double longitude_deg, const float altitude_msl_m) const {
    if (!ensure_mavlink_passthrough()) return mavsdk::MavlinkPassthrough::Result::ConnectionError;
    // param1=0 means "use the location given in params 5-7", not the current
    // position - MAV_CMD_DO_SET_HOME's own spec. MAV_COMP_ID_AUTOPILOT1 is
    // the standard autopilot component id (mavsdk::Mavsdk also defines
    // DEFAULT_COMPONENT_ID_AUTOPILOT as the same value, but it's private in
    // the version this builds against). COMMAND_LONG carries lat/lon as
    // float32 (~1m worst-case precision at these magnitudes), not the
    // int32-scaled precision COMMAND_INT would give - accepted here the
    // same way every other GCS sends this exact command: a home point
    // doesn't need waypoint-grade precision.
    const mavsdk::MavlinkPassthrough::CommandLong command{
        connection_.get_system()->get_system_id(),
        MAV_COMP_ID_AUTOPILOT1,
        MAV_CMD_DO_SET_HOME,
        0.0f,  // param1: 0 = use specified location, not current position
        0.0f, 0.0f, 0.0f,
        static_cast<float>(latitude_deg),
        static_cast<float>(longitude_deg),
        altitude_msl_m};
    return log_result("set home position", mavlink_passthrough_->send_command_long(command));
}
