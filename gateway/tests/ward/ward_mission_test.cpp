#include "ward_mission.h"

#include <chrono>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "ward_connection.h"

// WardMission against a never-connected ward: validation failures (bad
// mission shape) must reject fast without ever touching MAVSDK, and every
// MAVSDK-backed path (ensure_mission()'s connectivity check, start(), pause())
// must fail fast with a readable Result rather than hang. This mirrors
// CommandExecutorTest's strategy for WardActions: a real but deliberately
// unconnected WardConnection. Actual upload/progress/repeat-loop behavior
// against a live MAVLink mission handshake is not exercised here (no
// MAVSDK server-plugin or SITL harness exists in this repo yet); see
// gateway/docs/ward-mission.md's "Automated tests" section.

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
    mission.set_ward_id("sitl-1");
    *mission.add_items() = make_waypoint(0, karshipta::v1::MISSION_ACTION_WAYPOINT);
    *mission.add_items() = make_waypoint(1, karshipta::v1::MISSION_ACTION_WAYPOINT);
    return mission;
}

// Polls take_upload_result() with a bounded retry loop: unlike CommandExecutor,
// WardMission has no ack callback to synchronize on.
std::optional<WardMission::UploadResult> wait_for_upload_result(
    WardMission& mission, const std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = mission.take_upload_result()) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::nullopt;
}

std::optional<WardMission::DownloadResult> wait_for_download_result(
    WardMission& mission, const std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = mission.take_download_result()) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::nullopt;
}

class WardMissionTest : public ::testing::Test {
protected:
    WardMissionTest()
        : core_(WardConnection::create_shared_core()),
          ward_(core_, "udpin://127.0.0.1:24997"),
          mission_(ward_) {}

    std::shared_ptr<mavsdk::Mavsdk> core_;
    WardConnection ward_;
    WardMission mission_;
};

}  // namespace

TEST_F(WardMissionTest, RejectsMissionWithEmptyWardId) {
    auto mission = make_mission("mission-empty-ward-id");
    mission.clear_ward_id();
    mission_.enqueue_upload(mission);

    const auto result = wait_for_upload_result(mission_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->mission_id, "mission-empty-ward-id");
    EXPECT_EQ(result->result, mavsdk::Mission::Result::InvalidArgument);
}

TEST_F(WardMissionTest, RejectsMissionWithNoItems) {
    karshipta::v1::Mission mission;
    mission.set_mission_id("mission-no-items");
    mission.set_ward_id("sitl-1");
    mission_.enqueue_upload(mission);

    const auto result = wait_for_upload_result(mission_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->result, mavsdk::Mission::Result::InvalidArgument);
}

TEST_F(WardMissionTest, RejectsMissionWithRtlNotLast) {
    auto mission = make_mission("mission-rtl-not-last");
    *mission.add_items() = make_waypoint(2, karshipta::v1::MISSION_ACTION_RTL);
    *mission.add_items() = make_waypoint(3, karshipta::v1::MISSION_ACTION_WAYPOINT);
    mission_.enqueue_upload(mission);

    const auto result = wait_for_upload_result(mission_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->result, mavsdk::Mission::Result::InvalidArgument);
}

TEST_F(WardMissionTest, AcceptsRtlAsLastItemButFailsWithoutConnection) {
    auto mission = make_mission("mission-rtl-last");
    *mission.add_items() = make_waypoint(2, karshipta::v1::MISSION_ACTION_RTL);
    mission_.enqueue_upload(mission);

    // Reaches ensure_mission() (validation passed) but there's no connection,
    // so it fails there rather than as InvalidArgument.
    const auto result = wait_for_upload_result(mission_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->result, mavsdk::Mission::Result::NoSystem);
}

TEST_F(WardMissionTest, DownloadFailsWithoutConnection) {
    mission_.enqueue_download();
    const auto result = wait_for_download_result(mission_);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->mission.has_value());
    EXPECT_FALSE(result->message.empty());
}

TEST_F(WardMissionTest, TakeDownloadResultIsNulloptBeforeAnyJobFinishes) {
    EXPECT_FALSE(mission_.take_download_result().has_value());
}

TEST_F(WardMissionTest, StartAndPauseFailWithoutConnection) {
    EXPECT_EQ(mission_.start(), mavsdk::Mission::Result::NoSystem);
    EXPECT_EQ(mission_.pause(), mavsdk::Mission::Result::NoSystem);
}

// ensure_mission() fails synchronously (never connected), so start_async()/
// pause_async() invoke the callback before returning, on the caller's own
// thread, same as any other never-connected WardMission path here.
TEST_F(WardMissionTest, StartAsyncAndPauseAsyncFailWithoutConnection) {
    std::optional<mavsdk::Mission::Result> start_result;
    mission_.start_async([&](const mavsdk::Mission::Result result) { start_result = result; });
    ASSERT_TRUE(start_result.has_value());
    EXPECT_EQ(*start_result, mavsdk::Mission::Result::NoSystem);

    std::optional<mavsdk::Mission::Result> pause_result;
    mission_.pause_async([&](const mavsdk::Mission::Result result) { pause_result = result; });
    ASSERT_TRUE(pause_result.has_value());
    EXPECT_EQ(*pause_result, mavsdk::Mission::Result::NoSystem);
}

TEST_F(WardMissionTest, NotifyInterruptedFalseWhenNothingUploaded) {
    EXPECT_FALSE(mission_.notify_interrupted());
}

TEST_F(WardMissionTest, GetProgressDefaultsBeforeAnyUpload) {
    const auto progress = mission_.get_progress();
    EXPECT_TRUE(progress.mission_id().empty());
    EXPECT_FALSE(progress.finished());
}

TEST_F(WardMissionTest, TakePendingReturnToLaunchDefaultsFalse) {
    EXPECT_FALSE(mission_.take_pending_return_to_launch());
    EXPECT_FALSE(mission_.take_pending_return_to_launch());
}

TEST_F(WardMissionTest, TakeUploadResultIsNulloptBeforeAnyJobFinishes) {
    EXPECT_FALSE(mission_.take_upload_result().has_value());
}

TEST_F(WardMissionTest, DestructorDoesNotHangWithQueuedUploads) {
    const auto start = std::chrono::steady_clock::now();
    {
        WardMission scoped(ward_);
        for (int i = 0; i < 5; ++i) {
            scoped.enqueue_upload(make_mission("mission-" + std::to_string(i)));
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::seconds(5))
        << "destructor should drop leftover uploads, not run them to completion";
}

TEST_F(WardMissionTest, ResultNameIsNonEmpty) {
    EXPECT_FALSE(WardMission::result_name(mavsdk::Mission::Result::Success).empty());
    EXPECT_FALSE(WardMission::result_name(mavsdk::Mission::Result::InvalidArgument).empty());
}
