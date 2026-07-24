#include "herald_field_mapper.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path write_temp_yaml(const std::string& name, const std::string& content) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::trunc);
    out << content;
    out.close();
    return path;
}

constexpr auto kValidConfig = R"(
source_name: test-vendor
entity_class: ENTITY_CLASS_GENERIC_TRACKER
timestamp_unit: unix_ms
fields:
  entity_id: device_id
  timestamp_ms: timestamp
  latitude_deg: location.lat
  longitude_deg: location.lon
  altitude_msl_m: location.alt
  battery_voltage_v: battery.voltage
  battery_remaining_pct: battery.percent
  num_satellites: gps.satellites
  health_ok: status.ok
)";

// ---------- load_mapping ----------

TEST(HeraldFieldMapper, LoadMappingParsesValidConfig) {
    const auto path = write_temp_yaml("karshipta_herald_mapping_valid.yaml", kValidConfig);
    const auto mapping = HeraldFieldMapper::load_mapping(path.string());
    ASSERT_TRUE(mapping.has_value());
    EXPECT_EQ(mapping->source_name, "test-vendor");
    EXPECT_EQ(mapping->entity_class, herald::v0::ENTITY_CLASS_GENERIC_TRACKER);
    EXPECT_EQ(mapping->timestamp_unit, "unix_ms");
    EXPECT_EQ(mapping->fields.at("entity_id"), "device_id");
    EXPECT_EQ(mapping->fields.at("latitude_deg"), "location.lat");
}

TEST(HeraldFieldMapper, LoadMappingDefaultsTimestampUnitToUnixMs) {
    constexpr auto config = R"(
source_name: no-unit-specified
fields:
  entity_id: id
  timestamp_ms: ts
  latitude_deg: lat
  longitude_deg: lon
)";
    const auto path = write_temp_yaml("karshipta_herald_mapping_default_unit.yaml", config);
    const auto mapping = HeraldFieldMapper::load_mapping(path.string());
    ASSERT_TRUE(mapping.has_value());
    EXPECT_EQ(mapping->timestamp_unit, "unix_ms");
}

TEST(HeraldFieldMapper, LoadMappingRejectsMissingFile) {
    const auto mapping = HeraldFieldMapper::load_mapping("/nonexistent/path/does-not-exist.yaml");
    EXPECT_FALSE(mapping.has_value());
}

TEST(HeraldFieldMapper, LoadMappingRejectsMissingSourceName) {
    constexpr auto config = R"(
fields:
  entity_id: device_id
  timestamp_ms: timestamp
  latitude_deg: location.lat
  longitude_deg: location.lon
)";
    const auto path = write_temp_yaml("karshipta_herald_mapping_no_source_name.yaml", config);
    const auto mapping = HeraldFieldMapper::load_mapping(path.string());
    EXPECT_FALSE(mapping.has_value());
}

TEST(HeraldFieldMapper, LoadMappingRejectsMissingRequiredFieldMapping) {
    // No latitude_deg/longitude_deg entries at all.
    constexpr auto config = R"(
source_name: incomplete-vendor
fields:
  entity_id: device_id
  timestamp_ms: timestamp
)";
    const auto path = write_temp_yaml("karshipta_herald_mapping_incomplete.yaml", config);
    const auto mapping = HeraldFieldMapper::load_mapping(path.string());
    EXPECT_FALSE(mapping.has_value());
}

TEST(HeraldFieldMapper, LoadMappingRejectsInvalidTimestampUnit) {
    constexpr auto config = R"(
source_name: bad-unit-vendor
timestamp_unit: furlongs_per_fortnight
fields:
  entity_id: device_id
  timestamp_ms: timestamp
  latitude_deg: location.lat
  longitude_deg: location.lon
)";
    const auto path = write_temp_yaml("karshipta_herald_mapping_bad_unit.yaml", config);
    const auto mapping = HeraldFieldMapper::load_mapping(path.string());
    EXPECT_FALSE(mapping.has_value());
}

TEST(HeraldFieldMapper, LoadMappingRejectsInvalidEntityClass) {
    constexpr auto config = R"(
source_name: bad-class-vendor
entity_class: ENTITY_CLASS_FLYING_SAUCER
fields:
  entity_id: device_id
  timestamp_ms: timestamp
  latitude_deg: location.lat
  longitude_deg: location.lon
)";
    const auto path = write_temp_yaml("karshipta_herald_mapping_bad_class.yaml", config);
    const auto mapping = HeraldFieldMapper::load_mapping(path.string());
    EXPECT_FALSE(mapping.has_value());
}

// ---------- apply ----------

class HeraldFieldMapperApplyTest : public ::testing::Test {
   protected:
    HeraldFieldMapping mapping_ = *HeraldFieldMapper::load_mapping(
        write_temp_yaml("karshipta_herald_mapping_apply_test.yaml", kValidConfig).string());
};

TEST_F(HeraldFieldMapperApplyTest, BuildsHeraldMessageFromValidPayload) {
    const auto payload = nlohmann::json::parse(R"({
        "device_id": "tracker-42",
        "timestamp": 1710505845000,
        "location": {"lat": 52.37, "lon": 4.90, "alt": 12.5},
        "battery": {"voltage": 3.98, "percent": 76},
        "gps": {"satellites": 9},
        "status": {"ok": true}
    })");
    const auto msg = HeraldFieldMapper::apply(mapping_, payload);
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->entity_id(), "tracker-42");
    EXPECT_EQ(msg->timestamp_ms(), 1710505845000ULL);
    EXPECT_EQ(msg->entity_class(), herald::v0::ENTITY_CLASS_GENERIC_TRACKER);
    EXPECT_DOUBLE_EQ(msg->position().latitude_deg(), 52.37);
    EXPECT_DOUBLE_EQ(msg->position().longitude_deg(), 4.90);
    EXPECT_FLOAT_EQ(msg->position().altitude_msl_m(), 12.5F);
    ASSERT_TRUE(msg->has_battery());
    EXPECT_FLOAT_EQ(msg->battery().voltage_v(), 3.98F);
    EXPECT_FLOAT_EQ(msg->battery().remaining_pct(), 76.0F);
    ASSERT_TRUE(msg->has_gps());
    EXPECT_EQ(msg->gps().num_satellites(), 9U);
    EXPECT_TRUE(msg->health_ok());
    EXPECT_EQ(msg->source(), "test-vendor");
}

TEST_F(HeraldFieldMapperApplyTest, ConvertsUnixSecondsToMilliseconds) {
    HeraldFieldMapping seconds_mapping = mapping_;
    seconds_mapping.timestamp_unit = "unix_seconds";
    const auto payload = nlohmann::json::parse(R"({
        "device_id": "x", "timestamp": 1710505845,
        "location": {"lat": 1.0, "lon": 2.0}
    })");
    const auto msg = HeraldFieldMapper::apply(seconds_mapping, payload);
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->timestamp_ms(), 1710505845000ULL);
}

TEST_F(HeraldFieldMapperApplyTest, RejectsPayloadMissingRequiredField) {
    const auto payload = nlohmann::json::parse(R"({"device_id": "x", "timestamp": 1})");
    EXPECT_FALSE(HeraldFieldMapper::apply(mapping_, payload).has_value());
}

TEST_F(HeraldFieldMapperApplyTest, RejectsWrongTypeForRequiredNumericField) {
    const auto payload = nlohmann::json::parse(R"({
        "device_id": "x", "timestamp": 1,
        "location": {"lat": "not-a-number", "lon": 2.0}
    })");
    EXPECT_FALSE(HeraldFieldMapper::apply(mapping_, payload).has_value());
}

TEST_F(HeraldFieldMapperApplyTest, AcceptsNumericStringForRequiredNumericField) {
    // Some vendors send coordinates as strings - accepted, not rejected.
    const auto payload = nlohmann::json::parse(R"({
        "device_id": "x", "timestamp": 1,
        "location": {"lat": "52.37", "lon": "4.90"}
    })");
    const auto msg = HeraldFieldMapper::apply(mapping_, payload);
    ASSERT_TRUE(msg.has_value());
    EXPECT_DOUBLE_EQ(msg->position().latitude_deg(), 52.37);
}

TEST_F(HeraldFieldMapperApplyTest, AcceptsNumericEntityIdAsString) {
    const auto payload = nlohmann::json::parse(R"({
        "device_id": 123456, "timestamp": 1,
        "location": {"lat": 1.0, "lon": 2.0}
    })");
    const auto msg = HeraldFieldMapper::apply(mapping_, payload);
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->entity_id(), "123456");
}

TEST_F(HeraldFieldMapperApplyTest, LeavesOptionalFieldsUnsetWhenAbsentFromPayload) {
    const auto payload = nlohmann::json::parse(R"({
        "device_id": "x", "timestamp": 1,
        "location": {"lat": 1.0, "lon": 2.0}
    })");
    const auto msg = HeraldFieldMapper::apply(mapping_, payload);
    ASSERT_TRUE(msg.has_value());
    EXPECT_FALSE(msg->has_battery());
    EXPECT_FALSE(msg->has_gps());
}

TEST_F(HeraldFieldMapperApplyTest, DefaultsHealthOkTrueWhenNotMapped) {
    HeraldFieldMapping mapping_without_health = mapping_;
    mapping_without_health.fields.erase("health_ok");
    const auto payload = nlohmann::json::parse(R"({
        "device_id": "x", "timestamp": 1,
        "location": {"lat": 1.0, "lon": 2.0}
    })");
    const auto msg = HeraldFieldMapper::apply(mapping_without_health, payload);
    ASSERT_TRUE(msg.has_value());
    EXPECT_TRUE(msg->health_ok());
}

}  // namespace
