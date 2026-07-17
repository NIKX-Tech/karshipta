#ifndef KARSHIPTA_GATEWAY_MISSION_IMPORTER_H
#define KARSHIPTA_GATEWAY_MISSION_IMPORTER_H

#include <karshipta/v1/command.pb.h>
#include <mavsdk/plugins/mission_raw/mission_raw.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "vehicle_connection.h"

// Converts a customer-supplied mission file (proto/karshipta/v1/command.proto's
// MissionFileUpload: a QGroundControl .plan or Mission Planner WPL, sent as
// raw text) into an ordinary proto Mission, using MAVSDK's MissionRaw plugin
// only for its import parsers. Never used to upload, start, pause, or track
// progress; that stays VehicleMission's job against the Mission plugin, and
// this class's output is meant to be handed straight to
// VehicleMission::enqueue_upload() exactly like a console-editor-authored
// Mission would be. One instance per vehicle, same lazy-bind-to-a-connection
// pattern as VehicleActions/TelemetryInfo/VehicleMission.
class MissionImporter {
   public:
    // Binds this wrapper to a connection; does not create the MissionRaw
    // plugin yet (that happens lazily in ensure_mission_raw() on first use).
    explicit MissionImporter(VehicleConnection& connection);

    // Human-readable text for a MAVSDK MissionRaw result.
    static std::string result_name(mavsdk::MissionRaw::Result result);

    // Parses upload.raw_content() per upload.format(), maps every raw mission
    // item onto MissionAction, and returns a Mission ready for
    // VehicleMission::enqueue_upload(). Returns nullopt (with a
    // human-readable reason as the second element) instead of a Mission if:
    // the format is unspecified; MAVSDK's own parse fails (malformed file);
    // the import produced no items; any item's MAV_CMD has no MissionAction
    // equivalent (this schema only carries WAYPOINT/TAKEOFF/LAND/RTL/HOLD,
    // so e.g. a QGC survey pattern or an embedded speed-change command is
    // rejected, not silently dropped); or any item's coordinate frame isn't
    // one of the two relative-altitude global frames (MAV_FRAME_GLOBAL /
    // MAV_FRAME_GLOBAL_INT, i.e. MSL altitude, are rejected rather than
    // guessed, since GeoPoint only has altitude_rel_m for this path).
    //
    // Only does mapping-level validation (can this item be represented at
    // all); mission-level rules (non-empty vehicle_id, RTL only as the last
    // item) are VehicleMission::enqueue_upload()'s job via its own
    // validate_mission(), not duplicated here.
    //
    // Blocking, but on local text parsing, not a MAVLink round trip to the
    // vehicle; still requires a connected VehicleConnection, since MAVSDK's
    // MissionRaw plugin must be constructed against a discovered System even
    // though the parse itself doesn't need the vehicle armed, flying, or
    // otherwise reachable.
    [[nodiscard]] std::pair<std::optional<karshipta::v1::Mission>, std::string> import(
        const karshipta::v1::MissionFileUpload& upload) const;

   private:
    // The connection this importer binds its MissionRaw plugin through. Must
    // outlive this object.
    VehicleConnection& connection_;
    // Owned copy of the Mavsdk core, grabbed in ensure_mission_raw(). Keeps
    // the core alive for as long as `mission_raw_` exists, independent of
    // `connection_`'s own lifetime.
    mutable std::shared_ptr<mavsdk::Mavsdk> mavsdk_keepalive_;
    // The MissionRaw plugin bound to the connected System. Null until
    // ensure_mission_raw() lazily constructs it on first use.
    mutable std::unique_ptr<mavsdk::MissionRaw> mission_raw_;
    // Serializes the lazy init in ensure_mission_raw(). Once created,
    // `mission_raw_` is never reset, so reads after a successful
    // ensure_mission_raw() need no lock.
    mutable std::mutex init_mutex_;

    // Lazily creates `mission_raw_` the first time it's needed. Returns false
    // if `connection_` isn't connected yet; returns true immediately if
    // already created.
    bool ensure_mission_raw() const;
};

#endif  // KARSHIPTA_GATEWAY_MISSION_IMPORTER_H
