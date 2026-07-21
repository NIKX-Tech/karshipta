#ifndef KARSHIPTA_GATEWAY_FLEET_ZONE_STORE_H
#define KARSHIPTA_GATEWAY_FLEET_ZONE_STORE_H

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <karshipta/v1/fleet.pb.h>

struct sqlite3;  // opaque; keeps sqlite3.h out of every consumer's include path

// Owns the SQLite-backed persistence for Fleet and Zone (gateway issue #85).
// Fleet and Zone are named, saved, shared-within-one-operator objects, not
// live telemetry - this narrowly reopens the "no backend, no database in the
// MVP" rule (root CLAUDE.md), scoped to exactly these two saved-object types,
// with explicit product-owner sign-off (fleet-mission-model.md). SQLite
// instead of WardManager's YAML: Fleet<->Ward membership is many-to-many and
// Zone vertices are ordered, both more naturally relational than a flat list.
//
// RAII: opens the database and ensures the schema exists in the constructor,
// closes in the destructor. Every mutating call commits immediately - a
// handful of rows at this scale, not a hot path, so no explicit transaction
// batching across calls.
class FleetZoneStore {
   public:
    // path: the SQLite database file. ":memory:" works for tests (a fresh,
    // private, in-process database, never touching disk).
    // Throws std::runtime_error if the database cannot be opened or the
    // schema cannot be created - both indicate a broken environment (bad
    // path, corrupt file, out of disk), not a recoverable per-request
    // condition, so this is not folded into an std::optional<std::string>
    // return like the CRUD methods below.
    explicit FleetZoneStore(std::filesystem::path path);
    ~FleetZoneStore();

    FleetZoneStore(const FleetZoneStore&) = delete;
    FleetZoneStore& operator=(const FleetZoneStore&) = delete;
    FleetZoneStore(FleetZoneStore&&) = delete;
    FleetZoneStore& operator=(FleetZoneStore&&) = delete;

    // ---------- Fleet ----------

    // Generates a fresh fleet_id (gateway-assigned; unlike ward_id, the
    // console never supplies one) and inserts the row.
    std::string create_fleet(const std::string& name, const std::string& description);
    // Updates both name and description together (fleet.proto's RenameFleet
    // comment). Error string if fleet_id is unknown; nullopt on success.
    std::optional<std::string> rename_fleet(const std::string& fleet_id, const std::string& name,
                                             const std::string& description);
    // Cascades: also removes every fleet_wards row for this fleet.
    std::optional<std::string> delete_fleet(const std::string& fleet_id);
    // Idempotent: adding a ward already in the fleet, or removing one that
    // isn't, both succeed as a no-op (matches the config-mutation style
    // WardManager already uses elsewhere).
    std::optional<std::string> add_ward_to_fleet(const std::string& fleet_id, const std::string& ward_id);
    std::optional<std::string> remove_ward_from_fleet(const std::string& fleet_id,
                                                        const std::string& ward_id);
    std::vector<karshipta::v1::Fleet> list_fleets() const;

    // ---------- Zone ----------

    // Vertices are fixed at creation - only name/type/altitude bounds can
    // change afterward (update_zone), matching the console's v0 editing
    // scope. Does not validate vertex count (>= 3 for a real polygon); the
    // wire handler layer owns that check, same division of labor as
    // WardManager's handlers validating before calling into persistence.
    std::string create_zone(const std::string& name, karshipta::v1::ZoneType type,
                             const std::vector<karshipta::v1::GeoPoint>& vertices,
                             std::optional<float> altitude_min_m, std::optional<float> altitude_max_m);
    std::optional<std::string> update_zone(const std::string& zone_id, const std::string& name,
                                            karshipta::v1::ZoneType type,
                                            std::optional<float> altitude_min_m,
                                            std::optional<float> altitude_max_m);
    // Cascades: also removes every zone_vertices row for this zone.
    std::optional<std::string> delete_zone(const std::string& zone_id);
    std::vector<karshipta::v1::Zone> list_zones() const;

   private:
    void create_schema();

    sqlite3* db_ = nullptr;
};

#endif  // KARSHIPTA_GATEWAY_FLEET_ZONE_STORE_H
