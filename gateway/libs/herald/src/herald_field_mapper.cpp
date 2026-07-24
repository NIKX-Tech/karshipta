#include "herald_field_mapper.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <array>
#include <sstream>
#include <vector>

namespace {

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> segments;
    std::stringstream stream(path);
    std::string segment;
    while (std::getline(stream, segment, '.')) {
        segments.push_back(segment);
    }
    return segments;
}

// Walks a dot-path ("location.lat") into a JSON object, one segment at a
// time. Returns nullopt (not a thrown exception) if any segment is absent
// or the value at some intermediate segment isn't an object to descend
// into - a malformed/partial vendor payload is an expected, observable
// condition here, not a crash.
std::optional<nlohmann::json> resolve_path(const nlohmann::json& payload, const std::string& path) {
    const nlohmann::json* current = &payload;
    for (const auto& segment : split_path(path)) {
        if (!current->is_object() || !current->contains(segment)) return std::nullopt;
        current = &(*current)[segment];
    }
    return *current;
}

std::optional<std::string> find_field_path(const HeraldFieldMapping& mapping,
                                            const std::string& herald_field) {
    const auto it = mapping.fields.find(herald_field);
    if (it == mapping.fields.end()) return std::nullopt;
    return it->second;
}

// Reads a resolved JSON value as a double regardless of whether the vendor
// sent it as a JSON number or a numeric string (both show up in real device
// payloads) - returns nullopt for anything else rather than guessing.
std::optional<double> as_number(const nlohmann::json& value) {
    if (value.is_number()) return value.get<double>();
    if (value.is_string()) {
        try {
            size_t consumed = 0;
            const double parsed = std::stod(value.get<std::string>(), &consumed);
            if (consumed == value.get<std::string>().size()) return parsed;
        } catch (const std::exception&) {
            // fall through to nullopt
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<HeraldFieldMapping> HeraldFieldMapper::load_mapping(const std::string& yaml_path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(yaml_path);
    } catch (const YAML::Exception& parse_error) {
        spdlog::error("failed to parse Herald mapping config '{}': {}", yaml_path,
                      parse_error.what());
        return std::nullopt;
    }

    if (!root["source_name"]) {
        spdlog::error("Herald mapping config '{}' has no source_name", yaml_path);
        return std::nullopt;
    }

    HeraldFieldMapping mapping;
    mapping.source_name = root["source_name"].as<std::string>();

    if (root["timestamp_unit"]) {
        mapping.timestamp_unit = root["timestamp_unit"].as<std::string>();
        if (mapping.timestamp_unit != "unix_ms" && mapping.timestamp_unit != "unix_seconds") {
            spdlog::error(
                "Herald mapping config '{}' (source '{}') has unrecognized timestamp_unit '{}' "
                "(expected unix_ms or unix_seconds)",
                yaml_path, mapping.source_name, mapping.timestamp_unit);
            return std::nullopt;
        }
    }

    if (root["entity_class"]) {
        const auto entity_class_name = root["entity_class"].as<std::string>();
        if (!herald::v0::EntityClass_Parse(entity_class_name, &mapping.entity_class)) {
            spdlog::error(
                "Herald mapping config '{}' (source '{}') has unrecognized entity_class '{}'",
                yaml_path, mapping.source_name, entity_class_name);
            return std::nullopt;
        }
    }

    if (!root["fields"] || !root["fields"].IsMap()) {
        spdlog::error("Herald mapping config '{}' (source '{}') has no fields map", yaml_path,
                      mapping.source_name);
        return std::nullopt;
    }
    for (const auto& entry : root["fields"]) {
        mapping.fields[entry.first.as<std::string>()] = entry.second.as<std::string>();
    }

    static constexpr std::array<const char*, 4> kRequiredFields = {
        "entity_id", "timestamp_ms", "latitude_deg", "longitude_deg"};
    for (const auto* required : kRequiredFields) {
        if (!mapping.fields.contains(required)) {
            spdlog::error(
                "Herald mapping config '{}' (source '{}') has no field mapping for required "
                "field '{}'",
                yaml_path, mapping.source_name, required);
            return std::nullopt;
        }
    }

    return mapping;
}

std::optional<herald::v0::Herald> HeraldFieldMapper::apply(const HeraldFieldMapping& mapping,
                                                             const nlohmann::json& payload) {
    herald::v0::Herald msg;
    msg.set_entity_class(mapping.entity_class);
    msg.set_source(mapping.source_name);

    // entity_id: required, read as a plain string (device IDs are IDs, not
    // numbers to arithmetic on, even when a vendor happens to send a
    // numeric-looking one).
    const auto entity_id_path = *find_field_path(mapping, "entity_id");
    const auto entity_id_value = resolve_path(payload, entity_id_path);
    if (!entity_id_value) {
        spdlog::warn("Herald mapped source '{}': payload missing entity_id at path '{}'",
                     mapping.source_name, entity_id_path);
        return std::nullopt;
    }
    if (entity_id_value->is_string()) {
        msg.set_entity_id(entity_id_value->get<std::string>());
    } else if (entity_id_value->is_number()) {
        msg.set_entity_id(entity_id_value->dump());
    } else {
        spdlog::warn("Herald mapped source '{}': entity_id at path '{}' is not a string or number",
                     mapping.source_name, entity_id_path);
        return std::nullopt;
    }

    // timestamp_ms: required, numeric, unit-converted per the mapping.
    const auto timestamp_path = *find_field_path(mapping, "timestamp_ms");
    const auto timestamp_value = resolve_path(payload, timestamp_path);
    const auto timestamp_number = timestamp_value ? as_number(*timestamp_value) : std::nullopt;
    if (!timestamp_number) {
        spdlog::warn("Herald mapped source '{}': payload missing numeric timestamp at path '{}'",
                     mapping.source_name, timestamp_path);
        return std::nullopt;
    }
    const double timestamp_ms =
        mapping.timestamp_unit == "unix_seconds" ? *timestamp_number * 1000.0 : *timestamp_number;
    msg.set_timestamp_ms(static_cast<uint64_t>(timestamp_ms));

    // latitude_deg/longitude_deg: required, numeric.
    auto* position = msg.mutable_position();
    const auto latitude_path = *find_field_path(mapping, "latitude_deg");
    const auto latitude_value = resolve_path(payload, latitude_path);
    const auto latitude_number = latitude_value ? as_number(*latitude_value) : std::nullopt;
    if (!latitude_number) {
        spdlog::warn("Herald mapped source '{}': payload missing numeric latitude_deg at path '{}'",
                     mapping.source_name, latitude_path);
        return std::nullopt;
    }
    position->set_latitude_deg(*latitude_number);

    const auto longitude_path = *find_field_path(mapping, "longitude_deg");
    const auto longitude_value = resolve_path(payload, longitude_path);
    const auto longitude_number = longitude_value ? as_number(*longitude_value) : std::nullopt;
    if (!longitude_number) {
        spdlog::warn(
            "Herald mapped source '{}': payload missing numeric longitude_deg at path '{}'",
            mapping.source_name, longitude_path);
        return std::nullopt;
    }
    position->set_longitude_deg(*longitude_number);

    // Optional fields: absent mapping entry, absent payload value, or wrong
    // type all just mean "leave this part of the Herald message unset" -
    // never a rejection, unlike the required fields above.
    if (const auto path = find_field_path(mapping, "altitude_msl_m")) {
        if (const auto value = resolve_path(payload, *path)) {
            if (const auto number = as_number(*value)) {
                position->set_altitude_msl_m(static_cast<float>(*number));
            }
        }
    }
    if (const auto path = find_field_path(mapping, "battery_voltage_v")) {
        if (const auto value = resolve_path(payload, *path)) {
            if (const auto number = as_number(*value)) {
                msg.mutable_battery()->set_voltage_v(static_cast<float>(*number));
            }
        }
    }
    if (const auto path = find_field_path(mapping, "battery_remaining_pct")) {
        if (const auto value = resolve_path(payload, *path)) {
            if (const auto number = as_number(*value)) {
                msg.mutable_battery()->set_remaining_pct(static_cast<float>(*number));
            }
        }
    }
    if (const auto path = find_field_path(mapping, "num_satellites")) {
        if (const auto value = resolve_path(payload, *path)) {
            if (const auto number = as_number(*value)) {
                msg.mutable_gps()->set_num_satellites(static_cast<uint32_t>(*number));
            }
        }
    }
    if (const auto path = find_field_path(mapping, "health_ok")) {
        if (const auto value = resolve_path(payload, *path)) {
            if (value->is_boolean()) {
                msg.set_health_ok(value->get<bool>());
            }
        }
    } else {
        // No health signal mapped at all: assume healthy rather than
        // leaving the (non-optional) health_ok field defaulted to proto3's
        // false, which would read as "unhealthy" for every message from a
        // source that simply doesn't report health.
        msg.set_health_ok(true);
    }

    return msg;
}
