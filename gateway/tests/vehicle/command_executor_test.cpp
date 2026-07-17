#include "command_executor.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "telemetry.h"
#include "vehicle_actions.h"
#include "vehicle_connection.h"
#include "vehicle_mission.h"

namespace {

// Collects acks across threads and lets tests wait for a terminal one.
class AckCollector {
public:
    void add(const karshipta::v1::CommandAck& ack) {
        {
            std::lock_guard lock(mutex_);
            acks_.push_back(ack);
        }
        changed_.notify_all();
    }

    // Waits until a SUCCESS/REJECTED/TIMEOUT ack for command_id arrives.
    bool wait_terminal(const std::string& command_id,
                       std::chrono::seconds timeout = std::chrono::seconds(5)) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] {
            for (const auto& ack : acks_) {
                if (ack.command_id() == command_id &&
                    ack.status() != karshipta::v1::COMMAND_STATUS_ACCEPTED) {
                    return true;
                }
            }
            return false;
        });
    }

    std::vector<karshipta::v1::CommandAck> for_command(const std::string& command_id) {
        std::lock_guard lock(mutex_);
        std::vector<karshipta::v1::CommandAck> out;
        for (const auto& ack : acks_) {
            if (ack.command_id() == command_id) out.push_back(ack);
        }
        return out;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<karshipta::v1::CommandAck> acks_;
};

// Executor against a never-connected vehicle: every MAVSDK-backed command
// must come back REJECTED with a readable reason, not hang or crash.
class CommandExecutorTest : public ::testing::Test {
protected:
    CommandExecutorTest()
        : core_(VehicleConnection::create_shared_core()),
          vehicle_(core_, "udpin://127.0.0.1:24990"),
          actions_(vehicle_),
          telemetry_(vehicle_),
          mission_(vehicle_),
          executor_(actions_, telemetry_, mission_, [this](const karshipta::v1::CommandAck& ack) {
              collector_.add(ack);
          }) {}

    karshipta::v1::Command make_command(const std::string& id) {
        karshipta::v1::Command command;
        command.set_command_id(id);
        command.set_vehicle_id("sitl-1");
        command.set_timestamp_ms(0);
        return command;
    }

    void expect_accepted_then_rejected(const std::string& id) {
        ASSERT_TRUE(collector_.wait_terminal(id)) << "no terminal ack for " << id;
        const auto acks = collector_.for_command(id);
        ASSERT_GE(acks.size(), 2u);
        EXPECT_EQ(acks.front().status(), karshipta::v1::COMMAND_STATUS_ACCEPTED);
        EXPECT_EQ(acks.back().status(), karshipta::v1::COMMAND_STATUS_REJECTED);
        EXPECT_FALSE(acks.back().message().empty())
            << "rejection must carry a human-readable reason";
    }

    std::shared_ptr<mavsdk::Mavsdk> core_;
    VehicleConnection vehicle_;
    VehicleActions actions_;
    TelemetryInfo telemetry_;
    VehicleMission mission_;
    AckCollector collector_;
    CommandExecutor executor_;
};

}  // namespace

TEST_F(CommandExecutorTest, EveryActionKindAnswersAcceptedThenTerminal) {
    auto arm = make_command("cmd-arm");
    arm.mutable_arm();
    executor_.enqueue(arm);
    expect_accepted_then_rejected("cmd-arm");

    auto disarm = make_command("cmd-disarm");
    disarm.mutable_disarm()->set_force(false);
    executor_.enqueue(disarm);
    expect_accepted_then_rejected("cmd-disarm");

    auto force_disarm = make_command("cmd-kill");
    force_disarm.mutable_disarm()->set_force(true);
    executor_.enqueue(force_disarm);
    expect_accepted_then_rejected("cmd-kill");

    auto takeoff = make_command("cmd-takeoff");
    takeoff.mutable_takeoff()->set_altitude_rel_m(20.0f);
    executor_.enqueue(takeoff);
    expect_accepted_then_rejected("cmd-takeoff");

    auto land = make_command("cmd-land");
    land.mutable_land();
    executor_.enqueue(land);
    expect_accepted_then_rejected("cmd-land");

    auto rtl = make_command("cmd-rtl");
    rtl.mutable_rtl();
    executor_.enqueue(rtl);
    expect_accepted_then_rejected("cmd-rtl");

    auto go = make_command("cmd-goto");
    auto* target = go.mutable_goto_()->mutable_target();
    target->set_latitude_deg(47.397742);
    target->set_longitude_deg(8.545594);
    executor_.enqueue(go);
    expect_accepted_then_rejected("cmd-goto");
}

TEST_F(CommandExecutorTest, StartMissionFailsWithoutConnection) {
    auto start = make_command("cmd-start-mission");
    start.mutable_start_mission();
    executor_.enqueue(start);
    ASSERT_TRUE(collector_.wait_terminal("cmd-start-mission"));
    const auto acks = collector_.for_command("cmd-start-mission");
    EXPECT_EQ(acks.back().status(), karshipta::v1::COMMAND_STATUS_REJECTED);
    EXPECT_FALSE(acks.back().message().empty());
}

// Not connected means flight_mode() is Unknown (never Mission), so this
// exercises the manual-hold branch, not Mission::pause_mission().
TEST_F(CommandExecutorTest, PauseMissionFallsBackToHoldOutsideMissionFlightModeAndFailsWithoutConnection) {
    auto pause = make_command("cmd-pause-mission");
    pause.mutable_pause_mission();
    executor_.enqueue(pause);
    ASSERT_TRUE(collector_.wait_terminal("cmd-pause-mission"));
    const auto acks = collector_.for_command("cmd-pause-mission");
    EXPECT_EQ(acks.back().status(), karshipta::v1::COMMAND_STATUS_REJECTED);
    EXPECT_FALSE(acks.back().message().empty());
}

TEST_F(CommandExecutorTest, CommandWithoutActionIsRejectedWithoutQueueing) {
    const auto empty = make_command("cmd-empty");
    executor_.enqueue(empty);
    ASSERT_TRUE(collector_.wait_terminal("cmd-empty"));
    const auto acks = collector_.for_command("cmd-empty");
    ASSERT_EQ(acks.size(), 1u);
    EXPECT_EQ(acks.front().status(), karshipta::v1::COMMAND_STATUS_REJECTED);
    EXPECT_EQ(acks.front().message(), "command has no action");
}

TEST_F(CommandExecutorTest, AckEchoesCommandAndVehicleIds) {
    auto arm = make_command("cmd-echo");
    arm.mutable_arm();
    executor_.enqueue(arm);
    ASSERT_TRUE(collector_.wait_terminal("cmd-echo"));
    for (const auto& ack : collector_.for_command("cmd-echo")) {
        EXPECT_EQ(ack.command_id(), "cmd-echo");
        EXPECT_EQ(ack.vehicle_id(), "sitl-1");
    }
}
