#ifndef KARSHIPTA_GATEWAY_HERALD_FIELD_MAPPER_H
#define KARSHIPTA_GATEWAY_HERALD_FIELD_MAPPER_H

#include <herald/v0/herald.pb.h>
#include <nlohmann/json_fwd.hpp>

#include <map>
#include <optional>
#include <string>

// Declarative field mapping for a non-native Herald source (github issue
// #102, the Herald spec's "Mapped" adoption path): a vendor's own JSON
// payload gets translated into a herald::v0::Herald via a YAML config that
// says where each Herald field lives in the vendor's own field names,
// instead of writing a new bespoke integration per vendor. One config per
// source, identified by source_name; HeraldHttpServer looks one up by name
// from the request URL (POST /herald/mapped/<source_name>) and applies it
// before calling HeraldWardManager::ingest() - the same entry point the
// native and GT06 paths already use.
//
// Scope is deliberately narrow (github issue #102's own scope, pulled back
// from the original milestone): one clearly-documented generic worked
// example (gateway/config/herald_mappings/example-generic.yaml), a template
// for the community or future work to extend, not a specific closed
// vendor's integration and not every possible Herald field. Supports the
// fields a typical vendor payload plausibly offers - position, timestamp,
// battery, satellite count, health - not velocity, hdop, tags, or metadata.
// entity_class is a fixed value per mapping config, not a further
// per-message value-to-EntityClass lookup table (the "what value maps to
// which EntityClass" idea the original issue raised) - a source reporting
// more than one device type needs a separate mapping config per type, which
// the existing one-config-per-source model already supports without extra
// machinery; a real per-message lookup table is a natural future extension
// if ever needed, not implemented here.
struct HeraldFieldMapping {
    std::string source_name;
    herald::v0::EntityClass entity_class = herald::v0::ENTITY_CLASS_UNSPECIFIED;
    // "unix_ms" (default) or "unix_seconds" - vendors disagree on units;
    // ISO 8601 string timestamps are out of scope for this pass (real
    // timezone/format complexity, not needed for one worked example).
    std::string timestamp_unit = "unix_ms";

    // Herald field name -> dot-path into the vendor's own JSON payload
    // (e.g. "location.lat" reads payload["location"]["lat"]). Recognized
    // keys: entity_id, timestamp_ms (required); latitude_deg, longitude_deg
    // (required); altitude_msl_m, battery_voltage_v, battery_remaining_pct,
    // num_satellites, health_ok (all optional - a message missing an
    // optional field's path, or with no mapping entry for it at all, simply
    // leaves that part of the Herald message unset). Any other key is
    // ignored, not an error - keeps a mapping config forward-compatible
    // with a future field this parser doesn't know about yet.
    std::map<std::string, std::string> fields;
};

class HeraldFieldMapper {
   public:
    // Parses one mapping config file. Returns nullopt (logging why) on a
    // missing file, malformed YAML, or a config missing source_name / any
    // required field's path - never throws.
    static std::optional<HeraldFieldMapping> load_mapping(const std::string& yaml_path);

    // Applies `mapping` to `payload` (already-parsed vendor JSON) and
    // builds a herald::v0::Herald. Returns nullopt (logging why) if a
    // required field's path is missing from `payload`, points at a value of
    // the wrong JSON type, or `timestamp_ms`'s value can't be read as a
    // number - never throws (a malformed request body must not crash the
    // ingestion path).
    static std::optional<herald::v0::Herald> apply(const HeraldFieldMapping& mapping,
                                                     const nlohmann::json& payload);
};

#endif  // KARSHIPTA_GATEWAY_HERALD_FIELD_MAPPER_H
