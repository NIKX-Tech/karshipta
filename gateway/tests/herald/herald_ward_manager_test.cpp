#include "herald_ward_manager.h"

#include <gtest/gtest.h>
#include <herald/v0/herald.pb.h>
#include <karshipta/v1/envelope.pb.h>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "transport.h"
#include "ward_connection.h"
#include "ward_manager.h"

namespace {

// Records every broadcast()/send() call instead of touching a real socket.
// Duplicated from ward_manager_test.cpp's own FakeTransport rather than
// shared across test binaries for one small class, matching this repo's
// existing serialize_envelope-style duplication precedent.
class FakeTransport : public Transport {
   public:
    void start() override { running_ = true; }
    void stop() override { running_ = false; }
    [[nodiscard]] bool is_running() const override { return running_; }

    void send(ClientId client, const std::vector<uint8_t>& bytes) override {
        std::lock_guard lock(mutex_);
        sent_.emplace_back(client, bytes);
    }
    void broadcast(const std::vector<uint8_t>& bytes) override {
        std::lock_guard lock(mutex_);
        broadcast_.push_back(bytes);
    }
    // Not exercised by these tests; satisfies the interface.
    void disconnect(ClientId /*client*/) override {}

    void on_receive(ReceiveCallback callback) override { receive_callback_ = std::move(callback); }
    void on_connect(ConnectCallback callback) override { connect_callback_ = std::move(callback); }
    void on_disconnect(DisconnectCallback callback) override {
        disconnect_callback_ = std::move(callback);
    }

    [[nodiscard]] ClientRole role(ClientId /*client*/) const override {
        return ClientRole::kOperator;
    }

    [[nodiscard]] std::vector<karshipta::v1::Envelope> broadcast_envelopes() const {
        std::lock_guard lock(mutex_);
        std::vector<karshipta::v1::Envelope> envelopes;
        for (const auto& bytes : broadcast_) {
            karshipta::v1::Envelope envelope;
            if (envelope.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
                envelopes.push_back(envelope);
            }
        }
        return envelopes;
    }

    [[nodiscard]] std::vector<karshipta::v1::Envelope> sent_envelopes(const ClientId client) const {
        std::lock_guard lock(mutex_);
        std::vector<karshipta::v1::Envelope> envelopes;
        for (const auto& entry : sent_) {
            if (entry.first != client) continue;
            karshipta::v1::Envelope envelope;
            if (envelope.ParseFromArray(entry.second.data(),
                                        static_cast<int>(entry.second.size()))) {
                envelopes.push_back(envelope);
            }
        }
        return envelopes;
    }

   private:
    mutable std::mutex mutex_;
    bool running_ = false;
    std::vector<std::pair<ClientId, std::vector<uint8_t>>> sent_;
    std::vector<std::vector<uint8_t>> broadcast_;
    ReceiveCallback receive_callback_;
    ConnectCallback connect_callback_;
    DisconnectCallback disconnect_callback_;
};

herald::v0::Herald make_herald(
    const std::string& entity_id,
    const herald::v0::EntityClass entity_class = herald::v0::ENTITY_CLASS_LIVESTOCK_TAG) {
    herald::v0::Herald msg;
    msg.set_entity_id(entity_id);
    msg.set_timestamp_ms(1700000000000ULL);
    msg.set_entity_class(entity_class);
    msg.mutable_position()->set_latitude_deg(52.37);
    msg.mutable_position()->set_longitude_deg(4.90);
    msg.set_health_ok(true);
    msg.set_org_id("demo");
    return msg;
}

// Base fixture: a WardManager (never connected, same "construction only, no
// connect()" pattern as ward_manager_test.cpp) and a HeraldWardManager
// sharing one FakeTransport, so tests can exercise the ward_id collision
// path between the two without any real MAVSDK network activity.
class HeraldWardManagerTest : public ::testing::Test {
   protected:
    HeraldWardManagerTest()
        : core_(WardConnection::create_shared_core()),
          ward_manager_(core_, transport_),
          herald_manager_(transport_, ward_manager_) {}

    std::shared_ptr<mavsdk::Mavsdk> core_;
    FakeTransport transport_;
    WardManager ward_manager_;
    HeraldWardManager herald_manager_;
};

}  // namespace

TEST_F(HeraldWardManagerTest, IngestBroadcastsWardInfoThenWardState) {
    const auto result = herald_manager_.ingest(make_herald("tag-1"));
    EXPECT_EQ(result, HeraldIngestResult::kOk);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 2u);

    ASSERT_TRUE(envelopes[0].has_ward_info());
    const auto& info = envelopes[0].ward_info();
    EXPECT_EQ(info.ward_id(), "tag-1");
    EXPECT_EQ(info.ward_class(), karshipta::v1::WARD_CLASS_LIVESTOCK_TAG);
    EXPECT_EQ(info.origin(), karshipta::v1::WARD_ORIGIN_HARDWARE);
    EXPECT_EQ(info.mavlink_system_id(), 0u);
    EXPECT_TRUE(info.autopilot().empty());

    ASSERT_TRUE(envelopes[1].has_ward_state());
    const auto& state = envelopes[1].ward_state();
    EXPECT_EQ(state.ward_id(), "tag-1");
    EXPECT_TRUE(state.health_ok());
    EXPECT_TRUE(state.connected());
    EXPECT_FALSE(state.has_flight());
    EXPECT_DOUBLE_EQ(state.position().latitude_deg(), 52.37);
}

TEST_F(HeraldWardManagerTest, SecondMessageFromSameEntityDoesNotResendWardInfo) {
    ASSERT_EQ(herald_manager_.ingest(make_herald("tag-1")), HeraldIngestResult::kOk);
    ASSERT_EQ(herald_manager_.ingest(make_herald("tag-1")), HeraldIngestResult::kOk);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 3u);
    EXPECT_TRUE(envelopes[0].has_ward_info());
    EXPECT_TRUE(envelopes[1].has_ward_state());
    EXPECT_TRUE(envelopes[2].has_ward_state());
}

TEST_F(HeraldWardManagerTest, RejectsEntityIdCollidingWithMavlinkWard) {
    WardConfig collision_cfg;
    collision_cfg.ward_id = "alpha-1";
    collision_cfg.connection_url = "udpin://127.0.0.1:25990";
    ASSERT_TRUE(ward_manager_.add_ward(collision_cfg));

    const auto result = herald_manager_.ingest(make_herald("alpha-1"));
    EXPECT_EQ(result, HeraldIngestResult::kWardIdCollision);
    EXPECT_TRUE(transport_.broadcast_envelopes().empty());
}

TEST_F(HeraldWardManagerTest, SendKnownWardsReplaysWardInfoToNewClient) {
    ASSERT_EQ(herald_manager_.ingest(make_herald("tag-1")), HeraldIngestResult::kOk);

    herald_manager_.send_known_wards(/*client=*/42);

    const auto envelopes = transport_.sent_envelopes(42);
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_ward_info());
    EXPECT_EQ(envelopes.front().ward_info().ward_id(), "tag-1");
}

TEST_F(HeraldWardManagerTest, MapsEveryEntityClassToExpectedWardClass) {
    const std::vector<std::pair<herald::v0::EntityClass, karshipta::v1::WardClass>> cases = {
        {herald::v0::ENTITY_CLASS_UNSPECIFIED, karshipta::v1::WARD_CLASS_UNSPECIFIED},
        {herald::v0::ENTITY_CLASS_MULTIROTOR, karshipta::v1::WARD_CLASS_MULTIROTOR},
        {herald::v0::ENTITY_CLASS_FIXED_WING, karshipta::v1::WARD_CLASS_FIXED_WING},
        {herald::v0::ENTITY_CLASS_VTOL, karshipta::v1::WARD_CLASS_VTOL},
        {herald::v0::ENTITY_CLASS_HELICOPTER, karshipta::v1::WARD_CLASS_HELICOPTER},
        {herald::v0::ENTITY_CLASS_GROUND_VEHICLE, karshipta::v1::WARD_CLASS_GROUND},
        {herald::v0::ENTITY_CLASS_LIVESTOCK_TAG, karshipta::v1::WARD_CLASS_LIVESTOCK_TAG},
        {herald::v0::ENTITY_CLASS_GENERIC_TRACKER, karshipta::v1::WARD_CLASS_GENERIC_TRACKER},
    };

    int index = 0;
    for (const auto& test_case : cases) {
        const std::string entity_id = "class-test-" + std::to_string(index++);
        ASSERT_EQ(herald_manager_.ingest(make_herald(entity_id, test_case.first)),
                  HeraldIngestResult::kOk);
    }

    const auto envelopes = transport_.broadcast_envelopes();
    // Every entity_id above is unique, so each case is a first sight: one
    // WardInfo followed by one WardState, in order.
    ASSERT_EQ(envelopes.size(), cases.size() * 2);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        ASSERT_TRUE(envelopes[i * 2].has_ward_info());
        EXPECT_EQ(envelopes[i * 2].ward_info().ward_class(), cases[i].second);
    }
}

// karshipta#268: timestamp_ms (device/event time, from the payload) and
// ingestion_ms (gateway wall-clock at receipt) are independent fields now;
// make_herald's fixed timestamp_ms (year 2023) proves this isn't just
// echoing the same value under a new name - a real gateway receiving that
// message today has an ingestion_ms far later than its timestamp_ms.
TEST_F(HeraldWardManagerTest, SetsIngestionTimeIndependentlyOfDeviceEventTime) {
    ASSERT_EQ(herald_manager_.ingest(make_herald("tag-1")), HeraldIngestResult::kOk);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 2u);
    const auto& state = envelopes[1].ward_state();
    EXPECT_EQ(state.timestamp_ms(), 1700000000000ULL);
    EXPECT_GT(state.ingestion_ms(), state.timestamp_ms());
}

// Fixture for staleness tests: a short stale_timeout_ so these don't have to
// wait out the real 30s default. Separate from HeraldWardManagerTest (which
// keeps the production default) since none of its own tests run anywhere
// near long enough for staleness to matter to them either way.
class HeraldWardManagerStalenessTest : public ::testing::Test {
   protected:
    HeraldWardManagerStalenessTest()
        : core_(WardConnection::create_shared_core()),
          ward_manager_(core_, transport_),
          herald_manager_(transport_, ward_manager_, kStaleTimeout) {}

    static constexpr std::chrono::milliseconds kStaleTimeout{100};

    std::shared_ptr<mavsdk::Mavsdk> core_;
    FakeTransport transport_;
    WardManager ward_manager_;
    HeraldWardManager herald_manager_;
};

TEST_F(HeraldWardManagerStalenessTest, FlipsConnectedFalseAfterTimeoutWithNoNewMessage) {
    ASSERT_EQ(herald_manager_.ingest(make_herald("tag-1")), HeraldIngestResult::kOk);

    // Poll rather than a fixed sleep: the checker thread's own wake interval
    // is derived from kStaleTimeout (see stale_check_interval()), not a
    // value this test controls directly, so a fixed sleep would be guessing
    // at its schedule instead of just waiting for the effect.
    bool stale_broadcast_seen = false;
    for (int attempt = 0; attempt < 100 && !stale_broadcast_seen; ++attempt) {
        for (const auto& envelope : transport_.broadcast_envelopes()) {
            if (envelope.has_ward_state() && !envelope.ward_state().connected()) {
                stale_broadcast_seen = true;
                break;
            }
        }
        if (!stale_broadcast_seen) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(stale_broadcast_seen) << "no stale (connected=false) WardState ever broadcast";

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_GE(envelopes.size(),
              3u);  // info, first state(connected=true), stale state(connected=false)
    const auto& stale_state = envelopes.back().ward_state();
    EXPECT_EQ(stale_state.ward_id(), "tag-1");
    EXPECT_FALSE(stale_state.connected());
    // The last-known reading is re-sent, not fabricated: position survives
    // onto the stale broadcast unchanged.
    EXPECT_DOUBLE_EQ(stale_state.position().latitude_deg(), 52.37);
}

TEST_F(HeraldWardManagerStalenessTest, RecoversOnNextMessageAfterGoingStale) {
    ASSERT_EQ(herald_manager_.ingest(make_herald("tag-1")), HeraldIngestResult::kOk);

    bool stale_broadcast_seen = false;
    for (int attempt = 0; attempt < 100 && !stale_broadcast_seen; ++attempt) {
        for (const auto& envelope : transport_.broadcast_envelopes()) {
            if (envelope.has_ward_state() && !envelope.ward_state().connected()) {
                stale_broadcast_seen = true;
                break;
            }
        }
        if (!stale_broadcast_seen) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(stale_broadcast_seen) << "no stale (connected=false) WardState ever broadcast";

    ASSERT_EQ(herald_manager_.ingest(make_herald("tag-1")), HeraldIngestResult::kOk);

    const auto envelopes = transport_.broadcast_envelopes();
    EXPECT_TRUE(envelopes.back().ward_state().connected());
}

TEST_F(HeraldWardManagerStalenessTest, NeverGoesStaleWhileMessagesKeepArriving) {
    // Re-ingests faster than kStaleTimeout, well past when a ward with no
    // staleness-reset logic would have already flipped once - proves the
    // checker is actually looking at last_seen_at, not just firing once per
    // ward unconditionally after some fixed delay from first sight.
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(herald_manager_.ingest(make_herald("tag-1")), HeraldIngestResult::kOk);
        std::this_thread::sleep_for(HeraldWardManagerStalenessTest::kStaleTimeout / 4);
    }

    for (const auto& envelope : transport_.broadcast_envelopes()) {
        if (envelope.has_ward_state()) {
            EXPECT_TRUE(envelope.ward_state().connected());
        }
    }
}

// karshipta#268: these three tests document ingest()'s current, deliberately
// unvalidated behavior against the forum thread's own validation matrix
// ("delayed or out-of-order updates", "replayed older samples", "device
// clock skew or invalid timestamps") - they are not regression tests for a
// bug fix, since no fix exists yet. ingest() has no concept of "this
// message's timestamp_ms is older than the last one I accepted for this
// ward" at all: every message is accepted, stored, and broadcast exactly as
// it arrives, unconditionally. These tests make that fact explicit and
// checkable rather than implicit, so a future decision about whether to add
// ordering/replay/clock-skew protection starts from a documented baseline,
// not a guess. See the follow-up issue filed alongside these tests for the
// actual design question (reject vs. flag vs. leave as-is) - deliberately
// not decided here: rejecting an out-of-order message outright risks
// permanently "poisoning" a ward's state if the very first message a
// tracker ever sends happens to carry a bad clock, since every later,
// correct message would then look older by comparison.
TEST_F(HeraldWardManagerTest, AcceptsAnOlderTimestampArrivingAfterANewerOneWithNoProtection) {
    auto newer = make_herald("tag-1");
    newer.set_timestamp_ms(2'000'000'000'000ULL);
    newer.mutable_position()->set_latitude_deg(60.0);
    ASSERT_EQ(herald_manager_.ingest(newer), HeraldIngestResult::kOk);

    // Same entity_id, an event timestamp a full day older than the message
    // just accepted - modeling either a delayed/out-of-order delivery or an
    // exact replay of an earlier reading, which look identical from
    // ingest()'s point of view (it only ever looks at the message in front
    // of it, never compares against what it last accepted).
    auto older = make_herald("tag-1");
    older.set_timestamp_ms(newer.timestamp_ms() - 86'400'000);
    older.mutable_position()->set_latitude_deg(10.0);
    ASSERT_EQ(herald_manager_.ingest(older), HeraldIngestResult::kOk);

    const auto envelopes = transport_.broadcast_envelopes();
    const auto& latest_state = envelopes.back().ward_state();
    // The console now displays the OLDER reading as current, having
    // overwritten the newer one with no warning anywhere in the wire data -
    // confirmed here, not assumed. connected() stays true throughout: this
    // is not a staleness-timeout scenario, the ward is reporting normally
    // the whole time, just with a message that arrived out of event order.
    EXPECT_DOUBLE_EQ(latest_state.position().latitude_deg(), 10.0);
    EXPECT_EQ(latest_state.timestamp_ms(), older.timestamp_ms());
    EXPECT_TRUE(latest_state.connected());
}

TEST_F(HeraldWardManagerTest, AcceptsAFarFutureTimestampWithNoClockSkewValidation) {
    // A device clock far enough ahead of the gateway's own that
    // ingestion_ms - timestamp_ms (the exact quantity isStaleSample() on the
    // console reads, see ward-format.ts) comes out negative - "this sample
    // arrived before its own recorded event time," which should be
    // impossible and is exactly the kind of bad-clock signal the matrix's
    // "device clock skew or invalid timestamps" item asks about. ingest()
    // has no sanity check on timestamp_ms at all; this documents that, not
    // a bug fix.
    auto msg = make_herald("tag-1");
    const uint64_t now_ms =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count());
    msg.set_timestamp_ms(now_ms + 3'600'000);  // 1 hour in the future
    ASSERT_EQ(herald_manager_.ingest(msg), HeraldIngestResult::kOk);

    const auto envelopes = transport_.broadcast_envelopes();
    const auto& state = envelopes.back().ward_state();
    EXPECT_EQ(state.timestamp_ms(), msg.timestamp_ms());
    // ingestion_ms is the gateway's own real wall clock, so this is
    // reliably negative when timestamp_ms claims to be in the future -
    // exactly the nonsensical value a clock-skew check would need to catch.
    const int64_t event_to_ingestion_gap_ms =
        static_cast<int64_t>(state.ingestion_ms()) - static_cast<int64_t>(state.timestamp_ms());
    EXPECT_LT(event_to_ingestion_gap_ms, 0);
}
