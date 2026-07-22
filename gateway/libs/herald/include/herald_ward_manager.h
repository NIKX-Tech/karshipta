#ifndef KARSHIPTA_GATEWAY_HERALD_WARD_MANAGER_H
#define KARSHIPTA_GATEWAY_HERALD_WARD_MANAGER_H

#include <herald/v0/herald.pb.h>
#include <karshipta/v1/telemetry.pb.h>
#include <transport.h>
#include <ward_manager.h>

#include <map>
#include <mutex>
#include <string>

// Result of ingesting one Herald message. kOk means a WardState (and, on
// first sight of entity_id, a WardInfo) was built and broadcast; kWardIdCollision
// means nothing was broadcast because entity_id is already a known MAVLink
// ward_id (see WardManager::has_ward).
enum class HeraldIngestResult { kOk, kWardIdCollision };

// Owns ingestion of Herald messages (github.com/NIKX-Tech/herald), the
// non-MAVLink counterpart to WardManager. Kept as its own class rather than
// folded into WardManager: Herald wards are push-only telemetry with no
// command surface at all (no autopilot, no arm/disarm/land/mission), so
// nothing here should ever sit inside the class that owns those safety-
// critical MAVSDK code paths (gateway/CLAUDE.md rule 3, one clear owner per
// resource). Herald is also meant to be the normalization target for every
// future non-MAVLink format (a CoT bridge, a SensorThings bridge,
// declarative vendor mapping): each of those translates its source into a
// Herald message and calls ingest() here, so this class is "every
// non-MAVLink ward," permanently, not just Herald's own native wire format.
class HeraldWardManager {
   public:
    // ward_manager is consulted (read-only, via has_ward()) to reject a
    // Herald message whose entity_id collides with an existing MAVLink
    // ward_id; this class never mutates it.
    HeraldWardManager(Transport& transport, WardManager& ward_manager);

    HeraldWardManager(const HeraldWardManager&) = delete;
    HeraldWardManager& operator=(const HeraldWardManager&) = delete;
    HeraldWardManager(HeraldWardManager&&) = delete;
    HeraldWardManager& operator=(HeraldWardManager&&) = delete;

    // Builds a WardState from msg and broadcasts it, and, on first sight of
    // msg.entity_id(), also builds and broadcasts a WardInfo first (WardInfo's
    // own contract: "sent on connect and on change"). Rejects with
    // kWardIdCollision, broadcasting nothing, if entity_id is already a
    // MAVLink ward's ward_id. Never populates WardState.flight: Herald never
    // carries flight-specific data, and this ward has no autopilot for that
    // data to describe (see herald_ward_manager.cpp's build_ward_state()
    // for the field-by-field mapping).
    HeraldIngestResult ingest(const herald::v0::Herald& msg);

    // Sends one WardInfo to exactly this client for every entity_id seen so
    // far. Wire this to Transport::on_connect alongside
    // WardManager::send_ward_info, so a client that connects after a Herald
    // source has already reported still learns about it.
    void send_known_wards(Transport::ClientId client) const;

   private:
    Transport& transport_;
    WardManager& ward_manager_;
    mutable std::mutex mutex_;
    // Every entity_id ingest() has ever built a WardInfo for, so a repeat
    // message from the same source doesn't re-announce it. In-memory only,
    // unlike WardManager's persisted fleet config: a Herald ward's identity
    // is defined entirely by "it has posted a message," so there is nothing
    // meaningful to persist across a restart.
    std::map<std::string, karshipta::v1::WardInfo> known_wards_;
};

#endif  // KARSHIPTA_GATEWAY_HERALD_WARD_MANAGER_H
