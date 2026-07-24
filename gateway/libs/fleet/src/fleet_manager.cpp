#include "fleet_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
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

#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
// Not a real UUID, same rationale as mission_importer.cpp's
// synthesize_mission_id: only needs to be unique enough for one gateway
// process to tell fanned-out missions apart. Only handle_fleet_mission_assignment
// (below, also guarded) calls this - unused otherwise.
std::string synthesize_fleet_mission_id(const std::string& fleet_id, const std::string& ward_id) {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    return "fleet-" + fleet_id + "-" + ward_id + "-" + std::to_string(now_ns);
}
#endif

}  // namespace

FleetManager::FleetManager(Transport& transport, std::filesystem::path db_path)
    : transport_(transport), store_(std::move(db_path)) {}

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

// ---------- Fleet-wide mission assignment ----------

#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
void FleetManager::handle_fleet_mission_assignment(
    const karshipta::v1::FleetMissionAssignment& request, WardManager& ward_manager) {
    // Empty fleet_id means an ad-hoc ward selection, not tied to a saved
    // Fleet (the console's Mission tab lets an operator pick either a
    // Fleet or a handful of individual wards); only a non-empty fleet_id
    // is validated against the store, same as everywhere else fleet_id is
    // looked up.
    if (!request.fleet_id().empty()) {
        // See handle_create_fleet()'s comment: find_fleet() can throw if the
        // store itself fails, and that must become a rejection here too, not
        // an uncaught exception on this connection's worker thread.
        bool fleet_exists = false;
        try {
            fleet_exists = find_fleet(request.fleet_id()).has_value();
        } catch (const std::exception& error) {
            spdlog::error("fleet_mission_assignment request '{}' failed: {}", request.request_id(),
                          error.what());
            broadcast_gateway_warning("FLEET_MISSION_ASSIGNMENT_REJECTED",
                                       std::string("internal error: ") + error.what());
            return;
        }
        if (!fleet_exists) {
            spdlog::warn("fleet_mission_assignment request '{}' rejected: unknown fleet_id '{}'",
                         request.request_id(), request.fleet_id());
            broadcast_gateway_warning("FLEET_MISSION_ASSIGNMENT_REJECTED",
                                       "unknown fleet_id: " + request.fleet_id());
            return;
        }
    }
    if (request.ward_ids_size() == 0) {
        spdlog::warn("fleet_mission_assignment request '{}' rejected: no wards selected",
                     request.request_id());
        broadcast_gateway_warning("FLEET_MISSION_ASSIGNMENT_REJECTED",
                                   "no wards selected for fleet_id: " + request.fleet_id());
        return;
    }

    // Each selected ward gets its own independent copy: fresh mission_id,
    // that ward's ward_id, otherwise identical items/repeat_count (proto
    // comment on FleetMissionAssignment). A per-ward rejection (unknown/
    // stopped/busy ward) surfaces as a WARNING Event via WardManager itself,
    // the same channel a solo mission upload rejection already uses -
    // deliberately not aggregated into one ack here, since there is no
    // FleetMissionAssignment ack type (mirrors solo Envelope.mission_upload,
    // which has none either).
    for (const auto& ward_id : request.ward_ids()) {
        karshipta::v1::Mission mission;
        mission.set_mission_id(synthesize_fleet_mission_id(request.fleet_id(), ward_id));
        mission.set_ward_id(ward_id);
        mission.set_name(request.mission_name());
        *mission.mutable_items() = request.items();
        mission.set_repeat_count(request.repeat_count());
        (void)ward_manager.dispatch_mission_upload_and_start(mission);
    }
}
#endif  // KARSHIPTA_GATEWAY_ENABLE_MAVLINK

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
        case karshipta::v1::Envelope::kFleetMissionAssignment: {
            const auto& request = envelope.fleet_mission_assignment();
            spdlog::warn("viewer client {} attempted fleet_mission_assignment on fleet_id '{}', "
                         "rejecting",
                         client, request.fleet_id());
            broadcast_gateway_warning("FLEET_MISSION_ASSIGNMENT_REJECTED", kReadOnlySessionMessage);
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
