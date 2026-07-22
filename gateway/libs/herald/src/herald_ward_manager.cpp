#include "herald_ward_manager.h"

#include <karshipta/v1/envelope.pb.h>
#include <spdlog/spdlog.h>

#include <utility>
#include <vector>

namespace {

// Duplicated from ward_manager.cpp/fleet_manager.cpp rather than shared
// across libraries for one small function; matches this repo's existing
// precedent for serialize_envelope.
std::vector<uint8_t> serialize_envelope(const karshipta::v1::Envelope& envelope) {
    std::vector<uint8_t> bytes(envelope.ByteSizeLong());
    if (!envelope.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()))) {
        spdlog::error("failed to serialize an Envelope of {} bytes", bytes.size());
        bytes.clear();
    }
    return bytes;
}

// Maps Herald's EntityClass onto karshipta's WardClass. Not a 1:1 ordinal
// mapping: WardClass has two extra flight-irrelevant values (UNDERWATER,
// SURFACE_VESSEL) inserted before LIVESTOCK_TAG/GENERIC_TRACKER that Herald
// has no equivalent for, so this maps by name, not by raw enum number.
karshipta::v1::WardClass to_ward_class(const herald::v0::EntityClass entity_class) {
    switch (entity_class) {
        case herald::v0::ENTITY_CLASS_MULTIROTOR:
            return karshipta::v1::WARD_CLASS_MULTIROTOR;
        case herald::v0::ENTITY_CLASS_FIXED_WING:
            return karshipta::v1::WARD_CLASS_FIXED_WING;
        case herald::v0::ENTITY_CLASS_VTOL:
            return karshipta::v1::WARD_CLASS_VTOL;
        case herald::v0::ENTITY_CLASS_HELICOPTER:
            return karshipta::v1::WARD_CLASS_HELICOPTER;
        case herald::v0::ENTITY_CLASS_GROUND_VEHICLE:
            return karshipta::v1::WARD_CLASS_GROUND;
        case herald::v0::ENTITY_CLASS_LIVESTOCK_TAG:
            return karshipta::v1::WARD_CLASS_LIVESTOCK_TAG;
        case herald::v0::ENTITY_CLASS_GENERIC_TRACKER:
            return karshipta::v1::WARD_CLASS_GENERIC_TRACKER;
        case herald::v0::ENTITY_CLASS_UNSPECIFIED:
        default:
            return karshipta::v1::WARD_CLASS_UNSPECIFIED;
    }
}

// Builds a WardState from a Herald message. Mirrors ward_manager.cpp's own
// build_ward_state() field by field, but never calls mutable_flight():
// Herald never carries flight-specific data, and no autopilot exists behind
// a Herald-reporting source for that data to describe, so flight stays
// unset, exactly as WardState's own doc comment describes for a non-flight
// ward.
karshipta::v1::WardState build_ward_state(const herald::v0::Herald& msg) {
    karshipta::v1::WardState state;
    state.set_ward_id(msg.entity_id());
    state.set_timestamp_ms(msg.timestamp_ms());

    const auto& position = msg.position();
    auto* proto_position = state.mutable_position();
    proto_position->set_latitude_deg(position.latitude_deg());
    proto_position->set_longitude_deg(position.longitude_deg());
    proto_position->set_altitude_msl_m(position.altitude_msl_m());
    proto_position->set_altitude_rel_m(position.altitude_rel_m());

    if (msg.has_velocity()) {
        auto* proto_velocity = state.mutable_velocity();
        proto_velocity->set_north_m_s(msg.velocity().north_m_s());
        proto_velocity->set_east_m_s(msg.velocity().east_m_s());
        proto_velocity->set_down_m_s(msg.velocity().down_m_s());
    }

    if (msg.has_battery()) {
        auto* proto_battery = state.mutable_battery();
        proto_battery->set_voltage_v(msg.battery().voltage_v());
        proto_battery->set_remaining_pct(msg.battery().remaining_pct());
    }

    if (msg.has_gps()) {
        auto* proto_gps = state.mutable_gps();
        // Herald's Gps has no fix_type equivalent; explicit UNSPECIFIED is
        // the honest default rather than guessing a fix quality (matches
        // the now-deleted console/src/lib/herald.ts, which made the same
        // call for the same reason).
        proto_gps->set_fix_type(karshipta::v1::GPS_FIX_TYPE_UNSPECIFIED);
        proto_gps->set_num_satellites(msg.gps().num_satellites());
        proto_gps->set_hdop(msg.gps().hdop());
    }

    state.set_health_ok(msg.health_ok());
    state.set_connected(true);
    for (const auto& tag : msg.tags()) {
        state.add_tags(tag);
    }

    // msg.org_id() (tenant/org scope) has no WardState equivalent yet:
    // WardState carries no org scoping today (github issue #104).
    // Intentionally dropped here rather than stored in a made-up field.

    return state;
}

}  // namespace

HeraldWardManager::HeraldWardManager(Transport& transport, WardManager& ward_manager)
    : transport_(transport), ward_manager_(ward_manager) {}

HeraldIngestResult HeraldWardManager::ingest(const herald::v0::Herald& msg) {
    const std::string& entity_id = msg.entity_id();

    if (ward_manager_.has_ward(entity_id)) {
        spdlog::error("rejected Herald message for entity_id '{}': already a MAVLink ward_id",
                      entity_id);
        return HeraldIngestResult::kWardIdCollision;
    }

    bool first_sight = false;
    karshipta::v1::WardInfo info;
    {
        std::lock_guard lock(mutex_);
        first_sight = !known_wards_.contains(entity_id);
        if (first_sight) {
            info.set_ward_id(entity_id);
            info.set_ward_class(to_ward_class(msg.entity_class()));
            info.set_autopilot("");
            info.set_firmware_version("");
            info.set_mavlink_system_id(0);
            info.set_origin(karshipta::v1::WARD_ORIGIN_HARDWARE);
            known_wards_[entity_id] = info;
        }
    }

    // Broadcasts happen unlocked, same as run_publish_loop() in
    // ward_manager.cpp: transport_.broadcast() is a blocking socket write
    // per client and must not run while mutex_ is held.
    if (first_sight) {
        karshipta::v1::Envelope info_envelope;
        *info_envelope.mutable_ward_info() = info;
        transport_.broadcast(serialize_envelope(info_envelope));
    }

    karshipta::v1::Envelope state_envelope;
    *state_envelope.mutable_ward_state() = build_ward_state(msg);
    transport_.broadcast(serialize_envelope(state_envelope));

    return HeraldIngestResult::kOk;
}

void HeraldWardManager::send_known_wards(const Transport::ClientId client) const {
    std::vector<karshipta::v1::WardInfo> infos;
    {
        std::lock_guard lock(mutex_);
        infos.reserve(known_wards_.size());
        for (const auto& entry : known_wards_) {
            infos.push_back(entry.second);
        }
    }
    for (const auto& info : infos) {
        karshipta::v1::Envelope envelope;
        *envelope.mutable_ward_info() = info;
        transport_.send(client, serialize_envelope(envelope));
    }
}
