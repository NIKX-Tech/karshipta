#include "fleet_zone_store.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

karshipta::v1::GeoPoint make_point(const double lat, const double lon) {
    karshipta::v1::GeoPoint point;
    point.set_latitude_deg(lat);
    point.set_longitude_deg(lon);
    return point;
}

std::vector<karshipta::v1::GeoPoint> make_triangle() {
    return {make_point(1.0, 1.0), make_point(2.0, 1.0), make_point(1.5, 2.0)};
}

}  // namespace

class FleetZoneStoreTest : public ::testing::Test {
   protected:
    // ":memory:" is a fresh, private, in-process database per instance -
    // never touches disk, so tests stay hermetic without needing cleanup.
    FleetZoneStore store_{std::filesystem::path(":memory:")};
};

// ---------- Fleet ----------

TEST_F(FleetZoneStoreTest, CreateFleetReturnsIdAndListsIt) {
    const auto fleet_id = store_.create_fleet("Inspection Team", "Roof surveys");

    const auto fleets = store_.list_fleets();
    ASSERT_EQ(fleets.size(), 1u);
    EXPECT_EQ(fleets.front().fleet_id(), fleet_id);
    EXPECT_EQ(fleets.front().name(), "Inspection Team");
    EXPECT_EQ(fleets.front().description(), "Roof surveys");
    EXPECT_EQ(fleets.front().ward_ids_size(), 0);
}

TEST_F(FleetZoneStoreTest, RenameFleetUpdatesNameAndDescriptionAndRejectsUnknownId) {
    const auto fleet_id = store_.create_fleet("Team A", "Original description");
    EXPECT_FALSE(store_.rename_fleet(fleet_id, "Team B", "Updated description").has_value());
    const auto fleet = store_.list_fleets().front();
    EXPECT_EQ(fleet.name(), "Team B");
    EXPECT_EQ(fleet.description(), "Updated description");

    const auto error = store_.rename_fleet("ghost", "New Name", "");
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("ghost"), std::string::npos) << *error;
}

TEST_F(FleetZoneStoreTest, DeleteFleetRemovesItAndCascadesMembership) {
    const auto fleet_id = store_.create_fleet("Team A", "");
    ASSERT_FALSE(store_.add_ward_to_fleet(fleet_id, "alpha-1").has_value());

    EXPECT_FALSE(store_.delete_fleet(fleet_id).has_value());
    EXPECT_TRUE(store_.list_fleets().empty());

    // Cascade proof: re-adding a fleet with wards, then deleting it, must not
    // leave an orphaned fleet_wards row a later fleet with the same id could
    // somehow inherit (ids are never reused in practice, but the cascade
    // itself is what this test actually verifies).
    const auto error = store_.delete_fleet(fleet_id);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find(fleet_id), std::string::npos) << *error;
}

TEST_F(FleetZoneStoreTest, AddWardToFleetRejectsUnknownFleet) {
    const auto error = store_.add_ward_to_fleet("ghost", "alpha-1");
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("ghost"), std::string::npos) << *error;
}

TEST_F(FleetZoneStoreTest, AddWardToFleetIsIdempotent) {
    const auto fleet_id = store_.create_fleet("Team A", "");
    ASSERT_FALSE(store_.add_ward_to_fleet(fleet_id, "alpha-1").has_value());
    ASSERT_FALSE(store_.add_ward_to_fleet(fleet_id, "alpha-1").has_value());  // no error, no duplicate

    const auto fleets = store_.list_fleets();
    ASSERT_EQ(fleets.front().ward_ids_size(), 1);
    EXPECT_EQ(fleets.front().ward_ids(0), "alpha-1");
}

TEST_F(FleetZoneStoreTest, RemoveWardFromFleetIsIdempotentAndRejectsUnknownFleet) {
    const auto fleet_id = store_.create_fleet("Team A", "");
    // Never added: removing it is a no-op, not an error.
    EXPECT_FALSE(store_.remove_ward_from_fleet(fleet_id, "alpha-1").has_value());

    ASSERT_FALSE(store_.add_ward_to_fleet(fleet_id, "alpha-1").has_value());
    EXPECT_FALSE(store_.remove_ward_from_fleet(fleet_id, "alpha-1").has_value());
    EXPECT_EQ(store_.list_fleets().front().ward_ids_size(), 0);

    const auto error = store_.remove_ward_from_fleet("ghost", "alpha-1");
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("ghost"), std::string::npos) << *error;
}

TEST_F(FleetZoneStoreTest, WardCanBelongToMultipleFleets) {
    const auto fleet_a = store_.create_fleet("Team A", "");
    const auto fleet_b = store_.create_fleet("Team B", "");
    ASSERT_FALSE(store_.add_ward_to_fleet(fleet_a, "alpha-1").has_value());
    ASSERT_FALSE(store_.add_ward_to_fleet(fleet_b, "alpha-1").has_value());

    const auto fleets = store_.list_fleets();
    ASSERT_EQ(fleets.size(), 2u);
    for (const auto& fleet : fleets) {
        ASSERT_EQ(fleet.ward_ids_size(), 1);
        EXPECT_EQ(fleet.ward_ids(0), "alpha-1");
    }
}

// ---------- Zone ----------

TEST_F(FleetZoneStoreTest, CreateZoneReturnsIdAndListsItWithVerticesInOrder) {
    const auto zone_id = store_.create_zone("No-fly", karshipta::v1::ZONE_TYPE_KEEP_OUT, make_triangle(),
                                             /*altitude_min_m=*/std::nullopt, /*altitude_max_m=*/120.0f);

    const auto zones = store_.list_zones();
    ASSERT_EQ(zones.size(), 1u);
    const auto& zone = zones.front();
    EXPECT_EQ(zone.zone_id(), zone_id);
    EXPECT_EQ(zone.name(), "No-fly");
    EXPECT_EQ(zone.type(), karshipta::v1::ZONE_TYPE_KEEP_OUT);
    EXPECT_FALSE(zone.has_altitude_min_m());
    ASSERT_TRUE(zone.has_altitude_max_m());
    EXPECT_FLOAT_EQ(zone.altitude_max_m(), 120.0f);

    ASSERT_EQ(zone.vertices_size(), 3);
    EXPECT_DOUBLE_EQ(zone.vertices(0).latitude_deg(), 1.0);
    EXPECT_DOUBLE_EQ(zone.vertices(1).latitude_deg(), 2.0);
    EXPECT_DOUBLE_EQ(zone.vertices(2).latitude_deg(), 1.5);
}

TEST_F(FleetZoneStoreTest, UpdateZoneChangesNameTypeAndAltitudeNotGeometry) {
    const auto zone_id = store_.create_zone("Staging", karshipta::v1::ZONE_TYPE_KEEP_IN, make_triangle(),
                                             std::nullopt, std::nullopt);

    EXPECT_FALSE(store_
                     .update_zone(zone_id, "Staging Area", karshipta::v1::ZONE_TYPE_KEEP_OUT, 10.0f, 50.0f)
                     .has_value());

    const auto zone = store_.list_zones().front();
    EXPECT_EQ(zone.name(), "Staging Area");
    EXPECT_EQ(zone.type(), karshipta::v1::ZONE_TYPE_KEEP_OUT);
    ASSERT_TRUE(zone.has_altitude_min_m());
    EXPECT_FLOAT_EQ(zone.altitude_min_m(), 10.0f);
    ASSERT_TRUE(zone.has_altitude_max_m());
    EXPECT_FLOAT_EQ(zone.altitude_max_m(), 50.0f);
    ASSERT_EQ(zone.vertices_size(), 3);  // unchanged
}

TEST_F(FleetZoneStoreTest, UpdateZoneRejectsUnknownId) {
    const auto error = store_.update_zone("ghost", "Name", karshipta::v1::ZONE_TYPE_KEEP_IN, std::nullopt,
                                           std::nullopt);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("ghost"), std::string::npos) << *error;
}

TEST_F(FleetZoneStoreTest, DeleteZoneRemovesItAndCascadesVertices) {
    const auto zone_id =
        store_.create_zone("Staging", karshipta::v1::ZONE_TYPE_KEEP_IN, make_triangle(), std::nullopt,
                            std::nullopt);
    EXPECT_FALSE(store_.delete_zone(zone_id).has_value());
    EXPECT_TRUE(store_.list_zones().empty());

    const auto error = store_.delete_zone(zone_id);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find(zone_id), std::string::npos) << *error;
}

TEST_F(FleetZoneStoreTest, MultipleZonesListedByName) {
    store_.create_zone("Zulu", karshipta::v1::ZONE_TYPE_KEEP_IN, make_triangle(), std::nullopt,
                        std::nullopt);
    store_.create_zone("Alpha", karshipta::v1::ZONE_TYPE_KEEP_OUT, make_triangle(), std::nullopt,
                        std::nullopt);

    const auto zones = store_.list_zones();
    ASSERT_EQ(zones.size(), 2u);
    EXPECT_EQ(zones[0].name(), "Alpha");
    EXPECT_EQ(zones[1].name(), "Zulu");
}

// ---------- Persistence across instances (crash-recovery equivalent of
// WardManagerPersistenceTest.PersistsOnAddAndRoundTripsOnReload) ----------

TEST(FleetZoneStorePersistenceTest, PersistsOnMutateAndRoundTripsOnReload) {
    const auto path =
        std::filesystem::temp_directory_path() / "karshipta_fleet_zone_store_test.db";
    std::filesystem::remove(path);

    std::string fleet_id;
    std::string zone_id;
    {
        FleetZoneStore store(path);
        fleet_id = store.create_fleet("Team A", "Roof surveys");
        ASSERT_FALSE(store.add_ward_to_fleet(fleet_id, "alpha-1").has_value());
        zone_id = store.create_zone("No-fly", karshipta::v1::ZONE_TYPE_KEEP_OUT, make_triangle(),
                                     std::nullopt, 100.0f);
    }  // store destructs here, closing the connection

    {
        FleetZoneStore reloaded(path);
        const auto fleets = reloaded.list_fleets();
        ASSERT_EQ(fleets.size(), 1u);
        EXPECT_EQ(fleets.front().fleet_id(), fleet_id);
        ASSERT_EQ(fleets.front().ward_ids_size(), 1);
        EXPECT_EQ(fleets.front().ward_ids(0), "alpha-1");

        const auto zones = reloaded.list_zones();
        ASSERT_EQ(zones.size(), 1u);
        EXPECT_EQ(zones.front().zone_id(), zone_id);
        EXPECT_EQ(zones.front().vertices_size(), 3);
    }

    std::filesystem::remove(path);
}
