#include "ward_connection.h"

#include <chrono>
#include <memory>
#include <stop_token>

#include <gtest/gtest.h>
#include <mavsdk/mavsdk.h>

namespace {

// Each test uses its own loopback port so parallel test runs cannot cross-talk.
constexpr uint16_t kBasePort = 24540;

std::string listen_url(uint16_t port) {
    return "udpin://127.0.0.1:" + std::to_string(port);
}

// Stands in for PX4 SITL: an independent Mavsdk core that heartbeats as an
// autopilot with a chosen MAVLink system id, so system-id binding is testable
// without Docker or a real ward.
std::unique_ptr<mavsdk::Mavsdk> make_fake_autopilot(uint8_t system_id, uint16_t port) {
    mavsdk::Mavsdk::Configuration config{system_id, /*component_id=*/1,
                                         /*always_send_heartbeats=*/true};
    auto fake = std::make_unique<mavsdk::Mavsdk>(config);
    const auto result =
        fake->add_any_connection("udpout://127.0.0.1:" + std::to_string(port));
    EXPECT_EQ(result, mavsdk::ConnectionResult::Success);
    return fake;
}

}  // namespace

TEST(WardConnection, RejectsEmptyDroneUrl) {
    auto core = WardConnection::create_shared_core();
    EXPECT_THROW(WardConnection(core, ""), std::invalid_argument);
}

TEST(WardConnection, ConnectTimesOutWhenNothingListens) {
    auto core = WardConnection::create_shared_core();
    WardConnection ward(core, listen_url(kBasePort));
    EXPECT_EQ(ward.connect(), WardConnection::ConnectResult::kDiscoveryTimeout);
    EXPECT_FALSE(ward.is_connected());
}

TEST(WardConnection, DoesNotBindToMismatchedSystemId) {
    constexpr uint16_t port = kBasePort + 1;
    auto fake = make_fake_autopilot(/*system_id=*/7, port);

    auto core = WardConnection::create_shared_core();
    WardConnection ward(core, listen_url(port), /*expected_system_id=*/3);
    EXPECT_EQ(ward.connect(), WardConnection::ConnectResult::kDiscoveryTimeout);
    EXPECT_EQ(ward.get_system(), nullptr);
}

TEST(WardConnection, BindsToMatchingSystemId) {
    constexpr uint16_t port = kBasePort + 2;
    auto fake = make_fake_autopilot(/*system_id=*/3, port);

    auto core = WardConnection::create_shared_core();
    WardConnection ward(core, listen_url(port), /*expected_system_id=*/3);
    EXPECT_EQ(ward.connect(), WardConnection::ConnectResult::kSuccess);
    ASSERT_NE(ward.get_system(), nullptr);
    EXPECT_EQ(ward.get_system()->get_system_id(), 3);
}

TEST(WardConnection, DisconnectIsIdempotentAndSafeBeforeConnect) {
    auto core = WardConnection::create_shared_core();
    WardConnection ward(core, listen_url(kBasePort + 3));
    EXPECT_NO_THROW(ward.disconnect());
    EXPECT_NO_THROW(ward.disconnect());
    EXPECT_FALSE(ward.is_connected());
}

TEST(WardConnection, DisconnectIsIdempotentAfterConnect) {
    constexpr uint16_t port = kBasePort + 4;
    auto fake = make_fake_autopilot(/*system_id=*/1, port);

    auto core = WardConnection::create_shared_core();
    WardConnection ward(core, listen_url(port), /*expected_system_id=*/1);
    ASSERT_EQ(ward.connect(), WardConnection::ConnectResult::kSuccess);
    EXPECT_NO_THROW(ward.disconnect());
    EXPECT_FALSE(ward.is_connected());
    EXPECT_NO_THROW(ward.disconnect());
}

TEST(WardConnection, SubscribeBeforeConnectThrows) {
    auto core = WardConnection::create_shared_core();
    WardConnection ward(core, listen_url(kBasePort + 5));
    EXPECT_THROW(ward.subscribe_connection_state([](bool) {}), std::logic_error);
}
