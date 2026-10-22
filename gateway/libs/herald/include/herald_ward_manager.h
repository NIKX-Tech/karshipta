#ifndef KARSHIPTA_GATEWAY_HERALD_WARD_MANAGER_H
#define KARSHIPTA_GATEWAY_HERALD_WARD_MANAGER_H

#include <herald/v0/herald.pb.h>
#include <karshipta/v1/telemetry.pb.h>
#include <transport.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

// Forward-declared, not included: this header only ever holds a WardManager*
// (never dereferenced here), so it does not need WardManager's full
// definition, which transitively pulls in MAVSDK. Keeping the include out
// lets this library compile with zero MAVSDK dependency in
// KARSHIPTA_GATEWAY_ENABLE_MAVLINK=OFF builds (see root CMakeLists.txt);
// herald_ward_manager.cpp includes the real header only where it actually
// calls a WardManager method, guarded by that same flag.
class WardManager;

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
    // karshipta#268: a Herald source has no MAVLink-style link to watch, just
    // messages that stop arriving, so staleness is detected by elapsed time
    // since the last one instead. 30s is a starting default, generous enough
    // for a slow cellular store-and-forward tracker without flagging a
    // merely-quiet one as gone; making this configurable per source via
    // gateway.yaml (the issue's own "configurable" wording) is follow-up
    // work once this mechanism has real-world tuning data behind it, not
    // part of this foundation.
    static constexpr std::chrono::milliseconds kDefaultStaleTimeout{30'000};

#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
    // ward_manager is consulted (read-only, via has_ward()) to reject a
    // Herald message whose entity_id collides with an existing MAVLink
    // ward_id; this class never mutates it. stale_timeout is a constructor
    // parameter (not just a constant) so tests can use a short one instead
    // of waiting out a real 30s window.
    explicit HeraldWardManager(Transport& transport, WardManager& ward_manager,
                               std::chrono::milliseconds stale_timeout = kDefaultStaleTimeout);
#else
    // MAVLink-disabled builds (KARSHIPTA_GATEWAY_ENABLE_MAVLINK=OFF, see root
    // CMakeLists.txt): no WardManager exists to collide-check against, since
    // there are no MAVLink ward_ids in that build at all. Only this
    // constructor exists here (not the WardManager&-taking one above,
    // conditionally compiled out) - a build where WardManager barely exists
    // as a type should not expose a constructor that implies it does.
    explicit HeraldWardManager(Transport& transport,
                               std::chrono::milliseconds stale_timeout = kDefaultStaleTimeout);
#endif

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
#ifdef KARSHIPTA_GATEWAY_ENABLE_MAVLINK
    // Never owned by this class. Conditionally compiled out entirely in
    // MAVLink-disabled builds (rather than kept as a nullable pointer marked
    // [[maybe_unused]]): GCC rejects that attribute on a data member here
    // ('maybe_unused' attribute ignored, -Werror=attributes) even though
    // it's valid per the standard, and since the two constructors are
    // already mutually exclusive per build (see above), there is no reason
    // for this to be nullable at all - a plain reference is simpler and
    // portable across both compilers this repo builds with.
    WardManager& ward_manager_;
#endif
    // karshipta#268: what the staleness checker needs per ward - the state
    // last broadcast for it (so a stale flip can re-send the same reading
    // with only connected() changed, not fabricate one) and when that
    // arrived, in steady_clock rather than wall-clock terms specifically so
    // a system clock jump (NTP sync, DST, manual change) can never produce
    // a false stale flip or a false recovery. marked_stale guards against
    // re-broadcasting the same "now stale" state on every check tick once
    // it has already been sent once.
    struct LastSeen {
        karshipta::v1::WardState last_state;
        std::chrono::steady_clock::time_point last_seen_at;
        bool marked_stale = false;
    };

    mutable std::mutex mutex_;
    // Every entity_id ingest() has ever built a WardInfo for, so a repeat
    // message from the same source doesn't re-announce it. In-memory only,
    // unlike WardManager's persisted fleet config: a Herald ward's identity
    // is defined entirely by "it has posted a message," so there is nothing
    // meaningful to persist across a restart.
    std::map<std::string, karshipta::v1::WardInfo> known_wards_;
    // karshipta#268: one entry per entity_id ever ingested, updated on every
    // ingest() and read by the staleness checker below. Same in-memory-only
    // reasoning as known_wards_ above.
    std::map<std::string, LastSeen> last_seen_;
    const std::chrono::milliseconds stale_timeout_;
    // How often the checker wakes to look for a ward that just crossed
    // stale_timeout_. A quarter of the timeout keeps the detection delay
    // small relative to the timeout itself without busy-looping; the 50ms
    // floor matters for tests using a short stale_timeout_, where
    // timeout/4 alone would otherwise round down to an unusably tight loop.
    [[nodiscard]] std::chrono::milliseconds stale_check_interval() const {
        return std::max(stale_timeout_ / 4, std::chrono::milliseconds(50));
    }

    // Body of stale_checker_, started in the constructor.
    void run_stale_checker(std::stop_token stop_token);

    // karshipta#276: "accept but flag," the same philosophy
    // isStaleSample() already applies on the console side (karshipta#268) -
    // never reject a sample outright (rejecting risks permanently
    // "poisoning" a ward whose very first message happened to carry a bad
    // clock), just surface that something looks wrong via the existing
    // Event/WARNING mechanism and let the operator judge. Mirrors
    // WardManager::broadcast_mission_event's own shape.
    void broadcast_herald_event(const std::string& ward_id, const std::string& code,
                                const std::string& message) const;

    // Declared last (after mutex_/known_wards_/last_seen_, which it reads,
    // and after stale_timeout_, which it only reads too): members destroy
    // in reverse declaration order, so this stops and joins first, before
    // the state it touches is torn down. Same reasoning WardManager's own
    // geofence_mutex_/managed_wards_ ordering bug (karshipta#270 writeup)
    // surfaced the hard way - getting it right here from the start instead
    // of after a crash.
    std::jthread stale_checker_;
};

#endif  // KARSHIPTA_GATEWAY_HERALD_WARD_MANAGER_H
