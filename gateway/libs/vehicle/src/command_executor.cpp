#include "command_executor.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <limits>
#include <utility>

namespace {

// Command speed 0 means "autopilot default" (schema comment); only forward a
// real request.
constexpr float kNoSpeedRequested = 0.0f;

// Normalizes a MAVSDK Result into {success, message}: blank message on
// success unless the caller supplies one (only PauseMissionCommand's
// hold/pause distinction needs that), the real failure reason otherwise.
std::pair<bool, std::string> action_outcome(const mavsdk::Action::Result result,
                                             std::string success_message = "") {
    if (result == mavsdk::Action::Result::Success) {
        return {true, std::move(success_message)};
    }
    return {false, VehicleActions::result_name(result)};
}

std::pair<bool, std::string> mission_outcome(const mavsdk::Mission::Result result,
                                              std::string success_message = "") {
    if (result == mavsdk::Mission::Result::Success) {
        return {true, std::move(success_message)};
    }
    return {false, VehicleMission::result_name(result)};
}

const char* action_case_name(const karshipta::v1::Command& command) {
    switch (command.action_case()) {
        case karshipta::v1::Command::kArm:
            return "arm";
        case karshipta::v1::Command::kDisarm:
            return "disarm";
        case karshipta::v1::Command::kTakeoff:
            return "takeoff";
        case karshipta::v1::Command::kLand:
            return "land";
        case karshipta::v1::Command::kRtl:
            return "rtl";
        case karshipta::v1::Command::kGoto:
            return "goto";
        case karshipta::v1::Command::kStartMission:
            return "start_mission";
        case karshipta::v1::Command::kPauseMission:
            return "pause_mission";
        default:
            return "none";
    }
}

}  // namespace

CommandExecutor::CommandExecutor(VehicleActions& actions, TelemetryInfo& telemetry,
                                 VehicleMission& mission, AckCallback on_ack)
    : actions_(actions),
      telemetry_(telemetry),
      mission_(mission),
      on_ack_(std::move(on_ack)),
      worker_([this](const std::stop_token& stop_token) { run(stop_token); }) {}

void CommandExecutor::enqueue(karshipta::v1::Command command) {
    if (command.action_case() == karshipta::v1::Command::ACTION_NOT_SET) {
        send_ack(command, karshipta::v1::COMMAND_STATUS_REJECTED, "command has no action");
        return;
    }
    send_ack(command, karshipta::v1::COMMAND_STATUS_ACCEPTED, "queued");
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(command));
    }
    queue_changed_.notify_one();
}

void CommandExecutor::run(const std::stop_token& stop_token) {
    while (true) {
        karshipta::v1::Command command;
        {
            std::unique_lock lock(mutex_);
            if (!queue_changed_.wait(lock, stop_token, [this] { return !queue_.empty(); })) {
                break;  // stop requested and nothing runnable
            }
            // wait() returns true whenever the queue is non-empty, even after a
            // stop request; without this check a stopping executor would keep
            // executing queued commands instead of rejecting them below.
            if (stop_token.stop_requested()) {
                break;
            }
            command = std::move(queue_.front());
            queue_.pop_front();
        }
        execute(command);
    }
    // Reject, never drop, whatever was still waiting when the gateway went down.
    std::lock_guard lock(mutex_);
    for (const auto& leftover : queue_) {
        send_ack(leftover, karshipta::v1::COMMAND_STATUS_REJECTED, "gateway shutting down");
    }
    queue_.clear();
}

void CommandExecutor::execute(const karshipta::v1::Command& command) {
    spdlog::info("executing {} for {} (command_id={})", action_case_name(command),
                 command.vehicle_id(), command.command_id());
    const auto [success, message] = dispatch(command);
    send_ack(command,
             success ? karshipta::v1::COMMAND_STATUS_SUCCESS : karshipta::v1::COMMAND_STATUS_REJECTED,
             message);
}

std::pair<bool, std::string> CommandExecutor::dispatch(const karshipta::v1::Command& command) {
    using Result = mavsdk::Action::Result;

    const auto describe = [](const Result result) { return VehicleActions::result_name(result); };

    switch (command.action_case()) {
        case karshipta::v1::Command::kArm:
            return action_outcome(actions_.arm());
        case karshipta::v1::Command::kDisarm:
            // force means "stop the motors no matter what": MAVSDK kill(). The
            // console gates this behind its own confirmation dialog.
            return action_outcome(command.disarm().force() ? actions_.kill() : actions_.disarm());
        case karshipta::v1::Command::kTakeoff: {
            if (command.takeoff().altitude_rel_m() > 0.0f) {
                const auto set_result =
                    actions_.set_takeoff_altitude(command.takeoff().altitude_rel_m());
                if (set_result != Result::Success) {
                    return {false, "setting takeoff altitude failed: " + describe(set_result)};
                }
            }
            return action_outcome(actions_.takeoff());
        }
        case karshipta::v1::Command::kLand:
            return action_outcome(actions_.land());
        case karshipta::v1::Command::kRtl:
            return action_outcome(actions_.return_to_launch());
        case karshipta::v1::Command::kGoto: {
            if (!command.goto_().has_target()) {
                return {false, "goto has no target"};
            }
            if (command.goto_().speed_m_s() > kNoSpeedRequested) {
                const auto speed_result = actions_.set_current_speed(command.goto_().speed_m_s());
                if (speed_result != Result::Success) {
                    return {false, "setting speed failed: " + describe(speed_result)};
                }
            }
            const auto& target = command.goto_().target();
            // The console may send zero altitudes meaning "keep the current
            // altitude"; goto_location needs MSL, so read it from telemetry.
            float altitude_msl_m = target.altitude_msl_m();
            if (altitude_msl_m <= 0.0f) {
                const auto position = telemetry_.get_position();
                altitude_msl_m =
                    target.altitude_rel_m() > 0.0f
                        ? (position.absolute_altitude_m - position.relative_altitude_m) +
                              target.altitude_rel_m()
                        : position.absolute_altitude_m;
            }
            return action_outcome(actions_.goto_location(
                target.latitude_deg(), target.longitude_deg(), altitude_msl_m,
                std::numeric_limits<float>::quiet_NaN()));  // NaN keeps the current yaw
        }
        case karshipta::v1::Command::kStartMission:
            return dispatch_start_mission();
        case karshipta::v1::Command::kPauseMission:
            return dispatch_pause();
        case karshipta::v1::Command::ACTION_NOT_SET:
            break;
    }
    return {false, "command has no action"};
}

std::pair<bool, std::string> CommandExecutor::dispatch_start_mission() const {
    return mission_outcome(mission_.start());
}

std::pair<bool, std::string> CommandExecutor::dispatch_pause() const {
    if (telemetry_.get_flight_mode() == mavsdk::Telemetry::FlightMode::Mission) {
        return mission_outcome(mission_.pause(), "pause");
    }
    return action_outcome(actions_.hold(), "hold");
}

void CommandExecutor::send_ack(const karshipta::v1::Command& command,
                               const karshipta::v1::CommandStatus status,
                               const std::string& message) {
    karshipta::v1::CommandAck ack;
    ack.set_command_id(command.command_id());
    ack.set_vehicle_id(command.vehicle_id());
    ack.set_status(status);
    ack.set_message(message);
    if (status == karshipta::v1::COMMAND_STATUS_REJECTED) {
        spdlog::warn("{} for {} rejected: {}", action_case_name(command), command.vehicle_id(),
                     message);
    }
    if (on_ack_) {
        on_ack_(ack);
    }
}
