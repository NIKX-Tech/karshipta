#include "mission_importer.h"

#include <mavsdk/mavlink/ardupilotmega/ardupilotmega.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <sstream>

namespace {

// Maps a raw MAV_CMD onto the one action our schema's per-item MissionAction
// can represent it as. Anything else (surveys, camera/gimbal actions,
// DO_CHANGE_SPEED, ...) has no MissionItem field to carry it, so it's
// unsupported rather than silently dropped.
std::optional<karshipta::v1::MissionAction> to_mission_action(const std::uint32_t command) {
    switch (command) {
        case MAV_CMD_NAV_WAYPOINT:
            return karshipta::v1::MISSION_ACTION_WAYPOINT;
        case MAV_CMD_NAV_TAKEOFF:
            return karshipta::v1::MISSION_ACTION_TAKEOFF;
        case MAV_CMD_NAV_LAND:
            return karshipta::v1::MISSION_ACTION_LAND;
        case MAV_CMD_NAV_RETURN_TO_LAUNCH:
            return karshipta::v1::MISSION_ACTION_RTL;
        case MAV_CMD_NAV_LOITER_UNLIM:
        case MAV_CMD_NAV_LOITER_TIME:
            return karshipta::v1::MISSION_ACTION_HOLD;
        default:
            return std::nullopt;
    }
}

// True for the two frames GeoPoint.altitude_rel_m can actually represent.
// MAV_FRAME_GLOBAL/_INT (MSL altitude) are rejected rather than reinterpreted,
// since guessing wrong here means the vehicle flies to the wrong altitude.
bool is_relative_altitude_frame(const std::uint32_t frame) {
    return frame == MAV_FRAME_GLOBAL_RELATIVE_ALT || frame == MAV_FRAME_GLOBAL_RELATIVE_ALT_INT;
}

// Not a real UUID: the schema comments mission_id as "uuid" but nothing
// validates that format, and this only needs to be unique enough for one
// gateway process to tell imported missions apart.
std::string synthesize_mission_id(const std::string& vehicle_id) {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return "imported-" + vehicle_id + "-" + std::to_string(now_ms);
}

}  // namespace

std::string MissionImporter::result_name(const mavsdk::MissionRaw::Result result) {
    std::ostringstream stream;
    stream << result;
    return stream.str();
}

MissionImporter::MissionImporter(VehicleConnection& connection) : connection_(connection) {}

bool MissionImporter::ensure_mission_raw() const {
    std::lock_guard lock(init_mutex_);
    if (mission_raw_) return true;
    if (!connection_.is_connected()) return false;
    mavsdk_keepalive_ = connection_.get_mavsdk();
    mission_raw_ = std::make_unique<mavsdk::MissionRaw>(connection_.get_system());
    spdlog::info("mission_raw plugin created");
    return true;
}

std::pair<std::optional<karshipta::v1::Mission>, std::string> MissionImporter::import(
    const karshipta::v1::MissionFileUpload& upload) const {
    if (upload.format() == karshipta::v1::MISSION_FILE_FORMAT_UNSPECIFIED) {
        return {std::nullopt, "mission file format unspecified"};
    }
    if (!ensure_mission_raw()) {
        return {std::nullopt, "mission file import failed: vehicle not connected"};
    }

    mavsdk::MissionRaw::Result result;
    mavsdk::MissionRaw::MissionImportData data;
    switch (upload.format()) {
        case karshipta::v1::MISSION_FILE_FORMAT_QGC_PLAN: {
            auto [import_result, import_data] =
                mission_raw_->import_qgroundcontrol_mission_from_string(upload.raw_content());
            result = import_result;
            data = std::move(import_data);
            break;
        }
        case karshipta::v1::MISSION_FILE_FORMAT_MISSION_PLANNER_WPL: {
            auto [import_result, import_data] =
                mission_raw_->import_mission_planner_mission_from_string(upload.raw_content());
            result = import_result;
            data = std::move(import_data);
            break;
        }
        case karshipta::v1::MISSION_FILE_FORMAT_UNSPECIFIED:
        default:
            return {std::nullopt, "mission file format unspecified"};
    }
    if (result != mavsdk::MissionRaw::Result::Success) {
        spdlog::error("mission file import rejected for {}: {}", upload.vehicle_id(),
                      result_name(result));
        return {std::nullopt, "failed to parse mission file: " + result_name(result)};
    }
    if (data.mission_items.empty()) {
        return {std::nullopt, "imported mission has no items"};
    }

    karshipta::v1::Mission mission;
    mission.set_mission_id(synthesize_mission_id(upload.vehicle_id()));
    mission.set_vehicle_id(upload.vehicle_id());

    for (const auto& raw : data.mission_items) {
        const auto action = to_mission_action(raw.command);
        if (!action) {
            return {std::nullopt, "unsupported command " + std::to_string(raw.command) +
                                       " in imported mission at seq " + std::to_string(raw.seq)};
        }
        if (!is_relative_altitude_frame(raw.frame)) {
            return {std::nullopt, "unsupported coordinate frame " + std::to_string(raw.frame) +
                                       " in imported mission at seq " + std::to_string(raw.seq)};
        }
        auto* item = mission.add_items();
        item->set_seq(raw.seq);
        item->set_action(*action);
        auto* position = item->mutable_position();
        // x/y are latitude/longitude in degrees, scaled by 1e7 (int32_t);
        // z is altitude in meters, already unscaled (MissionRaw::MissionItem's
        // own field comments).
        position->set_latitude_deg(static_cast<double>(raw.x) / 1e7);
        position->set_longitude_deg(static_cast<double>(raw.y) / 1e7);
        position->set_altitude_rel_m(raw.z);
        // param1 is hold/loiter time for WAYPOINT and LOITER_TIME, unused
        // (0) for TAKEOFF/LAND/RTL/LOITER_UNLIM.
        item->set_hold_time_s(raw.param1);
        // param2 is acceptance radius for WAYPOINT, unused (0) elsewhere.
        item->set_acceptance_radius_m(raw.param2);
    }
    return {std::move(mission), ""};
}
