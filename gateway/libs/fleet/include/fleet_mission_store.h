#ifndef KARSHIPTA_GATEWAY_FLEET_MISSION_STORE_H
#define KARSHIPTA_GATEWAY_FLEET_MISSION_STORE_H

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <karshipta/v1/fleet.pb.h>

struct sqlite3;  // opaque; keeps sqlite3.h out of every consumer's include path

// Owns the SQLite-backed persistence for FleetMission: a trackable,
// operator-visible unit grouping one independently-planned route per ward
// (fleet-mission-model.md; replaces an earlier flat-broadcast design that
// sent the identical route to every ward at once, a real collision hazard).
// A sibling to FleetZoneStore, not an extension of it: FleetZoneStore's own
// header comment scopes it to exactly Fleet and Zone.
//
// RAII: opens the database and ensures the schema exists in the constructor,
// closes in the destructor. create_fleet_mission's row + per-ward-plan +
// per-ward-state inserts are wrapped in one SQLite transaction, same
// reasoning as FleetZoneStore::create_zone's zone-row + vertex-rows
// transaction: a mid-operation failure must never leave a FleetMission
// committed with a partial or missing per-ward state.
//
// Every public method takes mutex_ for its entire body - see
// FleetZoneStore's class comment for why this is coarse-grained and
// deliberate, not an oversight.
class FleetMissionStore {
   public:
    // path: the SQLite database file. ":memory:" works for tests.
    // Throws std::runtime_error if the database cannot be opened or the
    // schema cannot be created, same convention as FleetZoneStore.
    explicit FleetMissionStore(std::filesystem::path path);
    ~FleetMissionStore();

    FleetMissionStore(const FleetMissionStore&) = delete;
    FleetMissionStore& operator=(const FleetMissionStore&) = delete;
    FleetMissionStore(FleetMissionStore&&) = delete;
    FleetMissionStore& operator=(FleetMissionStore&&) = delete;

    // Generates a fresh fleet_mission_id and persists the mission, its
    // per-ward plans, and one WardMissionState per plan (status left at
    // WARD_MISSION_STATUS_UNSPECIFIED; the caller sets real per-ward status
    // once dispatch_mission_upload_and_start's outcome is known - see
    // update_ward_state below). Aggregate status starts at
    // FLEET_MISSION_STATUS_ACTIVE.
    std::string create_fleet_mission(const std::string& fleet_id, const std::string& mission_name,
                                      uint32_t repeat_count,
                                      const std::vector<karshipta::v1::WardMissionPlan>& ward_plans);
    [[nodiscard]] std::optional<karshipta::v1::FleetMission> get_fleet_mission(
        const std::string& fleet_mission_id) const;
    [[nodiscard]] std::vector<karshipta::v1::FleetMission> list_fleet_missions() const;
    // Updates one ward's current state in place (not an append-only
    // history - the operator wants current status per ward, not an audit
    // trail). Error string if fleet_mission_id is unknown or ward_id was
    // never part of this mission's plans; nullopt on success.
    std::optional<std::string> update_ward_state(const std::string& fleet_mission_id,
                                                  const karshipta::v1::WardMissionState& state);
    std::optional<std::string> set_status(const std::string& fleet_mission_id,
                                           karshipta::v1::FleetMissionStatus status);
    // Replaces every ward_plan for this mission (Edit) and resets every
    // ward_state back to UNSPECIFIED, mirroring create_fleet_mission's
    // initial state - the caller re-runs the same upload-and-start loop
    // afterward, same as for a fresh create.
    std::optional<std::string> update_ward_plans(
        const std::string& fleet_mission_id, const std::string& mission_name, uint32_t repeat_count,
        const std::vector<karshipta::v1::WardMissionPlan>& ward_plans);
    std::optional<std::string> delete_fleet_mission(const std::string& fleet_mission_id);

   private:
    void create_schema();

    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
};

#endif  // KARSHIPTA_GATEWAY_FLEET_MISSION_STORE_H
