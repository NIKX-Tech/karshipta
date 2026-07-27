#include "fleet_mission_store.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

karshipta::v1::WardMissionPlan make_plan(const std::string& ward_id, const double lat) {
    karshipta::v1::WardMissionPlan plan;
    plan.set_ward_id(ward_id);
    auto* item = plan.add_items();
    item->set_seq(0);
    item->set_action(karshipta::v1::MISSION_ACTION_WAYPOINT);
    item->mutable_position()->set_latitude_deg(lat);
    item->mutable_position()->set_longitude_deg(1.0);
    return plan;
}

}  // namespace

class FleetMissionStoreTest : public ::testing::Test {
   protected:
    // ":memory:" is a fresh, private, in-process database per instance -
    // never touches disk, so tests stay hermetic without needing cleanup.
    FleetMissionStore store_{std::filesystem::path(":memory:")};
};

TEST_F(FleetMissionStoreTest, CreateFleetMissionReturnsIdAndPersistsDistinctPerWardPlans) {
    const auto fleet_mission_id =
        store_.create_fleet_mission("team-a", "Sweep", /*repeat_count=*/2,
                                     {make_plan("alpha-1", 1.0), make_plan("alpha-2", 2.0)});

    const auto mission = store_.get_fleet_mission(fleet_mission_id);
    ASSERT_TRUE(mission.has_value());
    EXPECT_EQ(mission->fleet_id(), "team-a");
    EXPECT_EQ(mission->mission_name(), "Sweep");
    EXPECT_EQ(mission->repeat_count(), 2u);
    EXPECT_EQ(mission->status(), karshipta::v1::FLEET_MISSION_STATUS_ACTIVE);

    // The actual fix this store exists for: each ward's own plan is stored
    // and read back independently, never merged into one shared route.
    ASSERT_EQ(mission->ward_plans_size(), 2);
    EXPECT_EQ(mission->ward_plans(0).ward_id(), "alpha-1");
    EXPECT_DOUBLE_EQ(mission->ward_plans(0).items(0).position().latitude_deg(), 1.0);
    EXPECT_EQ(mission->ward_plans(1).ward_id(), "alpha-2");
    EXPECT_DOUBLE_EQ(mission->ward_plans(1).items(0).position().latitude_deg(), 2.0);

    // Every ward starts with an UNSPECIFIED state row - the caller
    // (FleetManager) sets the real per-ward status once dispatch is known.
    ASSERT_EQ(mission->ward_states_size(), 2);
    for (const auto& state : mission->ward_states()) {
        EXPECT_EQ(state.status(), karshipta::v1::WARD_MISSION_STATUS_UNSPECIFIED);
    }
}

TEST_F(FleetMissionStoreTest, GetFleetMissionReturnsNulloptForUnknownId) {
    EXPECT_FALSE(store_.get_fleet_mission("ghost").has_value());
}

TEST_F(FleetMissionStoreTest, ListFleetMissionsOrdersByCreatedAt) {
    const auto first_id = store_.create_fleet_mission("", "First", 0, {make_plan("alpha-1", 1.0)});
    const auto second_id = store_.create_fleet_mission("", "Second", 0, {make_plan("alpha-2", 2.0)});

    const auto missions = store_.list_fleet_missions();
    ASSERT_EQ(missions.size(), 2u);
    EXPECT_EQ(missions[0].fleet_mission_id(), first_id);
    EXPECT_EQ(missions[1].fleet_mission_id(), second_id);
}

TEST_F(FleetMissionStoreTest, UpdateWardStateChangesOnlyThatWardAndRejectsUnknownWard) {
    const auto fleet_mission_id = store_.create_fleet_mission(
        "", "Sweep", 0, {make_plan("alpha-1", 1.0), make_plan("alpha-2", 2.0)});

    karshipta::v1::WardMissionState state;
    state.set_ward_id("alpha-1");
    state.set_status(karshipta::v1::WARD_MISSION_STATUS_ACTIVE);
    state.set_mission_id("mission-alpha-1");
    EXPECT_FALSE(store_.update_ward_state(fleet_mission_id, state).has_value());

    const auto mission = store_.get_fleet_mission(fleet_mission_id);
    ASSERT_TRUE(mission.has_value());
    EXPECT_EQ(mission->ward_states(0).status(), karshipta::v1::WARD_MISSION_STATUS_ACTIVE);
    EXPECT_EQ(mission->ward_states(0).mission_id(), "mission-alpha-1");
    // alpha-2 untouched.
    EXPECT_EQ(mission->ward_states(1).status(), karshipta::v1::WARD_MISSION_STATUS_UNSPECIFIED);

    karshipta::v1::WardMissionState unknown_ward;
    unknown_ward.set_ward_id("ghost");
    unknown_ward.set_status(karshipta::v1::WARD_MISSION_STATUS_ACTIVE);
    const auto error = store_.update_ward_state(fleet_mission_id, unknown_ward);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("ghost"), std::string::npos) << *error;
}

TEST_F(FleetMissionStoreTest, SetStatusChangesAggregateAndRejectsUnknownId) {
    const auto fleet_mission_id =
        store_.create_fleet_mission("", "Sweep", 0, {make_plan("alpha-1", 1.0)});
    EXPECT_FALSE(
        store_.set_status(fleet_mission_id, karshipta::v1::FLEET_MISSION_STATUS_STOPPING).has_value());
    EXPECT_EQ(store_.get_fleet_mission(fleet_mission_id)->status(),
              karshipta::v1::FLEET_MISSION_STATUS_STOPPING);

    const auto error = store_.set_status("ghost", karshipta::v1::FLEET_MISSION_STATUS_STOPPED);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("ghost"), std::string::npos) << *error;
}

TEST_F(FleetMissionStoreTest, UpdateWardPlansReplacesRoutesAndResetsStatesAndAggregateStatus) {
    const auto fleet_mission_id =
        store_.create_fleet_mission("", "Sweep", 1, {make_plan("alpha-1", 1.0)});
    ASSERT_FALSE(store_.set_status(fleet_mission_id, karshipta::v1::FLEET_MISSION_STATUS_STOPPED)
                     .has_value());
    karshipta::v1::WardMissionState stopped;
    stopped.set_ward_id("alpha-1");
    stopped.set_status(karshipta::v1::WARD_MISSION_STATUS_STOPPED);
    ASSERT_FALSE(store_.update_ward_state(fleet_mission_id, stopped).has_value());

    // Edit: a completely different ward set and route (alpha-1 dropped,
    // alpha-3 added) - proves this replaces, not merges.
    const auto error =
        store_.update_ward_plans(fleet_mission_id, "Sweep v2", 3, {make_plan("alpha-3", 9.0)});
    EXPECT_FALSE(error.has_value());

    const auto mission = store_.get_fleet_mission(fleet_mission_id);
    ASSERT_TRUE(mission.has_value());
    EXPECT_EQ(mission->mission_name(), "Sweep v2");
    EXPECT_EQ(mission->repeat_count(), 3u);
    EXPECT_EQ(mission->status(), karshipta::v1::FLEET_MISSION_STATUS_ACTIVE);  // reset from STOPPED
    ASSERT_EQ(mission->ward_plans_size(), 1);
    EXPECT_EQ(mission->ward_plans(0).ward_id(), "alpha-3");
    ASSERT_EQ(mission->ward_states_size(), 1);
    EXPECT_EQ(mission->ward_states(0).ward_id(), "alpha-3");
    EXPECT_EQ(mission->ward_states(0).status(), karshipta::v1::WARD_MISSION_STATUS_UNSPECIFIED);
}

TEST_F(FleetMissionStoreTest, UpdateWardPlansRejectsUnknownId) {
    const auto error = store_.update_ward_plans("ghost", "Name", 0, {make_plan("alpha-1", 1.0)});
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("ghost"), std::string::npos) << *error;
}

TEST_F(FleetMissionStoreTest, DeleteFleetMissionRemovesItAndCascadesPlansAndStates) {
    const auto fleet_mission_id =
        store_.create_fleet_mission("", "Sweep", 0, {make_plan("alpha-1", 1.0)});
    EXPECT_FALSE(store_.delete_fleet_mission(fleet_mission_id).has_value());
    EXPECT_FALSE(store_.get_fleet_mission(fleet_mission_id).has_value());
    EXPECT_TRUE(store_.list_fleet_missions().empty());

    const auto error = store_.delete_fleet_mission(fleet_mission_id);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find(fleet_mission_id), std::string::npos) << *error;
}

// ---------- Persistence across instances ----------

TEST(FleetMissionStorePersistenceTest, PersistsOnMutateAndRoundTripsOnReload) {
    const auto path =
        std::filesystem::temp_directory_path() / "karshipta_fleet_mission_store_test.db";
    std::filesystem::remove(path);

    std::string fleet_mission_id;
    {
        FleetMissionStore store(path);
        fleet_mission_id = store.create_fleet_mission("team-a", "Sweep", 1, {make_plan("alpha-1", 1.0)});
        karshipta::v1::WardMissionState state;
        state.set_ward_id("alpha-1");
        state.set_status(karshipta::v1::WARD_MISSION_STATUS_ACTIVE);
        state.set_mission_id("mission-alpha-1");
        ASSERT_FALSE(store.update_ward_state(fleet_mission_id, state).has_value());
    }  // store destructs here, closing the connection

    {
        FleetMissionStore reloaded(path);
        const auto mission = reloaded.get_fleet_mission(fleet_mission_id);
        ASSERT_TRUE(mission.has_value());
        EXPECT_EQ(mission->fleet_id(), "team-a");
        ASSERT_EQ(mission->ward_plans_size(), 1);
        EXPECT_EQ(mission->ward_plans(0).ward_id(), "alpha-1");
        ASSERT_EQ(mission->ward_states_size(), 1);
        EXPECT_EQ(mission->ward_states(0).status(), karshipta::v1::WARD_MISSION_STATUS_ACTIVE);
        EXPECT_EQ(mission->ward_states(0).mission_id(), "mission-alpha-1");
    }

    std::filesystem::remove(path);
}

// Same rationale as fleet_zone_store_test.cpp's matching test: a read-only
// containing directory forces every write SQLite attempts to fail for real,
// proving create_fleet_mission()'s mission-row-plus-plans-plus-states insert
// is genuinely all-or-nothing (the Transaction wrapping it), not left
// half-committed on a mid-loop failure.
TEST(FleetMissionStorePersistenceTest, CreateFleetMissionOnReadOnlyDirectoryThrowsWithoutPartialWrite) {
    const auto dir =
        std::filesystem::temp_directory_path() / "karshipta_fleet_mission_store_readonly_test_dir";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directory(dir);
    const auto path = dir / "store.db";
    FleetMissionStore store(path);  // opened while the directory is still writable

    std::filesystem::permissions(dir, std::filesystem::perms::owner_write,
                                  std::filesystem::perm_options::remove);

    EXPECT_THROW(store.create_fleet_mission("", "Sweep", 0, {make_plan("alpha-1", 1.0)}),
                 std::exception);
    EXPECT_TRUE(store.list_fleet_missions().empty());

    std::filesystem::permissions(dir, std::filesystem::perms::owner_write,
                                  std::filesystem::perm_options::add);
    std::filesystem::remove_all(dir);
}
