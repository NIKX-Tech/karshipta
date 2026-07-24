#ifndef KARSHIPTA_GATEWAY_FLEET_ZONE_STORE_H
#define KARSHIPTA_GATEWAY_FLEET_ZONE_STORE_H

#include <filesystem>
#include <mutex>
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
// batching across calls (create_zone is the one exception: its zone-row and
// per-vertex inserts are wrapped in a single SQLite transaction, so a
// mid-operation failure can never leave a zone committed with a partial
// vertex list).
//
// Every public method takes mutex_ for its entire body: Transport::on_receive
// fires on a per-connection worker thread, one thread per connection (see
// websocket_transport.h), so two clients mutating Fleet/Zone state
// concurrently is a real scenario, not a hypothetical. Coarse-grained (one
// mutex for the whole store, not per-table/per-row) deliberately, matching
// this class's own "not a hot path" framing above - this also closes a real
// TOCTOU gap add_ward_to_fleet()/remove_ward_from_fleet() otherwise have
// (existence check, then a separate mutating statement): a concurrent
// delete_fleet() between the two could turn the mutation into a foreign-key
// violation. One lock around each method makes that interleaving impossible.
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
    // Guards every public method's entire body; see the class comment above.
    mutable std::mutex mutex_;
};

#endif  // KARSHIPTA_GATEWAY_FLEET_ZONE_STORE_H
