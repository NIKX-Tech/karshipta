#ifndef KARSHIPTA_GATEWAY_FLEET_MANAGER_H
#define KARSHIPTA_GATEWAY_FLEET_MANAGER_H

#include <filesystem>
#include <optional>
#include <string>

#include <karshipta/v1/envelope.pb.h>
#include <karshipta/v1/fleet.pb.h>

#include <fleet_zone_store.h>
#include <transport.h>
#include <ward_manager.h>

// Owns Fleet/Zone persistence (via FleetZoneStore) and the wire-level
// request/ack translation for them - the Fleet/Zone counterpart to
// WardManager's handle_add_ward/handle_remove_ward. Kept as its own class
// rather than folded into WardManager: Fleet/Zone are their own resource
// with their own store, and WardManager already owns enough (gateway/
// CLAUDE.md rule 3, one clear owner per resource). The one place the two
// managers meet is handle_fleet_mission_assignment(), which is handed a
// WardManager& per call rather than storing one, since fanning a mission out
// to wards is the only thing FleetManager ever needs from it.
class FleetManager {
   public:
    // db_path is forwarded straight to FleetZoneStore (see its own header
    // for the ":memory:" test convenience); production always passes a real
    // path, mirroring WardManager's persistence_path convention.
    FleetManager(Transport& transport, std::filesystem::path db_path);

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

    // Fans a fleet-wide mission out to each selected ward as an independent
    // upload + start (fleet.proto FleetMissionAssignment's comment: fresh
    // mission_id and ward_id per recipient, otherwise identical items/
    // repeat_count). No dedicated ack type exists for this request, mirroring
    // solo Envelope.mission_upload, which has none either: a whole-request
    // rejection (unknown fleet_id, empty ward_ids) becomes a gateway-level
    // WARNING Event (Event.ward_id empty, "gateway-level event" per its own
    // comment); each ward's own upload/start outcome surfaces through
    // ward_manager's existing CommandAck/Event channels, exactly as it would
    // for a solo mission.
    void handle_fleet_mission_assignment(const karshipta::v1::FleetMissionAssignment& request,
                                          WardManager& ward_manager);

    // Rejects an upstream Fleet/Zone/mission-assignment envelope from a
    // connection Transport marked ClientRole::kViewer (gateway issue #20).
    // Mirrors WardManager::reject_viewer_envelope, scoped to exactly the
    // payload kinds this class owns; main.cpp routes each upstream kind to
    // whichever of the two reject_viewer_envelope methods actually owns it.
    void reject_viewer_envelope(Transport::ClientId client, const karshipta::v1::Envelope& envelope);

    // Sends one Fleet envelope per fleet and one Zone envelope per zone to
    // exactly this client. Wire this to Transport::on_connect alongside
    // WardManager::send_ward_info, so a client that connects after boot
    // still learns the gateway's persisted Fleet/Zone state.
    void send_fleet_zone_snapshot(Transport::ClientId client) const;

   private:
    Transport& transport_;
    FleetZoneStore store_;

    // Wraps fleet in an Envelope.fleet and broadcasts it to every connected
    // client, so a create/rename/membership change is visible to consoles
    // other than the one that requested it, not just the requester's own ack.
    void broadcast_fleet(const karshipta::v1::Fleet& fleet) const;
    // Same, for a created/updated Zone.
    void broadcast_zone(const karshipta::v1::Zone& zone) const;
    // Wraps a gateway-level (ward_id empty) WARNING Event and broadcasts it -
    // used for whole-request FleetMissionAssignment rejections that have no
    // single ward to attach the failure to.
    void broadcast_gateway_warning(const std::string& code, const std::string& message) const;

    // Looks up one Fleet/Zone by id via the store's list_* methods (linear
    // scan; fine at this scale - FleetZoneStore intentionally has no
    // single-lookup method, see its own header). Used to build the
    // post-mutation broadcasts above without adding a getter to the store
    // just for this.
    [[nodiscard]] std::optional<karshipta::v1::Fleet> find_fleet(const std::string& fleet_id) const;
    [[nodiscard]] std::optional<karshipta::v1::Zone> find_zone(const std::string& zone_id) const;
};

#endif  // KARSHIPTA_GATEWAY_FLEET_MANAGER_H
