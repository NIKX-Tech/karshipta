#ifndef KARSHIPTA_GATEWAY_COMMAND_EXECUTOR_H
#define KARSHIPTA_GATEWAY_COMMAND_EXECUTOR_H

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <karshipta/v1/command.pb.h>

#include "telemetry.h"
#include "vehicle_actions.h"

// Consumes decoded Command messages and executes them through VehicleActions
// on its own worker thread (BRIEF.md M3). Transport threads only enqueue:
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

    CommandExecutor(VehicleActions& actions, TelemetryInfo& telemetry, AckCallback on_ack);
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
    // Maps one action to its VehicleActions call; returns the result plus the
    // reason to report when it failed.
    std::pair<mavsdk::Action::Result, std::string> dispatch(const karshipta::v1::Command& command);

    VehicleActions& actions_;
    TelemetryInfo& telemetry_;
    AckCallback on_ack_;

    std::mutex mutex_;
    std::condition_variable_any queue_changed_;
    std::deque<karshipta::v1::Command> queue_;
    std::jthread worker_;
};

#endif  // KARSHIPTA_GATEWAY_COMMAND_EXECUTOR_H
