#ifndef KARSHIPTA_GATEWAY_FLEET_MANAGER_H
#define KARSHIPTA_GATEWAY_FLEET_MANAGER_H

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include <karshipta/v1/envelope.pb.h>
#include <karshipta/v1/fleet.pb.h>

#include <fleet_mission_store.h>
#include <fleet_zone_store.h>
#include <transport.h>

// Forward-declared, not included: only handle_create_fleet_mission(),
// handle_stop_fleet_mission(), and handle_update_fleet_mission_routes()
// below actually need WardManager (to dispatch mission uploads/commands to
// it), and those methods only exist in KARSHIPTA_GATEWAY_ENABLE_MAVLINK=ON
// builds (see root CMakeLists.txt) - every other Fleet/Zone/FleetMission
// path here (including removing a FleetMission, which only touches
// persisted tracking rows, never live flight state) is source-agnostic and
// must stay MAVSDK-free so a Herald-only build never links it in.
class WardManager;

// Owns Fleet/Zone persistence (via FleetZoneStore), FleetMission persistence
// (via FleetMissionStore), and the wire-level request/ack translation for
// all three - the Fleet/Zone/FleetMission counterpart to WardManager's
// handle_add_ward/handle_remove_ward. Kept as its own class rather than
// folded into WardManager: these are their own resources with their own
// stores, and WardManager already owns enough (gateway/CLAUDE.md rule 3, one
// clear owner per resource). The one place the two managers meet is the
// three MAVLink-gated FleetMission handlers above, each handed a
// WardManager& per call rather than storing one, since dispatching uploads/
// commands to wards is the only thing FleetManager ever needs from it.
//
// handle_command_outcome()/handle_mission_upload_outcome() are the other
// direction of that meeting point: WardManager calls back into FleetManager
// (via the observers main.cpp registers on it, see
// WardManager::set_command_outcome_observer/set_mission_upload_outcome_observer)
// so a fleet mission's per-ward WardMissionState can track a Stop dispatch
// through to STOPPED, and a Create/Edit's upload through to ACTIVE, without
// WardManager needing to know FleetMission exists.
class FleetManager {
   public:
    // db_path/fleet_mission_db_path are forwarded straight to
    // FleetZoneStore/FleetMissionStore respectively (see their own headers
    // for the ":memory:" test convenience); production always passes real,
    // distinct paths, mirroring WardManager's persistence_path convention.
    FleetManager(Transport& transport, std::filesystem::path db_path,
                 std::filesystem::path fleet_mission_db_path);

    FleetManager(const FleetManager&) = delete;
    FleetManager& operator=(const FleetManager&) = delete;
    FleetManager(FleetManager&&) = delete;
    FleetManager& operator=(FleetManager&&) = delete;

    // Wire-level entry points: translate a request from a console client
    // into a store mutation and the Ack to send back. Never throw on bad
    // input; every rejection carries a reason (gateway rule 5). On success,
    // create/rename/membership-change handlers also broadcast the affected
    // Fleet/Zone to every connected client (not just the requester), so
    // state stays in sync across more than one open console; delete has no
    // updated object to send, so the caller (main.cpp) must broadcast the
    // returned Ack itself to every client, same as it already does for
    // WardConfigAck.
    [[nodiscard]] karshipta::v1::FleetAck handle_create_fleet(
        const karshipta::v1::CreateFleet& request);
    [[nodiscard]] karshipta::v1::FleetAck handle_rename_fleet(
        const karshipta::v1::RenameFleet& request);
    [[nodiscard]] karshipta::v1::FleetAck handle_delete_fleet(
        const karshipta::v1::DeleteFleet& request);
    [[nodiscard]] karshipta::v1::FleetAck handle_add_ward_to_fleet(
        const karshipta::v1::AddWardToFleet& request);
    [[nodiscard]] karshipta::v1::FleetAck handle_remove_ward_from_fleet(
        const karshipta::v1::RemoveWardFromFleet& request);

    [[nodiscard]] karshipta::v1::ZoneAck handle_create_zone(
        const karshipta::v1::CreateZone& request);
    [[nodiscard]] karshipta::v1::ZoneAck handle_update_zone(
        const karshipta::v1::UpdateZone& request);
    [[nodiscard]] karshipta::v1::ZoneAck handle_delete_zone(
        const karshipta::v1::DeleteZone& request);

#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
    // Persists the FleetMission, then for each ward_plans() entry builds
    // that ward's own independent Mission (fresh mission_id, that ward's own
    // items, shared repeat_count - the actual fix over the old flat-broadcast
    // FleetMissionAssignment: no two wards ever share a route) and calls
    // ward_manager.dispatch_mission_upload_and_start(). That call's return
    // (nullopt = accepted, a reason = immediate rejection) becomes the
    // ward's initial WardMissionState (UPLOADING or REJECTED); the eventual
    // real upload outcome arrives later via handle_mission_upload_outcome().
    // ACCEPTED here means "the FleetMission was created," not "every ward's
    // upload succeeded" (mirrors AddWard's ack meaning "config accepted,"
    // not "connection succeeded") - per-ward outcomes live in ward_states().
    [[nodiscard]] karshipta::v1::FleetMissionAck handle_create_fleet_mission(
        const karshipta::v1::CreateFleetMission& request, WardManager& ward_manager);

    // Looks up the FleetMission, defaults an unspecified action to RTL, and
    // for every ward not already STOPPED/REJECTED/STOPPING, dispatches the
    // corresponding Command (rtl/pause_mission/land) via
    // ward_manager.dispatch_command(), setting that ward's WardMissionState
    // to STOPPING immediately. The real outcome (STOPPED, or reverted to
    // ACTIVE on a rejected/timed-out stop) arrives later via
    // handle_command_outcome() once that ward's CommandAck settles.
    [[nodiscard]] karshipta::v1::FleetMissionAck handle_stop_fleet_mission(
        const karshipta::v1::StopFleetMission& request, WardManager& ward_manager);

    // Re-plans routes for an existing FleetMission ("Edit"). Rejects unless
    // every ward_state is STOPPED/REJECTED (never let a route change land
    // under an active mission - the same hazard this whole redesign
    // targets), otherwise replaces every ward_plan via
    // fleet_mission_store_.update_ward_plans() and re-runs the same
    // upload-and-start loop handle_create_fleet_mission() uses: submitting
    // an edit re-plans and re-dispatches, it does not just save a draft.
    [[nodiscard]] karshipta::v1::FleetMissionAck handle_update_fleet_mission_routes(
        const karshipta::v1::UpdateFleetMissionRoutes& request, WardManager& ward_manager);
#endif

    // Safety-gated: rejects unless every ward_state.status() is
    // STOPPED/REJECTED (never started). Deliberately does NOT need
    // WardManager - unlike WardManager::remove_ward_impl's live MAVSDK
    // re-verification, deleting a FleetMission record only touches
    // persisted tracking rows, never a ward's connection or flight state;
    // the safety-critical check already happened inside
    // handle_stop_fleet_mission's dispatch_command calls. No updated object
    // to broadcast on success (the mission is gone): main.cpp broadcasts
    // the returned ack itself, same pattern as handle_delete_fleet.
    [[nodiscard]] karshipta::v1::FleetMissionAck handle_remove_fleet_mission(
        const karshipta::v1::RemoveFleetMission& request);

    // Rejects an upstream Fleet/Zone/FleetMission envelope from a connection
    // Transport marked ClientRole::kViewer (gateway issue #20). Mirrors
    // WardManager::reject_viewer_envelope, scoped to exactly the payload
    // kinds this class owns; main.cpp routes each upstream kind to whichever
    // of the two reject_viewer_envelope methods actually owns it.
    void reject_viewer_envelope(Transport::ClientId client, const karshipta::v1::Envelope& envelope);

    // Sends one Fleet envelope per fleet and one Zone envelope per zone to
    // exactly this client. Wire this to Transport::on_connect alongside
    // WardManager::send_ward_info, so a client that connects after boot
    // still learns the gateway's persisted Fleet/Zone state.
    void send_fleet_zone_snapshot(Transport::ClientId client) const;

    // Sends one FleetMission envelope per persisted mission to exactly this
    // client. Separate from send_fleet_zone_snapshot() (its own method in
    // the plan this class follows), wired alongside it in main.cpp's
    // on_connect callback.
    void send_fleet_mission_snapshot(Transport::ClientId client) const;

    // Registered by main.cpp as WardManager's command-outcome observer (see
    // WardManager::set_command_outcome_observer). Only acts on a terminal
    // CommandAck (SUCCESS/REJECTED/TIMEOUT - ACCEPTED/EXECUTING are ignored,
    // not yet a final outcome) whose command_id matches a Stop dispatch this
    // class itself issued (tracked in pending_stops_); anything else is
    // silently ignored, since most CommandAcks have nothing to do with a
    // fleet mission at all. No-op if the correlated FleetMission or ward was
    // since removed.
    void handle_command_outcome(const karshipta::v1::CommandAck& ack);

    // Registered by main.cpp as WardManager's mission-upload-outcome
    // observer (see WardManager::set_mission_upload_outcome_observer).
    // Scans persisted FleetMissions for the one whose ward_state for
    // ward_id is UPLOADING with this exact mission_id, and flips it to
    // ACTIVE (success) or REJECTED (failure, message carries the reason).
    // A linear scan, not a lookup, since a mission_id alone doesn't say
    // which FleetMission it belongs to; fine at this scale, same reasoning
    // as find_fleet()/find_zone() below.
    void handle_mission_upload_outcome(const std::string& ward_id, const std::string& mission_id,
                                        bool success, const std::string& message);

   private:
    Transport& transport_;
    FleetZoneStore store_;
    FleetMissionStore fleet_mission_store_;

    // Correlates a Stop dispatch's synthesized Command.command_id back to
    // the (fleet_mission_id, ward_id) it was issued for, so
    // handle_command_outcome() can update the right WardMissionState.
    // Populated by handle_stop_fleet_mission() before calling
    // ward_manager.dispatch_command() (so even a same-thread synchronous
    // rejection ack, delivered through the observer before dispatch_command
    // returns, finds its entry already there), consumed (erased) by
    // handle_command_outcome() on the first terminal ack. Guarded by its own
    // mutex, not store_'s or fleet_mission_store_'s: this map is
    // FleetManager-only bookkeeping, not persisted state.
    struct PendingStop {
        std::string fleet_mission_id;
        std::string ward_id;
    };
    mutable std::mutex pending_stops_mutex_;
    std::map<std::string, PendingStop> pending_stops_;

    // Wraps fleet in an Envelope.fleet and broadcasts it to every connected
    // client, so a create/rename/membership change is visible to consoles
    // other than the one that requested it, not just the requester's own ack.
    void broadcast_fleet(const karshipta::v1::Fleet& fleet) const;
    // Same, for a created/updated Zone.
    void broadcast_zone(const karshipta::v1::Zone& zone) const;
    // Same, for a created/updated FleetMission.
    void broadcast_fleet_mission(const karshipta::v1::FleetMission& mission) const;
    // Wraps a gateway-level (ward_id empty) WARNING Event and broadcasts it -
    // used for a whole-request rejection that has no single ward to attach
    // the failure to.
    void broadcast_gateway_warning(const std::string& code, const std::string& message) const;

    // Looks up one Fleet/Zone by id via the store's list_* methods (linear
    // scan; fine at this scale - FleetZoneStore intentionally has no
    // single-lookup method, see its own header). Used to build the
    // post-mutation broadcasts above without adding a getter to the store
    // just for this.
    [[nodiscard]] std::optional<karshipta::v1::Fleet> find_fleet(const std::string& fleet_id) const;
    [[nodiscard]] std::optional<karshipta::v1::Zone> find_zone(const std::string& zone_id) const;

    // If fleet_mission_id's aggregate status is STOPPING and every ward's
    // status has settled to STOPPED or REJECTED, flips the aggregate to
    // STOPPED. Called after handle_command_outcome() updates one ward's
    // state; a no-op (not an error) if the mission is unknown, not
    // STOPPING, or still has an unsettled ward.
    void maybe_finalize_stop(const std::string& fleet_mission_id);
};

#endif  // KARSHIPTA_GATEWAY_FLEET_MANAGER_H
