#ifndef KARSHIPTA_GATEWAY_WARD_ACTIONS_H
#define KARSHIPTA_GATEWAY_WARD_ACTIONS_H

#include <mavsdk/plugins/action/action.h>
#include <mavsdk/plugins/geofence/geofence.h>
#include <mavsdk/plugins/mavlink_passthrough/mavlink_passthrough.h>
#include <mavsdk/plugins/param/param.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "ward_connection.h"

// Wraps MAVSDK's Action plugin (plus MavlinkPassthrough for the one command
// Action doesn't cover, see set_home_position below, Geofence for
// upload_geofence, and Param for get_all_params/get_param_*/set_param_*)
// against exactly one connected ward. Scoped to precisely the commands
// proto/karshipta/v1/command.proto's Command.action oneof can carry
// (ArmCommand, DisarmCommand, TakeoffCommand, LandCommand,
// ReturnToLaunchCommand, GotoCommand, SetHomeCommand), plus the
// WardManager-driven geofence push (karshipta#247) and parameter access
// (karshipta#211, also WardManager-driven, not Command-shaped - see
// GetParams/SetParam below) - nothing here exists ahead of a schema field
// or a documented, scoped feature for it.
class WardActions {
   public:
    // Binds this wrapper to a connection; does not create the Action plugin yet
    // (that happens lazily in ensure_action() on first use).
    explicit WardActions(WardConnection& connection);

    // Human-readable text for a MAVSDK result (via its operator<<), the string
    // that CommandAck.message carries on rejection.
    static std::string result_name(mavsdk::Action::Result result);
    // Overload for set_home_position's own Result type (MavlinkPassthrough
    // has no relation to Action::Result - different plugin, different enum).
    static std::string result_name(mavsdk::MavlinkPassthrough::Result result);
    // Overload for upload_geofence's own Result type. Unlike
    // MavlinkPassthrough::Result, Geofence::Result's operator<< is actually
    // exported by the linked MAVSDK build (confirmed), so this one uses the
    // same fmt::streamed-via-ostringstream pattern as Action::Result instead
    // of a hand-written switch.
    static std::string result_name(mavsdk::Geofence::Result result);
    // Overload for Param's own Result type. Its operator<< is exported
    // (confirmed, same as Geofence::Result) - normal pattern. Carries much
    // more specific reasons than Action::Result (DoesNotExist,
    // ValueOutOfRange, PermissionDenied, ReadOnly, TypeMismatch, ...);
    // WardManager must surface this text as-is in ParamAck.message, never
    // collapse it to a bare "rejected" (karshipta#211).
    static std::string result_name(mavsdk::Param::Result result);

    // Implements ArmCommand. Arms the ward (motors become live). Returns a
    // failure Result if failsafe is active or pre-arm checks have not passed.
    [[nodiscard]] mavsdk::Action::Result arm() const;
    // Implements DisarmCommand{force: false}. The autopilot rejects this while
    // flying; only a landed ward actually disarms.
    [[nodiscard]] mavsdk::Action::Result disarm() const;
    // Reads the configured takeoff altitude (meters above ground). Lazily
    // creates the Action plugin via ensure_action() if needed; returns
    // Result::Failed with 0.0f if the connection isn't connected yet.
    [[nodiscard]] std::pair<mavsdk::Action::Result, float> get_takeoff_altitude() const;
    // Sets the takeoff altitude (meters above ground) used by takeoff();
    // backs TakeoffCommand.altitude_rel_m.
    [[nodiscard]] mavsdk::Action::Result set_takeoff_altitude(float new_altitude) const;
    // Implements TakeoffCommand. Commands takeoff to the configured takeoff
    // altitude. Must be armed first.
    [[nodiscard]] mavsdk::Action::Result takeoff() const;
    // Implements LandCommand. Commands landing at the current position.
    [[nodiscard]] mavsdk::Action::Result land() const;
    // Sets the RTL altitude (meters above home) used by return_to_launch();
    // backs ReturnToLaunchCommand.altitude_rel_m. Same shape as
    // set_takeoff_altitude() above - a separate FC-persisted value,
    // return_to_launch() itself takes no argument.
    [[nodiscard]] mavsdk::Action::Result set_return_to_launch_altitude(float new_altitude) const;
    // Implements ReturnToLaunchCommand. Flies back to the home position and lands.
    [[nodiscard]] mavsdk::Action::Result return_to_launch() const;
    // Manual hold: loiters at the current GPS position and altitude,
    // independent of any mission. Backs PauseMissionCommand only when the
    // ward isn't currently flying an uploaded mission (CommandExecutor
    // checks flight mode and calls Mission::pause_mission() instead when it
    // is, which is mission-aware and keeps the mission resumable; this is not
    // that). PX4-specific flight mode; not guaranteed on other autopilots.
    [[nodiscard]] mavsdk::Action::Result hold() const;
    // Implements GotoCommand. Commands the ward to GotoCommand.target
    // (WGS84 lat/lon, AMSL altitude in meters); yaw_deg is NED yaw in degrees.
    [[nodiscard]] mavsdk::Action::Result goto_location(double latitude_deg, double longitude_deg,
                                                       float absolute_altitude_m,
                                                       float yaw_deg) const;
    // Implements DisarmCommand{force: true}. Disarms immediately regardless of
    // landed state; the ward will fall out of the sky if used while
    // flying. The M3 executor must gate this on the force flag and must never
    // call it for a plain DisarmCommand{force: false}.
    [[nodiscard]] mavsdk::Action::Result kill() const;
    // Backs GotoCommand.speed_m_s: sets the current speed (m/s) used for
    // missions/repositioning. Ephemeral, not stored on the ward.
    [[nodiscard]] mavsdk::Action::Result set_current_speed(float speed_m_s) const;
    // Implements SetHomeCommand. Sends a raw MAV_CMD_DO_SET_HOME (param1=0,
    // "use specified location") - no higher-level MAVSDK plugin wraps this;
    // Action has set_return_to_launch_altitude() (the RTL climb height) but
    // nothing for the RTL location itself (karshipta#246). MavlinkPassthrough
    // is flagged for deprecation in MAVSDK v4 in favor of MavlinkDirect; a
    // conscious choice for now since it's the stable, documented path to a
    // single COMMAND_LONG.
    [[nodiscard]] mavsdk::MavlinkPassthrough::Result set_home_position(
        double latitude_deg, double longitude_deg, float altitude_msl_m) const;
    // Pushes a horizontal-only geofence (karshipta#247 - MAVSDK's Geofence
    // plugin has no altitude field anywhere in its API, checked the full
    // header; altitude bounds stay advisory, tracked separately as
    // karshipta#251). Not tied to a Command - called by WardManager, not
    // CommandExecutor, on every zone mutation and on this ward's own
    // (re)connect, mirroring MAV_CMD geofence upload's own full-replace
    // semantics: the complete current zone set is always re-sent, never an
    // incremental diff.
    [[nodiscard]] mavsdk::Geofence::Result upload_geofence(
        const mavsdk::Geofence::GeofenceData& data) const;
    // Implements GetParams (karshipta#211). Reads every int and float
    // parameter currently known to the autopilot. Unlike every other
    // MAVSDK call this class wraps, get_all_params() has no Result/failure
    // signal of its own - it returns whatever it collected. Returns an
    // empty AllParams if the connection isn't up yet (checked via
    // ensure_param(), same contract as every other ensure_*() here); a
    // genuine on-wire timeout or partial response is likewise
    // indistinguishable from "the vehicle has no parameters" at this
    // layer - WardManager must still check connection state itself before
    // trusting an empty result as real (see its own doc comment).
    [[nodiscard]] mavsdk::Param::AllParams get_all_params() const;
    // Reads one int-valued parameter by its raw MAVLink PARAM_ID. Used by
    // WardManager::handle_set_param() to read back the vehicle-confirmed
    // value after a successful set_param_int() - MAVSDK's setter returns
    // only a Result, not the value the vehicle actually stored, so an
    // honest "confirmed" value needs this explicit round trip.
    [[nodiscard]] std::pair<mavsdk::Param::Result, int32_t> get_param_int(
        const std::string& name) const;
    // Float counterpart to get_param_int(), same contract.
    [[nodiscard]] std::pair<mavsdk::Param::Result, float> get_param_float(
        const std::string& name) const;
    // Implements SetParam for an int-valued parameter (karshipta#211).
    // MAVSDK has no single polymorphic setter - get/set_param_int and
    // get/set_param_float are separate calls, each returning
    // Result::WrongType if name actually holds the other type; the caller
    // (WardManager) picks the right one from ParamType.
    [[nodiscard]] mavsdk::Param::Result set_param_int(const std::string& name,
                                                       int32_t value) const;
    // Float counterpart to set_param_int(), same contract.
    [[nodiscard]] mavsdk::Param::Result set_param_float(const std::string& name,
                                                         float value) const;

   private:
    // The connection this action wrapper sends commands through. Must outlive this object.
    WardConnection& connection_;
    // Owned copy of the Mavsdk core, grabbed in ensure_action(). Keeps the core alive
    // for as long as `action_` exists, independent of `connection_`'s own lifetime.
    mutable std::shared_ptr<mavsdk::Mavsdk> mavsdk_keepalive_;
    // The Action plugin bound to the connected System. Null until ensure_action()
    // lazily constructs it on first use.
    mutable std::unique_ptr<mavsdk::Action> action_;
    // The MavlinkPassthrough plugin, lazily constructed the same way as
    // action_ (see ensure_mavlink_passthrough()) - only set_home_position
    // uses this; every other method stays on the Action plugin above.
    mutable std::unique_ptr<mavsdk::MavlinkPassthrough> mavlink_passthrough_;
    // The Geofence plugin, lazily constructed the same way as action_ (see
    // ensure_geofence()) - only upload_geofence uses this.
    mutable std::unique_ptr<mavsdk::Geofence> geofence_;
    // The Param plugin, lazily constructed the same way as action_ (see
    // ensure_param()) - only get_all_params/get_param_*/set_param_* use this.
    mutable std::unique_ptr<mavsdk::Param> param_;

    // Serializes the lazy init in ensure_action()/ensure_mavlink_passthrough()/
    // ensure_geofence()/ensure_param(): the CommandExecutor worker,
    // WardManager's own zone-push and parameter calls, and WardManager's
    // reconnect_worker threads can all trigger a first creation
    // concurrently. Once created, action_/mavlink_passthrough_/geofence_/
    // param_ are never reset, so reads after a successful ensure_*() need
    // no lock. Shared across all four: none of them are ever constructed
    // concurrently with each other either.
    mutable std::mutex init_mutex_;

    // Lazily creates `action_` the first time it's needed. Returns false if
    // `connection_` isn't connected yet; returns true immediately if already created.
    // Thread-safe.
    bool ensure_action() const;
    // Lazily creates `mavlink_passthrough_`, same contract as ensure_action().
    bool ensure_mavlink_passthrough() const;
    // Lazily creates `geofence_`, same contract as ensure_action().
    bool ensure_geofence() const;
    // Lazily creates `param_`, same contract as ensure_action().
    bool ensure_param() const;
    // Logs the outcome of a command already sent to `action_` and passes the
    // Result through unchanged, so the caller (eventually the M3 CommandAck
    // path) keeps the reason a bool would have erased.
    static mavsdk::Action::Result log_result(const std::string& label,
                                             mavsdk::Action::Result result);
    // Overload for MavlinkPassthrough::Result, same contract as log_result above.
    static mavsdk::MavlinkPassthrough::Result log_result(const std::string& label,
                                                          mavsdk::MavlinkPassthrough::Result result);
    // Overload for Geofence::Result, same contract as log_result above.
    static mavsdk::Geofence::Result log_result(const std::string& label,
                                                mavsdk::Geofence::Result result);
    // Overload for Param::Result, same contract as log_result above.
    static mavsdk::Param::Result log_result(const std::string& label,
                                             mavsdk::Param::Result result);
};

#endif  // KARSHIPTA_GATEWAY_WARD_ACTIONS_H
