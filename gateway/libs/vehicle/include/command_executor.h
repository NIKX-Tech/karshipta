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
    void execute(const karshipta::v1::Command& command, const std::stop_token& stop_token);
    void send_ack(const karshipta::v1::Command& command, karshipta::v1::CommandStatus status,
                  const std::string& message);
    // Maps one action to its VehicleActions/VehicleMission call; returns
    // whether it succeeded plus the ack message (blank on success for every
    // command except pause, which reports which of hold/mission-pause
    // actually ran; the real MAVSDK failure reason on rejection). Needs
    // `stop_token` only to thread through to dispatch_start_mission()/
    // dispatch_pause(); every other action still runs its (fast, real-time)
    // blocking MAVSDK call unconditionally.
    std::pair<bool, std::string> dispatch(const karshipta::v1::Command& command,
                                           const std::stop_token& stop_token);
    // Implements StartMissionCommand via VehicleMission::start_async()
    // (gateway issue #69: mission_.start() blocks the worker thread inside
    // MAVSDK's own call with no interrupt hook at all). Fires the async
    // call, then waits for its callback the same interruptible way run()
    // waits on queue_changed_: stop_token can wake this early, so a
    // shutdown request is never held hostage behind MAVSDK's own call.
    // Returns {false, "gateway shutting down"} if interrupted that way,
    // never the callback's actual result.
    [[nodiscard]] std::pair<bool, std::string> dispatch_start_mission(
        const std::stop_token& stop_token) const;
    // Implements PauseMissionCommand: VehicleMission::pause_async()
    // (mission-aware, keeps the mission resumable) when the vehicle is
    // currently flying the uploaded mission, otherwise
    // VehicleActions::hold() (manual hold, unrelated to any mission, still
    // its synchronous blocking call) for every other flight mode. Same
    // interruptible-wait behavior as dispatch_start_mission() for the
    // mission-aware branch.
    [[nodiscard]] std::pair<bool, std::string> dispatch_pause(
        const std::stop_token& stop_token) const;

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
