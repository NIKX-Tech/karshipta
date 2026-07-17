#include "mission_importer.h"

#include <gtest/gtest.h>

#include "vehicle_connection.h"

// MissionImporter against a never-connected vehicle: format validation
// rejects before ever touching MAVSDK, and the MAVSDK-backed path (ensure_
// mission_raw()'s connectivity check) fails fast with a readable reason
// rather than hang. Actual parsing of a real QGC .plan / Mission Planner WPL
// string against a live MissionRaw plugin is not exercised here (no SITL
// harness in this repo); see gateway/docs/mission-importer.md's "Automated
// tests" section.

namespace {

class MissionImporterTest : public ::testing::Test {
protected:
    MissionImporterTest()
        : core_(VehicleConnection::create_shared_core()),
          vehicle_(core_, "udpin://127.0.0.1:24998"),
          importer_(vehicle_) {}

    karshipta::v1::MissionFileUpload make_upload(const karshipta::v1::MissionFileFormat format) {
        karshipta::v1::MissionFileUpload upload;
        upload.set_vehicle_id("sitl-1");
        upload.set_format(format);
        upload.set_raw_content("{}");
        return upload;
    }

    std::shared_ptr<mavsdk::Mavsdk> core_;
    VehicleConnection vehicle_;
    MissionImporter importer_;
};

}  // namespace

TEST_F(MissionImporterTest, RejectsUnspecifiedFormatWithoutTouchingMavsdk) {
    const auto upload = make_upload(karshipta::v1::MISSION_FILE_FORMAT_UNSPECIFIED);
    const auto [mission, reason] = importer_.import(upload);
    EXPECT_FALSE(mission.has_value());
    EXPECT_EQ(reason, "mission file format unspecified");
}

TEST_F(MissionImporterTest, QgcPlanFailsWithoutConnection) {
    const auto upload = make_upload(karshipta::v1::MISSION_FILE_FORMAT_QGC_PLAN);
    const auto [mission, reason] = importer_.import(upload);
    EXPECT_FALSE(mission.has_value());
    EXPECT_FALSE(reason.empty());
}

TEST_F(MissionImporterTest, MissionPlannerWplFailsWithoutConnection) {
    const auto upload = make_upload(karshipta::v1::MISSION_FILE_FORMAT_MISSION_PLANNER_WPL);
    const auto [mission, reason] = importer_.import(upload);
    EXPECT_FALSE(mission.has_value());
    EXPECT_FALSE(reason.empty());
}

TEST_F(MissionImporterTest, ResultNameIsNonEmpty) {
    EXPECT_FALSE(MissionImporter::result_name(mavsdk::MissionRaw::Result::Success).empty());
    EXPECT_FALSE(
        MissionImporter::result_name(mavsdk::MissionRaw::Result::FailedToParseQgcPlan).empty());
}
