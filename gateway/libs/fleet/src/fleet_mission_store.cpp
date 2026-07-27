#include "fleet_mission_store.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace {

// Same rationale as fleet_zone_store.cpp's synthesize_id: not a real UUID,
// nothing validates the format, only needs to be unique enough for one
// gateway process.
std::string synthesize_id(const char* prefix) {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return std::string(prefix) + "-" + std::to_string(now_ns);
}

// RAII wrapper for a prepared statement, identical shape to
// fleet_zone_store.cpp's own copy - each store file owns its copy rather
// than sharing a header for this, matching that file's own precedent
// (serialize_envelope in fleet_manager.cpp is duplicated the same way).
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
    // BLOB bind for a serialized WardMissionPlan (see create_fleet_mission).
    void bind_blob(const int index, const std::string& value) {
        sqlite3_bind_blob(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }
    void bind(const int index, const uint32_t value) { sqlite3_bind_int64(stmt_, index, value); }
    void bind(const int index, const uint64_t value) {
        sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(value));
    }

    void run() {
        if (sqlite3_step(stmt_) != SQLITE_DONE) {
            throw std::runtime_error(std::string("statement failed: ") +
                                      sqlite3_errmsg(sqlite3_db_handle(stmt_)));
        }
    }

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
    std::string column_blob(const int index) const {
        const auto* data = reinterpret_cast<const char*>(sqlite3_column_blob(stmt_, index));
        const int size = sqlite3_column_bytes(stmt_, index);
        return data ? std::string(data, static_cast<std::size_t>(size)) : std::string();
    }
    int column_int(const int index) const { return sqlite3_column_int(stmt_, index); }
    uint32_t column_uint32(const int index) const {
        return static_cast<uint32_t>(sqlite3_column_int64(stmt_, index));
    }
    uint64_t column_uint64(const int index) const {
        return static_cast<uint64_t>(sqlite3_column_int64(stmt_, index));
    }

    sqlite3_stmt* stmt_ = nullptr;
};

void exec(sqlite3* db, const char* sql) {
    char* error_message = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error_message) != SQLITE_OK) {
        const std::string message = error_message ? error_message : "unknown error";
        sqlite3_free(error_message);
        throw std::runtime_error("sqlite exec failed: " + message);
    }
}

// RAII SQLite transaction, identical shape to fleet_zone_store.cpp's own copy.
class Transaction {
   public:
    explicit Transaction(sqlite3* db) : db_(db) { exec(db_, "BEGIN;"); }
    ~Transaction() {
        if (!committed_) {
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    void commit() {
        exec(db_, "COMMIT;");
        committed_ = true;
    }

   private:
    sqlite3* db_;
    bool committed_ = false;
};

}  // namespace

FleetMissionStore::FleetMissionStore(std::filesystem::path path) {
    if (sqlite3_open(path.string().c_str(), &db_) != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "unknown error";
        if (db_) sqlite3_close(db_);
        throw std::runtime_error("failed to open '" + path.string() + "': " + message);
    }
    // See fleet_zone_store.cpp's identical PRAGMA: must be set on every
    // connection, not just once ever, per SQLite's documented behavior.
    exec(db_, "PRAGMA foreign_keys = ON;");
    create_schema();
}

FleetMissionStore::~FleetMissionStore() {
    if (db_) sqlite3_close(db_);
}

void FleetMissionStore::create_schema() {
    exec(db_, R"(
        CREATE TABLE IF NOT EXISTS fleet_missions (
            fleet_mission_id TEXT PRIMARY KEY,
            fleet_id TEXT NOT NULL DEFAULT '',
            mission_name TEXT NOT NULL,
            repeat_count INTEGER NOT NULL,
            status INTEGER NOT NULL,
            created_at_ms INTEGER NOT NULL
        );
    )");
    // plan_blob is a serialized WardMissionPlan (ward_id + items together) -
    // read/written whole, never queried by an individual waypoint's fields,
    // so a blob is a deliberate simplification versus Zone's fully
    // relational vertex table (fleet_zone_store.cpp's zone_vertices).
    exec(db_, R"(
        CREATE TABLE IF NOT EXISTS fleet_mission_ward_plans (
            fleet_mission_id TEXT NOT NULL REFERENCES fleet_missions(fleet_mission_id) ON DELETE CASCADE,
            ward_id TEXT NOT NULL,
            plan_blob BLOB NOT NULL,
            PRIMARY KEY (fleet_mission_id, ward_id)
        );
    )");
    exec(db_, R"(
        CREATE TABLE IF NOT EXISTS fleet_mission_ward_states (
            fleet_mission_id TEXT NOT NULL REFERENCES fleet_missions(fleet_mission_id) ON DELETE CASCADE,
            ward_id TEXT NOT NULL,
            status INTEGER NOT NULL,
            message TEXT NOT NULL DEFAULT '',
            mission_id TEXT NOT NULL DEFAULT '',
            PRIMARY KEY (fleet_mission_id, ward_id)
        );
    )");
}

std::string FleetMissionStore::create_fleet_mission(
    const std::string& fleet_id, const std::string& mission_name, const uint32_t repeat_count,
    const std::vector<karshipta::v1::WardMissionPlan>& ward_plans) {
    std::lock_guard lock(mutex_);
    const std::string fleet_mission_id = synthesize_id("fleet-mission");
    const auto created_at_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());

    // Transaction, not just the mutex: a mid-loop SQLite failure on this
    // thread must not leave a FleetMission committed with a partial set of
    // per-ward plans/states - same reasoning as fleet_zone_store.cpp's
    // create_zone.
    Transaction txn(db_);
    Statement insert(db_,
                      "INSERT INTO fleet_missions "
                      "(fleet_mission_id, fleet_id, mission_name, repeat_count, status, created_at_ms) "
                      "VALUES (?, ?, ?, ?, ?, ?);");
    insert.bind(1, fleet_mission_id);
    insert.bind(2, fleet_id);
    insert.bind(3, mission_name);
    insert.bind(4, repeat_count);
    insert.bind(5, static_cast<int>(karshipta::v1::FLEET_MISSION_STATUS_ACTIVE));
    insert.bind(6, created_at_ms);
    insert.run();

    Statement insert_plan(
        db_, "INSERT INTO fleet_mission_ward_plans (fleet_mission_id, ward_id, plan_blob) VALUES (?, ?, ?);");
    Statement insert_state(db_,
                            "INSERT INTO fleet_mission_ward_states "
                            "(fleet_mission_id, ward_id, status, message, mission_id) "
                            "VALUES (?, ?, ?, '', '');");
    for (const auto& plan : ward_plans) {
        insert_plan.bind(1, fleet_mission_id);
        insert_plan.bind(2, plan.ward_id());
        insert_plan.bind_blob(3, plan.SerializeAsString());
        insert_plan.run();
        sqlite3_reset(insert_plan.stmt_);

        insert_state.bind(1, fleet_mission_id);
        insert_state.bind(2, plan.ward_id());
        insert_state.bind(3, static_cast<int>(karshipta::v1::WARD_MISSION_STATUS_UNSPECIFIED));
        insert_state.run();
        sqlite3_reset(insert_state.stmt_);
    }
    txn.commit();
    return fleet_mission_id;
}

namespace {

// Shared by get_fleet_mission/list_fleet_missions: builds one FleetMission
// from its three tables. Caller already holds mutex_.
karshipta::v1::FleetMission load_fleet_mission_locked(sqlite3* db, const std::string& fleet_mission_id,
                                                       const std::string& fleet_id,
                                                       const std::string& mission_name,
                                                       const uint32_t repeat_count, const int status,
                                                       const uint64_t created_at_ms) {
    karshipta::v1::FleetMission mission;
    mission.set_fleet_mission_id(fleet_mission_id);
    mission.set_fleet_id(fleet_id);
    mission.set_mission_name(mission_name);
    mission.set_repeat_count(repeat_count);
    mission.set_status(static_cast<karshipta::v1::FleetMissionStatus>(status));
    mission.set_created_at_ms(created_at_ms);

    Statement plans(db, "SELECT plan_blob FROM fleet_mission_ward_plans WHERE fleet_mission_id = ? ORDER BY ward_id;");
    plans.bind(1, fleet_mission_id);
    while (plans.step()) {
        auto* plan = mission.add_ward_plans();
        if (!plan->ParseFromString(plans.column_blob(0))) {
            throw std::runtime_error("corrupt ward_plan blob for fleet_mission_id: " + fleet_mission_id);
        }
    }

    Statement states(db,
                      "SELECT ward_id, status, message, mission_id FROM fleet_mission_ward_states "
                      "WHERE fleet_mission_id = ? ORDER BY ward_id;");
    states.bind(1, fleet_mission_id);
    while (states.step()) {
        auto* state = mission.add_ward_states();
        state->set_ward_id(states.column_text(0));
        state->set_status(static_cast<karshipta::v1::WardMissionStatus>(states.column_int(1)));
        state->set_message(states.column_text(2));
        state->set_mission_id(states.column_text(3));
    }
    return mission;
}

}  // namespace

std::optional<karshipta::v1::FleetMission> FleetMissionStore::get_fleet_mission(
    const std::string& fleet_mission_id) const {
    std::lock_guard lock(mutex_);
    Statement select(db_,
                      "SELECT fleet_id, mission_name, repeat_count, status, created_at_ms "
                      "FROM fleet_missions WHERE fleet_mission_id = ?;");
    select.bind(1, fleet_mission_id);
    if (!select.step()) return std::nullopt;
    return load_fleet_mission_locked(db_, fleet_mission_id, select.column_text(0), select.column_text(1),
                                      select.column_uint32(2), select.column_int(3),
                                      select.column_uint64(4));
}

std::vector<karshipta::v1::FleetMission> FleetMissionStore::list_fleet_missions() const {
    std::lock_guard lock(mutex_);
    std::vector<karshipta::v1::FleetMission> missions;
    Statement select(db_,
                      "SELECT fleet_mission_id, fleet_id, mission_name, repeat_count, status, created_at_ms "
                      "FROM fleet_missions ORDER BY created_at_ms;");
    while (select.step()) {
        missions.push_back(load_fleet_mission_locked(db_, select.column_text(0), select.column_text(1),
                                                       select.column_text(2), select.column_uint32(3),
                                                       select.column_int(4), select.column_uint64(5)));
    }
    return missions;
}

std::optional<std::string> FleetMissionStore::update_ward_state(const std::string& fleet_mission_id,
                                                                  const karshipta::v1::WardMissionState& state) {
    std::lock_guard lock(mutex_);
    Statement update(db_,
                      "UPDATE fleet_mission_ward_states SET status = ?, message = ?, mission_id = ? "
                      "WHERE fleet_mission_id = ? AND ward_id = ?;");
    update.bind(1, static_cast<int>(state.status()));
    update.bind(2, state.message());
    update.bind(3, state.mission_id());
    update.bind(4, fleet_mission_id);
    update.bind(5, state.ward_id());
    update.run();
    if (sqlite3_changes(db_) == 0) {
        return "unknown fleet_mission_id/ward_id: " + fleet_mission_id + "/" + state.ward_id();
    }
    return std::nullopt;
}

std::optional<std::string> FleetMissionStore::set_status(const std::string& fleet_mission_id,
                                                           const karshipta::v1::FleetMissionStatus status) {
    std::lock_guard lock(mutex_);
    Statement update(db_, "UPDATE fleet_missions SET status = ? WHERE fleet_mission_id = ?;");
    update.bind(1, static_cast<int>(status));
    update.bind(2, fleet_mission_id);
    update.run();
    if (sqlite3_changes(db_) == 0) {
        return "unknown fleet_mission_id: " + fleet_mission_id;
    }
    return std::nullopt;
}

std::optional<std::string> FleetMissionStore::update_ward_plans(
    const std::string& fleet_mission_id, const std::string& mission_name, const uint32_t repeat_count,
    const std::vector<karshipta::v1::WardMissionPlan>& ward_plans) {
    std::lock_guard lock(mutex_);
    {
        Statement exists(db_, "SELECT 1 FROM fleet_missions WHERE fleet_mission_id = ?;");
        exists.bind(1, fleet_mission_id);
        if (!exists.step()) {
            return "unknown fleet_mission_id: " + fleet_mission_id;
        }
    }
    // Transaction: replacing every plan/state row must not leave a partial
    // set committed on a mid-loop failure, same reasoning as
    // create_fleet_mission.
    Transaction txn(db_);
    Statement update_mission(
        db_, "UPDATE fleet_missions SET mission_name = ?, repeat_count = ?, status = ? WHERE fleet_mission_id = ?;");
    update_mission.bind(1, mission_name);
    update_mission.bind(2, repeat_count);
    update_mission.bind(3, static_cast<int>(karshipta::v1::FLEET_MISSION_STATUS_ACTIVE));
    update_mission.bind(4, fleet_mission_id);
    update_mission.run();

    Statement delete_plans(db_, "DELETE FROM fleet_mission_ward_plans WHERE fleet_mission_id = ?;");
    delete_plans.bind(1, fleet_mission_id);
    delete_plans.run();
    Statement delete_states(db_, "DELETE FROM fleet_mission_ward_states WHERE fleet_mission_id = ?;");
    delete_states.bind(1, fleet_mission_id);
    delete_states.run();

    Statement insert_plan(
        db_, "INSERT INTO fleet_mission_ward_plans (fleet_mission_id, ward_id, plan_blob) VALUES (?, ?, ?);");
    Statement insert_state(db_,
                            "INSERT INTO fleet_mission_ward_states "
                            "(fleet_mission_id, ward_id, status, message, mission_id) "
                            "VALUES (?, ?, ?, '', '');");
    for (const auto& plan : ward_plans) {
        insert_plan.bind(1, fleet_mission_id);
        insert_plan.bind(2, plan.ward_id());
        insert_plan.bind_blob(3, plan.SerializeAsString());
        insert_plan.run();
        sqlite3_reset(insert_plan.stmt_);

        insert_state.bind(1, fleet_mission_id);
        insert_state.bind(2, plan.ward_id());
        insert_state.bind(3, static_cast<int>(karshipta::v1::WARD_MISSION_STATUS_UNSPECIFIED));
        insert_state.run();
        sqlite3_reset(insert_state.stmt_);
    }
    txn.commit();
    return std::nullopt;
}

std::optional<std::string> FleetMissionStore::delete_fleet_mission(const std::string& fleet_mission_id) {
    std::lock_guard lock(mutex_);
    Statement remove(db_, "DELETE FROM fleet_missions WHERE fleet_mission_id = ?;");
    remove.bind(1, fleet_mission_id);
    remove.run();
    if (sqlite3_changes(db_) == 0) {
        return "unknown fleet_mission_id: " + fleet_mission_id;
    }
    return std::nullopt;
}
