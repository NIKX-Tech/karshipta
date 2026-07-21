#include "fleet_zone_store.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace {

// Not a real UUID, same rationale as mission_importer.cpp's
// synthesize_mission_id(): the schema comments *_id as an id, nothing
// validates the format, and this only needs to be unique enough for one
// gateway process. Nanosecond resolution plus the prefix keeps fleet_id and
// zone_id visibly distinct from each other and from ward_id (operator-chosen,
// never synthesized) if they ever show up together in a log line.
std::string synthesize_id(const char* prefix) {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return std::string(prefix) + "-" + std::to_string(now_ns);
}

// RAII wrapper for a prepared statement: finalizes on every exit path
// (return, throw, break) instead of requiring a matching sqlite3_finalize()
// at each one.
class Statement {
   public:
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("failed to prepare statement: ") + sqlite3_errmsg(db));
        }
    }
    ~Statement() { sqlite3_finalize(stmt_); }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&&) = delete;
    Statement& operator=(Statement&&) = delete;

    void bind(const int index, const std::string& value) {
        sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }
    void bind(const int index, const int value) { sqlite3_bind_int(stmt_, index, value); }
    void bind(const int index, const double value) { sqlite3_bind_double(stmt_, index, value); }
    // optional<float> maps to SQLite NULL when unset, matching the proto
    // field's own `optional` semantics (unset = no bound).
    void bind(const int index, const std::optional<float>& value) {
        if (value) {
            sqlite3_bind_double(stmt_, index, *value);
        } else {
            sqlite3_bind_null(stmt_, index);
        }
    }

    // Runs a mutating statement (INSERT/UPDATE/DELETE) to completion.
    void run() {
        if (sqlite3_step(stmt_) != SQLITE_DONE) {
            throw std::runtime_error(std::string("statement failed: ") +
                                      sqlite3_errmsg(sqlite3_db_handle(stmt_)));
        }
    }

    // Advances to the next row; false once exhausted.
    bool step() {
        const int result = sqlite3_step(stmt_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("statement failed: ") +
                                  sqlite3_errmsg(sqlite3_db_handle(stmt_)));
    }

    std::string column_text(const int index) const {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index));
        return text ? std::string(text) : std::string();
    }
    int column_int(const int index) const { return sqlite3_column_int(stmt_, index); }
    double column_double(const int index) const { return sqlite3_column_double(stmt_, index); }
    std::optional<float> column_optional_float(const int index) const {
        if (sqlite3_column_type(stmt_, index) == SQLITE_NULL) return std::nullopt;
        return static_cast<float>(sqlite3_column_double(stmt_, index));
    }

    sqlite3_stmt* stmt_ = nullptr;
};

// Runs a plain (non-parameterized) statement, e.g. schema DDL or a PRAGMA.
void exec(sqlite3* db, const char* sql) {
    char* error_message = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error_message) != SQLITE_OK) {
        const std::string message = error_message ? error_message : "unknown error";
        sqlite3_free(error_message);
        throw std::runtime_error("sqlite exec failed: " + message);
    }
}

}  // namespace

FleetZoneStore::FleetZoneStore(std::filesystem::path path) {
    if (sqlite3_open(path.string().c_str(), &db_) != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "unknown error";
        if (db_) sqlite3_close(db_);
        throw std::runtime_error("failed to open '" + path.string() + "': " + message);
    }
    // SQLite disables foreign key enforcement by default even when the
    // schema declares ON DELETE CASCADE - without this, delete_fleet()/
    // delete_zone() would silently leave orphaned fleet_wards/zone_vertices
    // rows behind instead of cascading. Must be set on every connection,
    // not just once ever, per SQLite's own documented behavior.
    exec(db_, "PRAGMA foreign_keys = ON;");
    create_schema();
}

FleetZoneStore::~FleetZoneStore() {
    if (db_) sqlite3_close(db_);
}

void FleetZoneStore::create_schema() {
    exec(db_, R"(
        CREATE TABLE IF NOT EXISTS fleets (
            fleet_id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            description TEXT NOT NULL DEFAULT ''
        );
    )");
    exec(db_, R"(
        CREATE TABLE IF NOT EXISTS fleet_wards (
            fleet_id TEXT NOT NULL REFERENCES fleets(fleet_id) ON DELETE CASCADE,
            ward_id TEXT NOT NULL,
            PRIMARY KEY (fleet_id, ward_id)
        );
    )");
    exec(db_, R"(
        CREATE TABLE IF NOT EXISTS zones (
            zone_id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            type INTEGER NOT NULL,
            altitude_min_m REAL,
            altitude_max_m REAL
        );
    )");
    exec(db_, R"(
        CREATE TABLE IF NOT EXISTS zone_vertices (
            zone_id TEXT NOT NULL REFERENCES zones(zone_id) ON DELETE CASCADE,
            seq INTEGER NOT NULL,
            latitude_deg REAL NOT NULL,
            longitude_deg REAL NOT NULL,
            PRIMARY KEY (zone_id, seq)
        );
    )");
}

// ---------- Fleet ----------

std::string FleetZoneStore::create_fleet(const std::string& name, const std::string& description) {
    const std::string fleet_id = synthesize_id("fleet");
    Statement insert(db_, "INSERT INTO fleets (fleet_id, name, description) VALUES (?, ?, ?);");
    insert.bind(1, fleet_id);
    insert.bind(2, name);
    insert.bind(3, description);
    insert.run();
    return fleet_id;
}

std::optional<std::string> FleetZoneStore::rename_fleet(const std::string& fleet_id,
                                                          const std::string& name,
                                                          const std::string& description) {
    Statement update(db_, "UPDATE fleets SET name = ?, description = ? WHERE fleet_id = ?;");
    update.bind(1, name);
    update.bind(2, description);
    update.bind(3, fleet_id);
    update.run();
    if (sqlite3_changes(db_) == 0) {
        return "unknown fleet_id: " + fleet_id;
    }
    return std::nullopt;
}

std::optional<std::string> FleetZoneStore::delete_fleet(const std::string& fleet_id) {
    Statement remove(db_, "DELETE FROM fleets WHERE fleet_id = ?;");
    remove.bind(1, fleet_id);
    remove.run();
    if (sqlite3_changes(db_) == 0) {
        return "unknown fleet_id: " + fleet_id;
    }
    return std::nullopt;
}

std::optional<std::string> FleetZoneStore::add_ward_to_fleet(const std::string& fleet_id,
                                                               const std::string& ward_id) {
    {
        Statement exists(db_, "SELECT 1 FROM fleets WHERE fleet_id = ?;");
        exists.bind(1, fleet_id);
        if (!exists.step()) {
            return "unknown fleet_id: " + fleet_id;
        }
    }
    // INSERT OR IGNORE: idempotent, matching the doc comment - adding a ward
    // already in the fleet is a no-op, not an error.
    Statement insert(db_, "INSERT OR IGNORE INTO fleet_wards (fleet_id, ward_id) VALUES (?, ?);");
    insert.bind(1, fleet_id);
    insert.bind(2, ward_id);
    insert.run();
    return std::nullopt;
}

std::optional<std::string> FleetZoneStore::remove_ward_from_fleet(const std::string& fleet_id,
                                                                    const std::string& ward_id) {
    {
        Statement exists(db_, "SELECT 1 FROM fleets WHERE fleet_id = ?;");
        exists.bind(1, fleet_id);
        if (!exists.step()) {
            return "unknown fleet_id: " + fleet_id;
        }
    }
    // Idempotent: removing a ward that isn't a member is a no-op, same
    // rationale as add_ward_to_fleet().
    Statement remove(db_, "DELETE FROM fleet_wards WHERE fleet_id = ? AND ward_id = ?;");
    remove.bind(1, fleet_id);
    remove.bind(2, ward_id);
    remove.run();
    return std::nullopt;
}

std::vector<karshipta::v1::Fleet> FleetZoneStore::list_fleets() const {
    std::vector<karshipta::v1::Fleet> fleets;
    Statement select(db_, "SELECT fleet_id, name, description FROM fleets ORDER BY name;");
    while (select.step()) {
        karshipta::v1::Fleet fleet;
        fleet.set_fleet_id(select.column_text(0));
        fleet.set_name(select.column_text(1));
        fleet.set_description(select.column_text(2));

        Statement members(db_, "SELECT ward_id FROM fleet_wards WHERE fleet_id = ? ORDER BY ward_id;");
        members.bind(1, fleet.fleet_id());
        while (members.step()) {
            fleet.add_ward_ids(members.column_text(0));
        }
        fleets.push_back(std::move(fleet));
    }
    return fleets;
}

// ---------- Zone ----------

std::string FleetZoneStore::create_zone(const std::string& name, const karshipta::v1::ZoneType type,
                                         const std::vector<karshipta::v1::GeoPoint>& vertices,
                                         const std::optional<float> altitude_min_m,
                                         const std::optional<float> altitude_max_m) {
    const std::string zone_id = synthesize_id("zone");
    Statement insert(
        db_, "INSERT INTO zones (zone_id, name, type, altitude_min_m, altitude_max_m) "
             "VALUES (?, ?, ?, ?, ?);");
    insert.bind(1, zone_id);
    insert.bind(2, name);
    insert.bind(3, static_cast<int>(type));
    insert.bind(4, altitude_min_m);
    insert.bind(5, altitude_max_m);
    insert.run();

    Statement insert_vertex(
        db_, "INSERT INTO zone_vertices (zone_id, seq, latitude_deg, longitude_deg) VALUES (?, ?, ?, ?);");
    for (std::size_t seq = 0; seq < vertices.size(); ++seq) {
        insert_vertex.bind(1, zone_id);
        insert_vertex.bind(2, static_cast<int>(seq));
        insert_vertex.bind(3, vertices[seq].latitude_deg());
        insert_vertex.bind(4, vertices[seq].longitude_deg());
        insert_vertex.run();
        sqlite3_reset(insert_vertex.stmt_);
    }
    return zone_id;
}

std::optional<std::string> FleetZoneStore::update_zone(const std::string& zone_id,
                                                         const std::string& name,
                                                         const karshipta::v1::ZoneType type,
                                                         const std::optional<float> altitude_min_m,
                                                         const std::optional<float> altitude_max_m) {
    Statement update(
        db_, "UPDATE zones SET name = ?, type = ?, altitude_min_m = ?, altitude_max_m = ? "
             "WHERE zone_id = ?;");
    update.bind(1, name);
    update.bind(2, static_cast<int>(type));
    update.bind(3, altitude_min_m);
    update.bind(4, altitude_max_m);
    update.bind(5, zone_id);
    update.run();
    if (sqlite3_changes(db_) == 0) {
        return "unknown zone_id: " + zone_id;
    }
    return std::nullopt;
}

std::optional<std::string> FleetZoneStore::delete_zone(const std::string& zone_id) {
    Statement remove(db_, "DELETE FROM zones WHERE zone_id = ?;");
    remove.bind(1, zone_id);
    remove.run();
    if (sqlite3_changes(db_) == 0) {
        return "unknown zone_id: " + zone_id;
    }
    return std::nullopt;
}

std::vector<karshipta::v1::Zone> FleetZoneStore::list_zones() const {
    std::vector<karshipta::v1::Zone> zones;
    Statement select(db_,
                      "SELECT zone_id, name, type, altitude_min_m, altitude_max_m FROM zones ORDER BY name;");
    while (select.step()) {
        karshipta::v1::Zone zone;
        zone.set_zone_id(select.column_text(0));
        zone.set_name(select.column_text(1));
        zone.set_type(static_cast<karshipta::v1::ZoneType>(select.column_int(2)));
        if (const auto min_alt = select.column_optional_float(3)) {
            zone.set_altitude_min_m(*min_alt);
        }
        if (const auto max_alt = select.column_optional_float(4)) {
            zone.set_altitude_max_m(*max_alt);
        }

        Statement vertices(
            db_, "SELECT latitude_deg, longitude_deg FROM zone_vertices WHERE zone_id = ? ORDER BY seq;");
        vertices.bind(1, zone.zone_id());
        while (vertices.step()) {
            auto* vertex = zone.add_vertices();
            vertex->set_latitude_deg(vertices.column_double(0));
            vertex->set_longitude_deg(vertices.column_double(1));
        }
        zones.push_back(std::move(zone));
    }
    return zones;
}
