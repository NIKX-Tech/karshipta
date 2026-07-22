#include "herald_ward_manager.h"

#include <gtest/gtest.h>
#include <herald/v0/herald.pb.h>
#include <karshipta/v1/envelope.pb.h>

#include <cstddef>
#include <mutex>
#include <string>
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
