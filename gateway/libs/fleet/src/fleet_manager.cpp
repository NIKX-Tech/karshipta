#include "fleet_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

#include <spdlog/spdlog.h>

#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
#include <ward_manager.h>
#endif

namespace {

// Rejection reason for every upstream envelope from a viewer connection
// (gateway issue #20). Matches ward_manager.cpp's own copy verbatim; each
// file owns its constants rather than sharing a header for one string,
// following the repo's existing precedent (serialize_envelope below is
// duplicated the same way).
constexpr auto kReadOnlySessionMessage = "read-only session";

uint64_t unix_epoch_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                                      .count());
}

std::vector<uint8_t> serialize_envelope(const karshipta::v1::Envelope& envelope) {
    std::vector<uint8_t> bytes(envelope.ByteSizeLong());
    if (!envelope.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()))) {
        spdlog::error("failed to serialize an Envelope of {} bytes", bytes.size());
        bytes.clear();
    }
    return bytes;
}

// Finds ward_id's own WardMissionState within mission, or nullopt if this
// mission has no plan for that ward. Shared by handle_command_outcome() and
// handle_mission_upload_outcome(): both need to read-modify-write one ward's
// state without disturbing the others, and update_ward_state() overwrites
// status/message/mission_id together (FleetMissionStore has no per-field
// update), so the caller must start from the current row, not a fresh one.
std::optional<karshipta::v1::WardMissionState> find_ward_state(const karshipta::v1::FleetMission& mission,
                                                                 const std::string& ward_id) {
    auto it = std::find_if(mission.ward_states().begin(), mission.ward_states().end(),
                            [&](const auto& state) { return state.ward_id() == ward_id; });
    if (it == mission.ward_states().end()) return std::nullopt;
    return *it;
}

#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
// Not a real UUID, same rationale as mission_importer.cpp's
// synthesize_mission_id: only needs to be unique enough for one gateway
// process to tell wards' independent missions apart.
std::string synthesize_ward_mission_id(const std::string& fleet_mission_id, const std::string& ward_id) {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return fleet_mission_id + "-" + ward_id + "-" + std::to_string(now_ns);
}

// Same rationale, for a Stop dispatch's synthesized Command.command_id -
// the "fleet-mission-stop-" prefix has no special meaning to WardManager,
// it only needs to be unique enough for pending_stops_ to key on.
std::string synthesize_stop_command_id(const std::string& fleet_mission_id, const std::string& ward_id) {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return "fleet-mission-stop-" + fleet_mission_id + "-" + ward_id + "-" + std::to_string(now_ns);
}
#endif

}  // namespace

FleetManager::FleetManager(Transport& transport, std::filesystem::path db_path,
                            std::filesystem::path fleet_mission_db_path)
    : transport_(transport),
      store_(std::move(db_path)),
      fleet_mission_store_(std::move(fleet_mission_db_path)) {}

std::optional<karshipta::v1::Fleet> FleetManager::find_fleet(const std::string& fleet_id) const {
    auto fleets = store_.list_fleets();
    auto it = std::find_if(fleets.begin(), fleets.end(),
                            [&](const auto& fleet) { return fleet.fleet_id() == fleet_id; });
    if (it == fleets.end()) return std::nullopt;
    return *it;
}

std::optional<karshipta::v1::Zone> FleetManager::find_zone(const std::string& zone_id) const {
    auto zones = store_.list_zones();
    auto it = std::find_if(zones.begin(), zones.end(),
                            [&](const auto& zone) { return zone.zone_id() == zone_id; });
    if (it == zones.end()) return std::nullopt;
    return *it;
}

void FleetManager::broadcast_fleet(const karshipta::v1::Fleet& fleet) const {
    karshipta::v1::Envelope envelope;
    *envelope.mutable_fleet() = fleet;
    transport_.broadcast(serialize_envelope(envelope));
}

void FleetManager::broadcast_zone(const karshipta::v1::Zone& zone) const {
    karshipta::v1::Envelope envelope;
    *envelope.mutable_zone() = zone;
    transport_.broadcast(serialize_envelope(envelope));
}

void FleetManager::broadcast_fleet_mission(const karshipta::v1::FleetMission& mission) const {
    karshipta::v1::Envelope envelope;
    *envelope.mutable_fleet_mission() = mission;
    transport_.broadcast(serialize_envelope(envelope));
}

void FleetManager::broadcast_gateway_warning(const std::string& code,
                                              const std::string& message) const {
    karshipta::v1::Envelope envelope;
    auto* event = envelope.mutable_event();
    event->set_timestamp_ms(unix_epoch_ms());
    event->set_severity(karshipta::v1::SEVERITY_WARNING);
    event->set_code(code);
    event->set_message(message);
    transport_.broadcast(serialize_envelope(envelope));
}

// ---------- Fleet ----------

karshipta::v1::FleetAck FleetManager::handle_create_fleet(const karshipta::v1::CreateFleet& request) {
    karshipta::v1::FleetAck ack;
    ack.set_request_id(request.request_id());
    try {
        const auto fleet_id = store_.create_fleet(request.name(), request.description());
        ack.set_fleet_id(fleet_id);
        ack.set_status(karshipta::v1::FLEET_ACK_STATUS_ACCEPTED);
        ack.set_message("fleet created");
        if (const auto fleet = find_fleet(fleet_id)) broadcast_fleet(*fleet);
    } catch (const std::exception& error) {
        // FleetZoneStore throws on a SQLite failure (disk full, permissions,
        // a concurrent-mutation race) - a per-request condition, not grounds
        // to take the whole gateway down over one Fleet/Zone request. Caught
        // here, at the boundary this class's own header already documents
        // ("Never throw on bad input"), and turned into the same observable
        // rejection shape every other store failure already produces.
        spdlog::error("create_fleet request '{}' failed: {}", request.request_id(), error.what());
        ack.set_status(karshipta::v1::FLEET_ACK_STATUS_REJECTED);
        ack.set_message(std::string("internal error: ") + error.what());
    }
    return ack;
}

karshipta::v1::FleetAck FleetManager::handle_rename_fleet(const karshipta::v1::RenameFleet& request) {
    karshipta::v1::FleetAck ack;
    ack.set_request_id(request.request_id());
    ack.set_fleet_id(request.fleet_id());
    // See handle_create_fleet()'s comment. The whole body is one try, not
    // just the mutation: find_fleet() below can throw too, and a rejection
    // built from that must not be lost to an uncaught exception either.
    try {
        const auto error =
            store_.rename_fleet(request.fleet_id(), request.name(), request.description());
        if (error) {
            spdlog::warn("rename_fleet request '{}' rejected: {}", request.request_id(), *error);
            ack.set_status(karshipta::v1::FLEET_ACK_STATUS_REJECTED);
            ack.set_message(*error);
        } else {
            ack.set_status(karshipta::v1::FLEET_ACK_STATUS_ACCEPTED);
            ack.set_message("fleet renamed");
            if (const auto fleet = find_fleet(request.fleet_id())) broadcast_fleet(*fleet);
        }
    } catch (const std::exception& error) {
        spdlog::error("rename_fleet request '{}' failed: {}", request.request_id(), error.what());
        ack.set_status(karshipta::v1::FLEET_ACK_STATUS_REJECTED);
        ack.set_message(std::string("internal error: ") + error.what());
    }
    return ack;
}

karshipta::v1::FleetAck FleetManager::handle_delete_fleet(const karshipta::v1::DeleteFleet& request) {
    karshipta::v1::FleetAck ack;
    ack.set_request_id(request.request_id());
    ack.set_fleet_id(request.fleet_id());
    std::optional<std::string> error;
    try {
        error = store_.delete_fleet(request.fleet_id());
    } catch (const std::exception& store_error) {
        error = std::string("internal error: ") + store_error.what();
    }
    if (error) {
        spdlog::warn("delete_fleet request '{}' rejected: {}", request.request_id(), *error);
        ack.set_status(karshipta::v1::FLEET_ACK_STATUS_REJECTED);
        ack.set_message(*error);
    } else {
        ack.set_status(karshipta::v1::FLEET_ACK_STATUS_ACCEPTED);
        ack.set_message("fleet deleted");
        // No updated object to broadcast (the fleet is gone): main.cpp
        // broadcasts this ack itself, which now carries fleet_id, so every
        // connected client (not just the requester) can drop it locally.
    }
    return ack;
}

karshipta::v1::FleetAck FleetManager::handle_add_ward_to_fleet(
    const karshipta::v1::AddWardToFleet& request) {
    karshipta::v1::FleetAck ack;
    ack.set_request_id(request.request_id());
    ack.set_fleet_id(request.fleet_id());
    // Whole body in one try, same reasoning as handle_rename_fleet(): the
    // post-success find_fleet() below can throw too.
    try {
        const auto error = store_.add_ward_to_fleet(request.fleet_id(), request.ward_id());
        if (error) {
            spdlog::warn("add_ward_to_fleet request '{}' rejected: {}", request.request_id(),
                         *error);
            ack.set_status(karshipta::v1::FLEET_ACK_STATUS_REJECTED);
            ack.set_message(*error);
        } else {
            ack.set_status(karshipta::v1::FLEET_ACK_STATUS_ACCEPTED);
            ack.set_message("ward added to fleet");
            if (const auto fleet = find_fleet(request.fleet_id())) broadcast_fleet(*fleet);
        }
    } catch (const std::exception& error) {
        spdlog::error("add_ward_to_fleet request '{}' failed: {}", request.request_id(),
                      error.what());
        ack.set_status(karshipta::v1::FLEET_ACK_STATUS_REJECTED);
        ack.set_message(std::string("internal error: ") + error.what());
    }
    return ack;
}

karshipta::v1::FleetAck FleetManager::handle_remove_ward_from_fleet(
    const karshipta::v1::RemoveWardFromFleet& request) {
    karshipta::v1::FleetAck ack;
    ack.set_request_id(request.request_id());
    ack.set_fleet_id(request.fleet_id());
    // Whole body in one try, same reasoning as handle_rename_fleet().
    try {
        const auto error = store_.remove_ward_from_fleet(request.fleet_id(), request.ward_id());
        if (error) {
            spdlog::warn("remove_ward_from_fleet request '{}' rejected: {}", request.request_id(),
                         *error);
            ack.set_status(karshipta::v1::FLEET_ACK_STATUS_REJECTED);
            ack.set_message(*error);
        } else {
            ack.set_status(karshipta::v1::FLEET_ACK_STATUS_ACCEPTED);
            ack.set_message("ward removed from fleet");
            if (const auto fleet = find_fleet(request.fleet_id())) broadcast_fleet(*fleet);
        }
    } catch (const std::exception& error) {
        spdlog::error("remove_ward_from_fleet request '{}' failed: {}", request.request_id(),
                      error.what());
        ack.set_status(karshipta::v1::FLEET_ACK_STATUS_REJECTED);
        ack.set_message(std::string("internal error: ") + error.what());
    }
    return ack;
}

// ---------- Zone ----------

karshipta::v1::ZoneAck FleetManager::handle_create_zone(const karshipta::v1::CreateZone& request) {
    karshipta::v1::ZoneAck ack;
    ack.set_request_id(request.request_id());
    if (request.vertices_size() < 3) {
        spdlog::warn("create_zone request '{}' rejected: fewer than 3 vertices",
                     request.request_id());
        ack.set_status(karshipta::v1::ZONE_ACK_STATUS_REJECTED);
        ack.set_message("a zone needs at least 3 vertices");
        return ack;
    }
    const std::vector<karshipta::v1::GeoPoint> vertices(request.vertices().begin(),
                                                          request.vertices().end());
    const std::optional<float> altitude_min_m =
        request.has_altitude_min_m() ? std::optional(request.altitude_min_m()) : std::nullopt;
    const std::optional<float> altitude_max_m =
        request.has_altitude_max_m() ? std::optional(request.altitude_max_m()) : std::nullopt;
    try {
        const auto zone_id = store_.create_zone(request.name(), request.type(), vertices,
                                                 altitude_min_m, altitude_max_m);
        ack.set_zone_id(zone_id);
        ack.set_status(karshipta::v1::ZONE_ACK_STATUS_ACCEPTED);
        ack.set_message("zone created");
        if (const auto zone = find_zone(zone_id)) broadcast_zone(*zone);
    } catch (const std::exception& error) {
        spdlog::error("create_zone request '{}' failed: {}", request.request_id(), error.what());
        ack.set_status(karshipta::v1::ZONE_ACK_STATUS_REJECTED);
        ack.set_message(std::string("internal error: ") + error.what());
    }
    return ack;
}

karshipta::v1::ZoneAck FleetManager::handle_update_zone(const karshipta::v1::UpdateZone& request) {
    karshipta::v1::ZoneAck ack;
    ack.set_request_id(request.request_id());
    ack.set_zone_id(request.zone_id());
    const std::optional<float> altitude_min_m =
        request.has_altitude_min_m() ? std::optional(request.altitude_min_m()) : std::nullopt;
    const std::optional<float> altitude_max_m =
        request.has_altitude_max_m() ? std::optional(request.altitude_max_m()) : std::nullopt;
    // Whole body in one try, same reasoning as handle_rename_fleet(): the
    // post-success find_zone() below can throw too.
    try {
        const auto error = store_.update_zone(request.zone_id(), request.name(), request.type(),
                                               altitude_min_m, altitude_max_m);
        if (error) {
            spdlog::warn("update_zone request '{}' rejected: {}", request.request_id(), *error);
            ack.set_status(karshipta::v1::ZONE_ACK_STATUS_REJECTED);
            ack.set_message(*error);
        } else {
            ack.set_status(karshipta::v1::ZONE_ACK_STATUS_ACCEPTED);
            ack.set_message("zone updated");
            if (const auto zone = find_zone(request.zone_id())) broadcast_zone(*zone);
        }
    } catch (const std::exception& error) {
        spdlog::error("update_zone request '{}' failed: {}", request.request_id(), error.what());
        ack.set_status(karshipta::v1::ZONE_ACK_STATUS_REJECTED);
        ack.set_message(std::string("internal error: ") + error.what());
    }
    return ack;
}

karshipta::v1::ZoneAck FleetManager::handle_delete_zone(const karshipta::v1::DeleteZone& request) {
    karshipta::v1::ZoneAck ack;
    ack.set_request_id(request.request_id());
    ack.set_zone_id(request.zone_id());
    std::optional<std::string> error;
    try {
        error = store_.delete_zone(request.zone_id());
    } catch (const std::exception& store_error) {
        error = std::string("internal error: ") + store_error.what();
    }
    if (error) {
        spdlog::warn("delete_zone request '{}' rejected: {}", request.request_id(), *error);
        ack.set_status(karshipta::v1::ZONE_ACK_STATUS_REJECTED);
        ack.set_message(*error);
    } else {
        ack.set_status(karshipta::v1::ZONE_ACK_STATUS_ACCEPTED);
        ack.set_message("zone deleted");
    }
    return ack;
}

// ---------- Fleet mission ----------

#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
karshipta::v1::FleetMissionAck FleetManager::handle_create_fleet_mission(
    const karshipta::v1::CreateFleetMission& request, WardManager& ward_manager) {
    karshipta::v1::FleetMissionAck ack;
    ack.set_request_id(request.request_id());
    // Empty fleet_id means an ad-hoc ward selection, not tied to a saved
    // Fleet (the console's Mission wizard lets an operator pick either a
    // Fleet or a handful of individual wards); only a non-empty fleet_id is
    // validated against the store, same as everywhere else fleet_id is
    // looked up. Whole body in one try: find_fleet() below can throw, same
    // reasoning as handle_create_fleet()'s comment.
    try {
        if (!request.fleet_id().empty() && !find_fleet(request.fleet_id())) {
            spdlog::warn("create_fleet_mission request '{}' rejected: unknown fleet_id '{}'",
                         request.request_id(), request.fleet_id());
            ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
            ack.set_message("unknown fleet_id: " + request.fleet_id());
            return ack;
        }
        if (request.ward_plans_size() == 0) {
            spdlog::warn("create_fleet_mission request '{}' rejected: no ward plans",
                         request.request_id());
            ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
            ack.set_message("no ward plans in request");
            return ack;
        }

        const std::vector<karshipta::v1::WardMissionPlan> ward_plans(request.ward_plans().begin(),
                                                                       request.ward_plans().end());
        const std::string fleet_mission_id = fleet_mission_store_.create_fleet_mission(
            request.fleet_id(), request.mission_name(), request.repeat_count(), ward_plans);

        // Each ward gets its own independent Mission: fresh mission_id, that
        // ward's own items, shared repeat_count - the actual fix over the
        // old flat-broadcast design (no two wards ever share a route).
        // dispatch_mission_upload_and_start()'s return is the immediate
        // accept/reject; the real upload outcome lands later via
        // handle_mission_upload_outcome().
        for (const auto& plan : ward_plans) {
            karshipta::v1::Mission mission;
            mission.set_mission_id(synthesize_ward_mission_id(fleet_mission_id, plan.ward_id()));
            mission.set_ward_id(plan.ward_id());
            mission.set_name(request.mission_name());
            *mission.mutable_items() = plan.items();
            mission.set_repeat_count(request.repeat_count());

            const auto rejection = ward_manager.dispatch_mission_upload_and_start(mission);
            karshipta::v1::WardMissionState state;
            state.set_ward_id(plan.ward_id());
            state.set_mission_id(mission.mission_id());
            if (rejection) {
                state.set_status(karshipta::v1::WARD_MISSION_STATUS_REJECTED);
                state.set_message(*rejection);
            } else {
                state.set_status(karshipta::v1::WARD_MISSION_STATUS_UPLOADING);
            }
            fleet_mission_store_.update_ward_state(fleet_mission_id, state);
        }

        ack.set_fleet_mission_id(fleet_mission_id);
        ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_ACCEPTED);
        ack.set_message("fleet mission created");
        if (const auto mission = fleet_mission_store_.get_fleet_mission(fleet_mission_id)) {
            broadcast_fleet_mission(*mission);
        }
    } catch (const std::exception& error) {
        spdlog::error("create_fleet_mission request '{}' failed: {}", request.request_id(),
                      error.what());
        ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
        ack.set_message(std::string("internal error: ") + error.what());
    }
    return ack;
}

karshipta::v1::FleetMissionAck FleetManager::handle_stop_fleet_mission(
    const karshipta::v1::StopFleetMission& request, WardManager& ward_manager) {
    karshipta::v1::FleetMissionAck ack;
    ack.set_request_id(request.request_id());
    ack.set_fleet_mission_id(request.fleet_mission_id());
    try {
        const auto mission = fleet_mission_store_.get_fleet_mission(request.fleet_mission_id());
        if (!mission) {
            spdlog::warn("stop_fleet_mission request '{}' rejected: unknown fleet_mission_id '{}'",
                         request.request_id(), request.fleet_mission_id());
            ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
            ack.set_message("unknown fleet_mission_id: " + request.fleet_mission_id());
            return ack;
        }

        const auto action = request.action() == karshipta::v1::FLEET_MISSION_STOP_ACTION_UNSPECIFIED
                                 ? karshipta::v1::FLEET_MISSION_STOP_ACTION_RTL
                                 : request.action();
        int dispatched = 0;
        for (const auto& state : mission->ward_states()) {
            if (state.status() == karshipta::v1::WARD_MISSION_STATUS_STOPPED ||
                state.status() == karshipta::v1::WARD_MISSION_STATUS_REJECTED ||
                state.status() == karshipta::v1::WARD_MISSION_STATUS_STOPPING) {
                continue;
            }
            karshipta::v1::Command command;
            const auto command_id = synthesize_stop_command_id(request.fleet_mission_id(), state.ward_id());
            command.set_command_id(command_id);
            command.set_ward_id(state.ward_id());
            command.set_timestamp_ms(unix_epoch_ms());
            switch (action) {
                case karshipta::v1::FLEET_MISSION_STOP_ACTION_HOLD:
                    command.mutable_pause_mission();
                    break;
                case karshipta::v1::FLEET_MISSION_STOP_ACTION_LAND:
                    command.mutable_land();
                    break;
                default:
                    command.mutable_rtl();
                    break;
            }
            // Write STOPPING and register the correlation entry before
            // dispatching: dispatch_command() can synchronously reject
            // (unknown/stopped/busy ward) and, via the command-outcome
            // observer, call back into handle_command_outcome() before this
            // call even returns. Doing it in this order means that callback
            // always sees (and correctly overwrites) the STOPPING state this
            // loop just wrote, instead of racing it and being clobbered by
            // it - the reverse order silently loses a synchronous rejection.
            karshipta::v1::WardMissionState stopping_state;
            stopping_state.set_ward_id(state.ward_id());
            stopping_state.set_status(karshipta::v1::WARD_MISSION_STATUS_STOPPING);
            stopping_state.set_mission_id(state.mission_id());
            fleet_mission_store_.update_ward_state(request.fleet_mission_id(), stopping_state);
            {
                std::lock_guard lock(pending_stops_mutex_);
                pending_stops_[command_id] = PendingStop{request.fleet_mission_id(), state.ward_id()};
            }
            ward_manager.dispatch_command(command);
            ++dispatched;
        }

        if (dispatched > 0) {
            fleet_mission_store_.set_status(request.fleet_mission_id(),
                                             karshipta::v1::FLEET_MISSION_STATUS_STOPPING);
            // Catches any ward whose dispatch_command() above rejected
            // synchronously (and so already settled via
            // handle_command_outcome() mid-loop, before the aggregate
            // status above was actually STOPPING yet - maybe_finalize_stop()
            // no-ops unless it is, so that mid-loop call could not have
            // finalized anything).
            maybe_finalize_stop(request.fleet_mission_id());
        }
        ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_ACCEPTED);
        ack.set_message(dispatched > 0 ? "stop dispatched to " + std::to_string(dispatched) + " ward(s)"
                                        : "no active wards to stop");
        if (const auto updated = fleet_mission_store_.get_fleet_mission(request.fleet_mission_id())) {
            broadcast_fleet_mission(*updated);
        }
    } catch (const std::exception& error) {
        spdlog::error("stop_fleet_mission request '{}' failed: {}", request.request_id(), error.what());
        ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
        ack.set_message(std::string("internal error: ") + error.what());
    }
    return ack;
}

karshipta::v1::FleetMissionAck FleetManager::handle_update_fleet_mission_routes(
    const karshipta::v1::UpdateFleetMissionRoutes& request, WardManager& ward_manager) {
    karshipta::v1::FleetMissionAck ack;
    ack.set_request_id(request.request_id());
    ack.set_fleet_mission_id(request.fleet_mission_id());
    try {
        const auto mission = fleet_mission_store_.get_fleet_mission(request.fleet_mission_id());
        if (!mission) {
            spdlog::warn("update_fleet_mission_routes request '{}' rejected: unknown fleet_mission_id '{}'",
                         request.request_id(), request.fleet_mission_id());
            ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
            ack.set_message("unknown fleet_mission_id: " + request.fleet_mission_id());
            return ack;
        }
        const bool every_ward_settled = std::all_of(
            mission->ward_states().begin(), mission->ward_states().end(), [](const auto& state) {
                return state.status() == karshipta::v1::WARD_MISSION_STATUS_STOPPED ||
                       state.status() == karshipta::v1::WARD_MISSION_STATUS_REJECTED;
            });
        if (!every_ward_settled) {
            spdlog::warn(
                "update_fleet_mission_routes request '{}' rejected: fleet_mission_id '{}' still has "
                "an active ward",
                request.request_id(), request.fleet_mission_id());
            ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
            ack.set_message("cannot edit while any ward is still active; stop it first");
            return ack;
        }
        if (request.ward_plans_size() == 0) {
            spdlog::warn("update_fleet_mission_routes request '{}' rejected: no ward plans",
                         request.request_id());
            ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
            ack.set_message("no ward plans in request");
            return ack;
        }

        const std::vector<karshipta::v1::WardMissionPlan> ward_plans(request.ward_plans().begin(),
                                                                       request.ward_plans().end());
        const auto update_error = fleet_mission_store_.update_ward_plans(
            request.fleet_mission_id(), request.mission_name(), request.repeat_count(), ward_plans);
        if (update_error) {
            spdlog::warn("update_fleet_mission_routes request '{}' rejected: {}", request.request_id(),
                         *update_error);
            ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
            ack.set_message(*update_error);
            return ack;
        }

        for (const auto& plan : ward_plans) {
            karshipta::v1::Mission ward_mission;
            ward_mission.set_mission_id(
                synthesize_ward_mission_id(request.fleet_mission_id(), plan.ward_id()));
            ward_mission.set_ward_id(plan.ward_id());
            ward_mission.set_name(request.mission_name());
            *ward_mission.mutable_items() = plan.items();
            ward_mission.set_repeat_count(request.repeat_count());

            const auto rejection = ward_manager.dispatch_mission_upload_and_start(ward_mission);
            karshipta::v1::WardMissionState state;
            state.set_ward_id(plan.ward_id());
            state.set_mission_id(ward_mission.mission_id());
            if (rejection) {
                state.set_status(karshipta::v1::WARD_MISSION_STATUS_REJECTED);
                state.set_message(*rejection);
            } else {
                state.set_status(karshipta::v1::WARD_MISSION_STATUS_UPLOADING);
            }
            fleet_mission_store_.update_ward_state(request.fleet_mission_id(), state);
        }

        ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_ACCEPTED);
        ack.set_message("fleet mission updated");
        if (const auto updated = fleet_mission_store_.get_fleet_mission(request.fleet_mission_id())) {
            broadcast_fleet_mission(*updated);
        }
    } catch (const std::exception& error) {
        spdlog::error("update_fleet_mission_routes request '{}' failed: {}", request.request_id(),
                      error.what());
        ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
        ack.set_message(std::string("internal error: ") + error.what());
    }
    return ack;
}
#endif  // KARSHIPTA_GATEWAY_ENABLE_MAVLINK

karshipta::v1::FleetMissionAck FleetManager::handle_remove_fleet_mission(
    const karshipta::v1::RemoveFleetMission& request) {
    karshipta::v1::FleetMissionAck ack;
    ack.set_request_id(request.request_id());
    ack.set_fleet_mission_id(request.fleet_mission_id());
    try {
        const auto mission = fleet_mission_store_.get_fleet_mission(request.fleet_mission_id());
        if (!mission) {
            spdlog::warn("remove_fleet_mission request '{}' rejected: unknown fleet_mission_id '{}'",
                         request.request_id(), request.fleet_mission_id());
            ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
            ack.set_message("unknown fleet_mission_id: " + request.fleet_mission_id());
            return ack;
        }
        const bool every_ward_settled = std::all_of(
            mission->ward_states().begin(), mission->ward_states().end(), [](const auto& state) {
                return state.status() == karshipta::v1::WARD_MISSION_STATUS_STOPPED ||
                       state.status() == karshipta::v1::WARD_MISSION_STATUS_REJECTED;
            });
        if (!every_ward_settled) {
            spdlog::warn(
                "remove_fleet_mission request '{}' rejected: fleet_mission_id '{}' still has an "
                "active ward",
                request.request_id(), request.fleet_mission_id());
            ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
            ack.set_message("cannot remove while any ward is still active; stop it first");
            return ack;
        }
        const auto error = fleet_mission_store_.delete_fleet_mission(request.fleet_mission_id());
        if (error) {
            ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
            ack.set_message(*error);
        } else {
            ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_ACCEPTED);
            ack.set_message("fleet mission removed");
        }
    } catch (const std::exception& error) {
        spdlog::error("remove_fleet_mission request '{}' failed: {}", request.request_id(), error.what());
        ack.set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
        ack.set_message(std::string("internal error: ") + error.what());
    }
    return ack;
}

void FleetManager::handle_command_outcome(const karshipta::v1::CommandAck& ack) {
    if (ack.status() != karshipta::v1::COMMAND_STATUS_SUCCESS &&
        ack.status() != karshipta::v1::COMMAND_STATUS_REJECTED &&
        ack.status() != karshipta::v1::COMMAND_STATUS_TIMEOUT) {
        return;  // ACCEPTED/EXECUTING: not a terminal outcome yet
    }
    std::optional<PendingStop> pending;
    {
        std::lock_guard lock(pending_stops_mutex_);
        auto it = pending_stops_.find(ack.command_id());
        if (it != pending_stops_.end()) {
            pending = it->second;
            pending_stops_.erase(it);
        }
    }
    if (!pending) return;  // this ack has nothing to do with a fleet mission

    try {
        const auto mission = fleet_mission_store_.get_fleet_mission(pending->fleet_mission_id);
        if (!mission) return;  // removed meanwhile
        auto state = find_ward_state(*mission, pending->ward_id);
        if (!state) return;

        if (ack.status() == karshipta::v1::COMMAND_STATUS_SUCCESS) {
            state->set_status(karshipta::v1::WARD_MISSION_STATUS_STOPPED);
            state->set_message("");
        } else {
            // The stop itself failed or never landed: the ward never
            // actually stopped, so revert to ACTIVE rather than leaving it
            // stuck at STOPPING with no way for Remove's safety gate to
            // ever clear. The operator can retry Stop.
            state->set_status(karshipta::v1::WARD_MISSION_STATUS_ACTIVE);
            state->set_message(ack.status() == karshipta::v1::COMMAND_STATUS_REJECTED
                                    ? ack.message()
                                    : "stop command timed out");
        }
        fleet_mission_store_.update_ward_state(pending->fleet_mission_id, *state);
        maybe_finalize_stop(pending->fleet_mission_id);
        if (const auto updated = fleet_mission_store_.get_fleet_mission(pending->fleet_mission_id)) {
            broadcast_fleet_mission(*updated);
        }
    } catch (const std::exception& error) {
        spdlog::error("handle_command_outcome for fleet_mission '{}' ward '{}' failed: {}",
                      pending->fleet_mission_id, pending->ward_id, error.what());
    }
}

void FleetManager::handle_mission_upload_outcome(const std::string& ward_id,
                                                  const std::string& mission_id, const bool success,
                                                  const std::string& message) {
    try {
        for (const auto& mission : fleet_mission_store_.list_fleet_missions()) {
            auto state = find_ward_state(mission, ward_id);
            if (!state || state->mission_id() != mission_id ||
                state->status() != karshipta::v1::WARD_MISSION_STATUS_UPLOADING) {
                continue;
            }
            state->set_status(success ? karshipta::v1::WARD_MISSION_STATUS_ACTIVE
                                       : karshipta::v1::WARD_MISSION_STATUS_REJECTED);
            state->set_message(success ? "" : message);
            fleet_mission_store_.update_ward_state(mission.fleet_mission_id(), *state);
            if (const auto updated = fleet_mission_store_.get_fleet_mission(mission.fleet_mission_id())) {
                broadcast_fleet_mission(*updated);
            }
            return;
        }
    } catch (const std::exception& error) {
        spdlog::error("handle_mission_upload_outcome for ward '{}' mission '{}' failed: {}", ward_id,
                      mission_id, error.what());
    }
}

void FleetManager::maybe_finalize_stop(const std::string& fleet_mission_id) {
    const auto mission = fleet_mission_store_.get_fleet_mission(fleet_mission_id);
    if (!mission || mission->status() != karshipta::v1::FLEET_MISSION_STATUS_STOPPING) return;
    const bool all_settled =
        std::all_of(mission->ward_states().begin(), mission->ward_states().end(), [](const auto& state) {
            return state.status() == karshipta::v1::WARD_MISSION_STATUS_STOPPED ||
                   state.status() == karshipta::v1::WARD_MISSION_STATUS_REJECTED;
        });
    if (all_settled) {
        fleet_mission_store_.set_status(fleet_mission_id, karshipta::v1::FLEET_MISSION_STATUS_STOPPED);
    }
}

// ---------- Viewer rejection ----------

void FleetManager::reject_viewer_envelope(const Transport::ClientId client,
                                           const karshipta::v1::Envelope& envelope) {
    switch (envelope.payload_case()) {
        case karshipta::v1::Envelope::kCreateFleet: {
            const auto& request = envelope.create_fleet();
            spdlog::warn("viewer client {} attempted create_fleet, rejecting", client);
            karshipta::v1::Envelope ack_envelope;
            auto* ack = ack_envelope.mutable_fleet_ack();
            ack->set_request_id(request.request_id());
            ack->set_status(karshipta::v1::FLEET_ACK_STATUS_REJECTED);
            ack->set_message(kReadOnlySessionMessage);
            transport_.broadcast(serialize_envelope(ack_envelope));
            break;
        }
        case karshipta::v1::Envelope::kRenameFleet: {
            const auto& request = envelope.rename_fleet();
            spdlog::warn("viewer client {} attempted rename_fleet '{}', rejecting", client,
                         request.fleet_id());
            karshipta::v1::Envelope ack_envelope;
            auto* ack = ack_envelope.mutable_fleet_ack();
            ack->set_request_id(request.request_id());
            ack->set_fleet_id(request.fleet_id());
            ack->set_status(karshipta::v1::FLEET_ACK_STATUS_REJECTED);
            ack->set_message(kReadOnlySessionMessage);
            transport_.broadcast(serialize_envelope(ack_envelope));
            break;
        }
        case karshipta::v1::Envelope::kDeleteFleet: {
            const auto& request = envelope.delete_fleet();
            spdlog::warn("viewer client {} attempted delete_fleet '{}', rejecting", client,
                         request.fleet_id());
            karshipta::v1::Envelope ack_envelope;
            auto* ack = ack_envelope.mutable_fleet_ack();
            ack->set_request_id(request.request_id());
            ack->set_fleet_id(request.fleet_id());
            ack->set_status(karshipta::v1::FLEET_ACK_STATUS_REJECTED);
            ack->set_message(kReadOnlySessionMessage);
            transport_.broadcast(serialize_envelope(ack_envelope));
            break;
        }
        case karshipta::v1::Envelope::kAddWardToFleet: {
            const auto& request = envelope.add_ward_to_fleet();
            spdlog::warn("viewer client {} attempted add_ward_to_fleet '{}', rejecting", client,
                         request.fleet_id());
            karshipta::v1::Envelope ack_envelope;
            auto* ack = ack_envelope.mutable_fleet_ack();
            ack->set_request_id(request.request_id());
            ack->set_fleet_id(request.fleet_id());
            ack->set_status(karshipta::v1::FLEET_ACK_STATUS_REJECTED);
            ack->set_message(kReadOnlySessionMessage);
            transport_.broadcast(serialize_envelope(ack_envelope));
            break;
        }
        case karshipta::v1::Envelope::kRemoveWardFromFleet: {
            const auto& request = envelope.remove_ward_from_fleet();
            spdlog::warn("viewer client {} attempted remove_ward_from_fleet '{}', rejecting",
                         client, request.fleet_id());
            karshipta::v1::Envelope ack_envelope;
            auto* ack = ack_envelope.mutable_fleet_ack();
            ack->set_request_id(request.request_id());
            ack->set_fleet_id(request.fleet_id());
            ack->set_status(karshipta::v1::FLEET_ACK_STATUS_REJECTED);
            ack->set_message(kReadOnlySessionMessage);
            transport_.broadcast(serialize_envelope(ack_envelope));
            break;
        }
        case karshipta::v1::Envelope::kCreateZone: {
            const auto& request = envelope.create_zone();
            spdlog::warn("viewer client {} attempted create_zone, rejecting", client);
            karshipta::v1::Envelope ack_envelope;
            auto* ack = ack_envelope.mutable_zone_ack();
            ack->set_request_id(request.request_id());
            ack->set_status(karshipta::v1::ZONE_ACK_STATUS_REJECTED);
            ack->set_message(kReadOnlySessionMessage);
            transport_.broadcast(serialize_envelope(ack_envelope));
            break;
        }
        case karshipta::v1::Envelope::kUpdateZone: {
            const auto& request = envelope.update_zone();
            spdlog::warn("viewer client {} attempted update_zone '{}', rejecting", client,
                         request.zone_id());
            karshipta::v1::Envelope ack_envelope;
            auto* ack = ack_envelope.mutable_zone_ack();
            ack->set_request_id(request.request_id());
            ack->set_zone_id(request.zone_id());
            ack->set_status(karshipta::v1::ZONE_ACK_STATUS_REJECTED);
            ack->set_message(kReadOnlySessionMessage);
            transport_.broadcast(serialize_envelope(ack_envelope));
            break;
        }
        case karshipta::v1::Envelope::kDeleteZone: {
            const auto& request = envelope.delete_zone();
            spdlog::warn("viewer client {} attempted delete_zone '{}', rejecting", client,
                         request.zone_id());
            karshipta::v1::Envelope ack_envelope;
            auto* ack = ack_envelope.mutable_zone_ack();
            ack->set_request_id(request.request_id());
            ack->set_zone_id(request.zone_id());
            ack->set_status(karshipta::v1::ZONE_ACK_STATUS_REJECTED);
            ack->set_message(kReadOnlySessionMessage);
            transport_.broadcast(serialize_envelope(ack_envelope));
            break;
        }
        case karshipta::v1::Envelope::kCreateFleetMission: {
            const auto& request = envelope.create_fleet_mission();
            spdlog::warn("viewer client {} attempted create_fleet_mission, rejecting", client);
            karshipta::v1::Envelope ack_envelope;
            auto* ack = ack_envelope.mutable_fleet_mission_ack();
            ack->set_request_id(request.request_id());
            ack->set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
            ack->set_message(kReadOnlySessionMessage);
            transport_.broadcast(serialize_envelope(ack_envelope));
            break;
        }
        case karshipta::v1::Envelope::kStopFleetMission: {
            const auto& request = envelope.stop_fleet_mission();
            spdlog::warn("viewer client {} attempted stop_fleet_mission '{}', rejecting", client,
                         request.fleet_mission_id());
            karshipta::v1::Envelope ack_envelope;
            auto* ack = ack_envelope.mutable_fleet_mission_ack();
            ack->set_request_id(request.request_id());
            ack->set_fleet_mission_id(request.fleet_mission_id());
            ack->set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
            ack->set_message(kReadOnlySessionMessage);
            transport_.broadcast(serialize_envelope(ack_envelope));
            break;
        }
        case karshipta::v1::Envelope::kRemoveFleetMission: {
            const auto& request = envelope.remove_fleet_mission();
            spdlog::warn("viewer client {} attempted remove_fleet_mission '{}', rejecting", client,
                         request.fleet_mission_id());
            karshipta::v1::Envelope ack_envelope;
            auto* ack = ack_envelope.mutable_fleet_mission_ack();
            ack->set_request_id(request.request_id());
            ack->set_fleet_mission_id(request.fleet_mission_id());
            ack->set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
            ack->set_message(kReadOnlySessionMessage);
            transport_.broadcast(serialize_envelope(ack_envelope));
            break;
        }
        case karshipta::v1::Envelope::kUpdateFleetMissionRoutes: {
            const auto& request = envelope.update_fleet_mission_routes();
            spdlog::warn("viewer client {} attempted update_fleet_mission_routes '{}', rejecting",
                         client, request.fleet_mission_id());
            karshipta::v1::Envelope ack_envelope;
            auto* ack = ack_envelope.mutable_fleet_mission_ack();
            ack->set_request_id(request.request_id());
            ack->set_fleet_mission_id(request.fleet_mission_id());
            ack->set_status(karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
            ack->set_message(kReadOnlySessionMessage);
            transport_.broadcast(serialize_envelope(ack_envelope));
            break;
        }
        default:
            spdlog::warn("viewer client {} sent unexpected fleet payload kind {}, ignoring", client,
                         static_cast<int>(envelope.payload_case()));
            break;
    }
}

// ---------- Snapshot ----------

void FleetManager::send_fleet_zone_snapshot(const Transport::ClientId client) const {
    // Called from Transport::on_connect (see main.cpp), a path with no
    // request to reject if the store fails - just log and skip this
    // client's snapshot rather than let the exception escape uncaught and
    // take the whole gateway down over one new connection. See
    // handle_create_fleet()'s comment for the same reasoning applied to the
    // request/ack handlers.
    try {
        for (const auto& fleet : store_.list_fleets()) {
            karshipta::v1::Envelope envelope;
            *envelope.mutable_fleet() = fleet;
            transport_.send(client, serialize_envelope(envelope));
        }
        for (const auto& zone : store_.list_zones()) {
            karshipta::v1::Envelope envelope;
            *envelope.mutable_zone() = zone;
            transport_.send(client, serialize_envelope(envelope));
        }
    } catch (const std::exception& error) {
        spdlog::error("send_fleet_zone_snapshot to client {} failed: {}", client, error.what());
    }
}

void FleetManager::send_fleet_mission_snapshot(const Transport::ClientId client) const {
    // Same reasoning as send_fleet_zone_snapshot(): no request to reject
    // here, just log and skip this client's snapshot on a store failure.
    try {
        for (const auto& mission : fleet_mission_store_.list_fleet_missions()) {
            karshipta::v1::Envelope envelope;
            *envelope.mutable_fleet_mission() = mission;
            transport_.send(client, serialize_envelope(envelope));
        }
    } catch (const std::exception& error) {
        spdlog::error("send_fleet_mission_snapshot to client {} failed: {}", client, error.what());
    }
}
