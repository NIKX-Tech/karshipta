#include "herald_ward_manager.h"

#include <karshipta/v1/envelope.pb.h>
#include <spdlog/spdlog.h>

#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
#include <ward_manager.h>
#endif

#include <utility>
#include <vector>

namespace {

uint64_t unix_epoch_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

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
karshipta::v1::WardState build_ward_state(const herald::v0::Herald& msg,
                                          const uint64_t ingestion_ms) {
    karshipta::v1::WardState state;
    state.set_ward_id(msg.entity_id());
    // karshipta#268: timestamp_ms is the device's own sample time (parsed
    // from the vendor payload by herald_field_mapper), ingestion_ms is when
    // the gateway actually received this message - the two can differ a
    // lot for a store-and-forward tracker, which is the whole reason this
    // field exists.
    state.set_timestamp_ms(msg.timestamp_ms());
    state.set_ingestion_ms(ingestion_ms);

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

    // msg.source() (human-readable label for which system sent this) has no
    // WardState equivalent either, and isn't the same thing as tags (which
    // are operator-defined grouping labels, not a provenance string).
    // Intentionally dropped for the same reason as org_id above, not stored
    // in a made-up field or conflated with tags.

    return state;
}

}  // namespace

#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
HeraldWardManager::HeraldWardManager(Transport& transport, WardManager& ward_manager,
                                     std::chrono::milliseconds stale_timeout)
    : transport_(transport),
      ward_manager_(ward_manager),
      stale_timeout_(stale_timeout),
      stale_checker_([this](const std::stop_token& stop_token) { run_stale_checker(stop_token); }) {
}
#else
HeraldWardManager::HeraldWardManager(Transport& transport, std::chrono::milliseconds stale_timeout)
    : transport_(transport),
      stale_timeout_(stale_timeout),
      stale_checker_([this](const std::stop_token& stop_token) { run_stale_checker(stop_token); }) {
}
#endif

void HeraldWardManager::run_stale_checker(std::stop_token stop_token) {
    const auto interval = stale_check_interval();
    while (!stop_token.stop_requested()) {
        // karshipta#268: collect under the lock, broadcast unlocked - same
        // snapshot-then-release shape ingest() and WardManager's own
        // run_publish_loop/push_geofence already use (broadcast() is a
        // blocking socket write per client and must not run while mutex_
        // is held).
        std::vector<karshipta::v1::WardState> to_broadcast;
        {
            std::lock_guard lock(mutex_);
            const auto now = std::chrono::steady_clock::now();
            for (auto& [entity_id, entry] : last_seen_) {
                if (entry.marked_stale) continue;
                if (now - entry.last_seen_at < stale_timeout_) continue;
                entry.marked_stale = true;
                entry.last_state.set_connected(false);
                to_broadcast.push_back(entry.last_state);
            }
        }
        for (const auto& state : to_broadcast) {
            spdlog::warn("herald ward '{}' went stale ({}ms with no message)", state.ward_id(),
                         stale_timeout_.count());
            karshipta::v1::Envelope envelope;
            *envelope.mutable_ward_state() = state;
            transport_.broadcast(serialize_envelope(envelope));
        }

        // Slept in small steps rather than one sleep_for(interval): a
        // jthread's stop_token is cooperative, not something that
        // interrupts a blocking sleep, so one big sleep would make
        // ~HeraldWardManager() (which requests stop then joins) wait out
        // however much of interval was left - confirmed live, every
        // existing test's teardown got up to 7.5s slower (a quarter of the
        // 30s production default) the first time this used a single
        // sleep_for. 50ms caps that shutdown latency regardless of how long
        // interval itself is.
        constexpr auto kSleepStep = std::chrono::milliseconds(50);
        auto remaining = interval;
        while (remaining > std::chrono::milliseconds::zero() && !stop_token.stop_requested()) {
            const auto step = std::min(remaining, kSleepStep);
            std::this_thread::sleep_for(step);
            remaining -= step;
        }
    }
}

HeraldIngestResult HeraldWardManager::ingest(const herald::v0::Herald& msg) {
    const std::string& entity_id = msg.entity_id();

#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
    if (ward_manager_.has_ward(entity_id)) {
        spdlog::error("rejected Herald message for entity_id '{}': already a MAVLink ward_id",
                      entity_id);
        return HeraldIngestResult::kWardIdCollision;
    }
#endif

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

    const karshipta::v1::WardState state = build_ward_state(msg, unix_epoch_ms());

    // karshipta#268: a real message always means "alive, as of now" -
    // refresh last_seen_at and clear marked_stale regardless of whether the
    // staleness checker had already flipped this ward to disconnected,
    // which is what lets a tracker recover automatically on its next
    // message with no separate code path.
    {
        std::lock_guard lock(mutex_);
        last_seen_[entity_id] = LastSeen{.last_state = state,
                                         .last_seen_at = std::chrono::steady_clock::now(),
                                         .marked_stale = false};
    }

    karshipta::v1::Envelope state_envelope;
    *state_envelope.mutable_ward_state() = state;
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
