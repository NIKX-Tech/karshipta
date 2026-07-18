#include "vehicle_mission.h"

#include <chrono>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "vehicle_connection.h"

// VehicleMission against a never-connected vehicle: validation failures (bad
// mission shape) must reject fast without ever touching MAVSDK, and every
// MAVSDK-backed path (ensure_mission()'s connectivity check, start(), pause())
// must fail fast with a readable Result rather than hang. This mirrors
// CommandExecutorTest's strategy for VehicleActions: a real but deliberately
// unconnected VehicleConnection. Actual upload/progress/repeat-loop behavior
// against a live MAVLink mission handshake is not exercised here (no
// MAVSDK server-plugin or SITL harness exists in this repo yet); see
// gateway/docs/vehicle-mission.md's "Automated tests" section.

namespace {

karshipta::v1::MissionItem make_waypoint(const uint32_t seq, const karshipta::v1::MissionAction action) {
    karshipta::v1::MissionItem item;
    item.set_seq(seq);
    item.set_action(action);
    auto* position = item.mutable_position();
    position->set_latitude_deg(47.397742);
    position->set_longitude_deg(8.545594);
    position->set_altitude_rel_m(20.0f);
    return item;
}

karshipta::v1::Mission make_mission(const std::string& mission_id) {
    karshipta::v1::Mission mission;
    mission.set_mission_id(mission_id);
    mission.set_vehicle_id("sitl-1");
    *mission.add_items() = make_waypoint(0, karshipta::v1::MISSION_ACTION_WAYPOINT);
    *mission.add_items() = make_waypoint(1, karshipta::v1::MISSION_ACTION_WAYPOINT);
    return mission;
}

// Polls take_upload_result() with a bounded retry loop: unlike CommandExecutor,
// VehicleMission has no ack callback to synchronize on.
std::optional<VehicleMission::UploadResult> wait_for_upload_result(
    VehicleMission& mission, const std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = mission.take_upload_result()) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::nullopt;
}

std::optional<VehicleMission::DownloadResult> wait_for_download_result(
    VehicleMission& mission, const std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = mission.take_download_result()) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::nullopt;
}

class VehicleMissionTest : public ::testing::Test {
protected:
    VehicleMissionTest()
        : core_(VehicleConnection::create_shared_core()),
          vehicle_(core_, "udpin://127.0.0.1:24997"),
          mission_(vehicle_) {}

    std::shared_ptr<mavsdk::Mavsdk> core_;
    VehicleConnection vehicle_;
    VehicleMission mission_;
};

}  // namespace

TEST_F(VehicleMissionTest, RejectsMissionWithEmptyVehicleId) {
    auto mission = make_mission("mission-empty-vehicle-id");
    mission.clear_vehicle_id();
    mission_.enqueue_upload(mission);

    const auto result = wait_for_upload_result(mission_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->mission_id, "mission-empty-vehicle-id");
    EXPECT_EQ(result->result, mavsdk::Mission::Result::InvalidArgument);
}

TEST_F(VehicleMissionTest, RejectsMissionWithNoItems) {
    karshipta::v1::Mission mission;
    mission.set_mission_id("mission-no-items");
    mission.set_vehicle_id("sitl-1");
    mission_.enqueue_upload(mission);

    const auto result = wait_for_upload_result(mission_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->result, mavsdk::Mission::Result::InvalidArgument);
}

TEST_F(VehicleMissionTest, RejectsMissionWithRtlNotLast) {
    auto mission = make_mission("mission-rtl-not-last");
    *mission.add_items() = make_waypoint(2, karshipta::v1::MISSION_ACTION_RTL);
    *mission.add_items() = make_waypoint(3, karshipta::v1::MISSION_ACTION_WAYPOINT);
    mission_.enqueue_upload(mission);

    const auto result = wait_for_upload_result(mission_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->result, mavsdk::Mission::Result::InvalidArgument);
}

TEST_F(VehicleMissionTest, AcceptsRtlAsLastItemButFailsWithoutConnection) {
    auto mission = make_mission("mission-rtl-last");
    *mission.add_items() = make_waypoint(2, karshipta::v1::MISSION_ACTION_RTL);
    mission_.enqueue_upload(mission);

    // Reaches ensure_mission() (validation passed) but there's no connection,
    // so it fails there rather than as InvalidArgument.
    const auto result = wait_for_upload_result(mission_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->result, mavsdk::Mission::Result::NoSystem);
}

TEST_F(VehicleMissionTest, DownloadFailsWithoutConnection) {
    mission_.enqueue_download();
    const auto result = wait_for_download_result(mission_);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->mission.has_value());
    EXPECT_FALSE(result->message.empty());
}

TEST_F(VehicleMissionTest, TakeDownloadResultIsNulloptBeforeAnyJobFinishes) {
    EXPECT_FALSE(mission_.take_download_result().has_value());
}

TEST_F(VehicleMissionTest, StartAndPauseFailWithoutConnection) {
    EXPECT_EQ(mission_.start(), mavsdk::Mission::Result::NoSystem);
    EXPECT_EQ(mission_.pause(), mavsdk::Mission::Result::NoSystem);
}

// ensure_mission() fails synchronously (never connected), so start_async()/
// pause_async() invoke the callback before returning, on the caller's own
// thread, same as any other never-connected VehicleMission path here.
TEST_F(VehicleMissionTest, StartAsyncAndPauseAsyncFailWithoutConnection) {
    std::optional<mavsdk::Mission::Result> start_result;
    mission_.start_async([&](const mavsdk::Mission::Result result) { start_result = result; });
    ASSERT_TRUE(start_result.has_value());
    EXPECT_EQ(*start_result, mavsdk::Mission::Result::NoSystem);

    std::optional<mavsdk::Mission::Result> pause_result;
    mission_.pause_async([&](const mavsdk::Mission::Result result) { pause_result = result; });
    ASSERT_TRUE(pause_result.has_value());
    EXPECT_EQ(*pause_result, mavsdk::Mission::Result::NoSystem);
}

TEST_F(VehicleMissionTest, NotifyInterruptedFalseWhenNothingUploaded) {
    EXPECT_FALSE(mission_.notify_interrupted());
}

TEST_F(VehicleMissionTest, GetProgressDefaultsBeforeAnyUpload) {
    const auto progress = mission_.get_progress();
    EXPECT_TRUE(progress.mission_id().empty());
    EXPECT_FALSE(progress.finished());
}

TEST_F(VehicleMissionTest, TakePendingReturnToLaunchDefaultsFalse) {
    EXPECT_FALSE(mission_.take_pending_return_to_launch());
    EXPECT_FALSE(mission_.take_pending_return_to_launch());
}

TEST_F(VehicleMissionTest, TakeUploadResultIsNulloptBeforeAnyJobFinishes) {
    EXPECT_FALSE(mission_.take_upload_result().has_value());
}

TEST_F(VehicleMissionTest, DestructorDoesNotHangWithQueuedUploads) {
    const auto start = std::chrono::steady_clock::now();
    {
        VehicleMission scoped(vehicle_);
        for (int i = 0; i < 5; ++i) {
            scoped.enqueue_upload(make_mission("mission-" + std::to_string(i)));
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds(5))
        << "destructor should drop leftover uploads, not run them to completion";
}

TEST_F(VehicleMissionTest, ResultNameIsNonEmpty) {
    EXPECT_FALSE(VehicleMission::result_name(mavsdk::Mission::Result::Success).empty());
    EXPECT_FALSE(VehicleMission::result_name(mavsdk::Mission::Result::InvalidArgument).empty());
}
