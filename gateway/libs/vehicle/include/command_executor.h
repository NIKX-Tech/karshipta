#ifndef KARSHIPTA_GATEWAY_COMMAND_EXECUTOR_H
#define KARSHIPTA_GATEWAY_COMMAND_EXECUTOR_H

#include <karshipta/v1/command.pb.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "telemetry.h"
#include "vehicle_actions.h"
#include "vehicle_mission.h"

// Consumes decoded Command messages and executes them through VehicleActions
// (and, for StartMissionCommand/PauseMissionCommand, VehicleMission's
// start()/pause()) on its own worker thread (BRIEF.md M3/M5). Transport
// threads only enqueue:
// MAVSDK Action calls block for seconds and must never stall frame delivery.
// Every command is answered: ACCEPTED when it enters the queue, then SUCCESS
// or REJECTED with a human-readable reason (gateway rule 5); commands still
// queued at shutdown are rejected, not dropped.
class CommandExecutor {
   public:
    // Receives every ack this executor produces. Invoked from the enqueueing
    // thread (ACCEPTED) and from the worker thread (terminal acks); the
    // callback must be thread-safe, which Transport::broadcast is.
    using AckCallback = std::function<void(const karshipta::v1::CommandAck&)>;

    CommandExecutor(VehicleActions& actions, TelemetryInfo& telemetry, VehicleMission& mission,
                    AckCallback on_ack);
    // worker_ is the last member, so it stops and joins before anything the
    // worker uses is destroyed; run() rejects whatever is still queued.
    ~CommandExecutor() = default;

    CommandExecutor(const CommandExecutor&) = delete;
    CommandExecutor& operator=(const CommandExecutor&) = delete;
    CommandExecutor(CommandExecutor&&) = delete;
    CommandExecutor& operator=(CommandExecutor&&) = delete;

    // Non-blocking, safe from any thread. Immediately acks ACCEPTED (or
    // REJECTED for a command that carries no action).
    void enqueue(karshipta::v1::Command command);

   private:
    void run(const std::stop_token& stop_token);
    void execute(const karshipta::v1::Command& command);
    void send_ack(const karshipta::v1::Command& command, karshipta::v1::CommandStatus status,
                  const std::string& message);
    // Maps one action to its VehicleActions/VehicleMission call; returns
    // whether it succeeded plus the ack message (blank on success for every
    // command except pause, which reports which of hold/mission-pause
    // actually ran; the real MAVSDK failure reason on rejection).
    std::pair<bool, std::string> dispatch(const karshipta::v1::Command& command);
    // Implements StartMissionCommand: always Mission::start_mission().
    [[nodiscard]] std::pair<bool, std::string> dispatch_start_mission() const;
    // Implements PauseMissionCommand: Mission::pause_mission() (mission-aware,
    // keeps the mission resumable) when the vehicle is currently flying the
    // uploaded mission, otherwise VehicleActions::hold() (manual hold,
    // unrelated to any mission) for every other flight mode.
    [[nodiscard]] std::pair<bool, std::string> dispatch_pause() const;

    VehicleActions& actions_;
    TelemetryInfo& telemetry_;
    VehicleMission& mission_;
    AckCallback on_ack_;

    std::mutex mutex_;
    std::condition_variable_any queue_changed_;
    std::deque<karshipta::v1::Command> queue_;
    std::jthread worker_;
};

#endif  // KARSHIPTA_GATEWAY_COMMAND_EXECUTOR_H
