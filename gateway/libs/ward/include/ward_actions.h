#ifndef KARSHIPTA_GATEWAY_WARD_ACTIONS_H
#define KARSHIPTA_GATEWAY_WARD_ACTIONS_H

#include <mavsdk/plugins/action/action.h>
#include <mavsdk/plugins/mavlink_passthrough/mavlink_passthrough.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "ward_connection.h"

// Wraps MAVSDK's Action plugin (plus MavlinkPassthrough for the one command
// Action doesn't cover, see set_home_position below) against exactly one
// connected ward. Scoped to precisely the commands
// proto/karshipta/v1/command.proto's Command.action oneof can carry
// (ArmCommand, DisarmCommand, TakeoffCommand, LandCommand,
// ReturnToLaunchCommand, GotoCommand, SetHomeCommand); nothing here exists
// ahead of a schema field for it.
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

    // Serializes the lazy init in ensure_action()/ensure_mavlink_passthrough():
    // both the CommandExecutor worker and WardManager's own threads can
    // trigger the first creation concurrently. Once created, action_/
    // mavlink_passthrough_ are never reset, so reads after a successful
    // ensure_*() need no lock. Shared between both: they're never
    // constructed concurrently with each other either.
    mutable std::mutex init_mutex_;

    // Lazily creates `action_` the first time it's needed. Returns false if
    // `connection_` isn't connected yet; returns true immediately if already created.
    // Thread-safe.
    bool ensure_action() const;
    // Lazily creates `mavlink_passthrough_`, same contract as ensure_action().
    bool ensure_mavlink_passthrough() const;
    // Logs the outcome of a command already sent to `action_` and passes the
    // Result through unchanged, so the caller (eventually the M3 CommandAck
    // path) keeps the reason a bool would have erased.
    static mavsdk::Action::Result log_result(const std::string& label,
                                             mavsdk::Action::Result result);
    // Overload for MavlinkPassthrough::Result, same contract as log_result above.
    static mavsdk::MavlinkPassthrough::Result log_result(const std::string& label,
                                                          mavsdk::MavlinkPassthrough::Result result);
};

#endif  // KARSHIPTA_GATEWAY_WARD_ACTIONS_H
