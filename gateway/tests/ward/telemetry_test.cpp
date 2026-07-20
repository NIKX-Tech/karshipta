#include "telemetry.h"

#include <memory>

#include <gtest/gtest.h>
#include <mavsdk/mavsdk.h>

#include "ward_connection.h"

// TelemetryInfo against a never-connected ward (no MAVSDK round trip
// needed): every subscribe_*()/unsubscribe_*() and getter must fail fast
// with a safe default rather than hang or crash. Against a real connection
// (a heartbeat-only fake autopilot, same pattern as
// ward_connection_test.cpp's make_fake_autopilot): subscribe/unsubscribe
// must not leave a dangling handle, and the destructor must safely
// unsubscribe an active subscription, the two cases
// gateway/docs/telemetry.md's "Automated tests" section named as worth
// covering.

namespace {

constexpr uint16_t kBasePort = 24950;

std::string listen_url(const uint16_t port) {
    return "udpin://127.0.0.1:" + std::to_string(port);
}

// Stands in for PX4 SITL: an independent Mavsdk core that heartbeats as an
// autopilot, enough for WardConnection::connect() to succeed and for a
// mavsdk::Telemetry plugin to be constructed against the resulting System.
// No telemetry data is actually published; these tests only exercise
// subscribe/unsubscribe/destructor safety, not the data path itself.
std::unique_ptr<mavsdk::Mavsdk> make_fake_autopilot(const uint8_t system_id, const uint16_t port) {
    const mavsdk::Mavsdk::Configuration config{system_id, /*component_id=*/1,
                                                /*always_send_heartbeats=*/true};
    auto fake = std::make_unique<mavsdk::Mavsdk>(config);
    const auto result = fake->add_any_connection("udpout://127.0.0.1:" + std::to_string(port));
    EXPECT_EQ(result, mavsdk::ConnectionResult::Success);
    return fake;
}

}  // namespace

TEST(TelemetryInfo, SubscribeAndUnsubscribeAreNoOpsWithoutConnection) {
    auto core = WardConnection::create_shared_core();
    WardConnection ward(core, listen_url(kBasePort));
    TelemetryInfo telemetry(ward);

    EXPECT_NO_THROW(telemetry.subscribe_position());
    EXPECT_NO_THROW(telemetry.unsubscribe_position());
    EXPECT_NO_THROW(telemetry.subscribe_flight_mode());
    EXPECT_NO_THROW(telemetry.unsubscribe_flight_mode());
    EXPECT_NO_THROW(telemetry.subscribe_battery());
    EXPECT_NO_THROW(telemetry.unsubscribe_battery());
}

TEST(TelemetryInfo, GettersReturnSafeDefaultsWithoutConnection) {
    auto core = WardConnection::create_shared_core();
    WardConnection ward(core, listen_url(kBasePort + 1));
    TelemetryInfo telemetry(ward);

    EXPECT_FALSE(telemetry.check_for_calibration());
    EXPECT_FALSE(telemetry.is_armed());
    EXPECT_FALSE(telemetry.is_in_air());
    EXPECT_FALSE(telemetry.is_health_ok());
    EXPECT_EQ(telemetry.get_flight_mode(), mavsdk::Telemetry::FlightMode::Unknown);
}

TEST(TelemetryInfo, DestructorSafeWithoutConnection) {
    auto core = WardConnection::create_shared_core();
    WardConnection ward(core, listen_url(kBasePort + 2));
    EXPECT_NO_THROW({ TelemetryInfo telemetry(ward); });
}

TEST(TelemetryInfo, SubscribeThenUnsubscribeTwiceDoesNotLeaveDanglingHandle) {
    constexpr uint16_t port = kBasePort + 3;
    auto fake = make_fake_autopilot(/*system_id=*/1, port);

    auto core = WardConnection::create_shared_core();
    WardConnection ward(core, listen_url(port), /*expected_system_id=*/1);
    ASSERT_EQ(ward.connect(), WardConnection::ConnectResult::kSuccess);

    TelemetryInfo telemetry(ward);
    EXPECT_NO_THROW(telemetry.subscribe_position());
    EXPECT_NO_THROW(telemetry.unsubscribe_position());
    // Idempotent: a second unsubscribe with no active handle must not touch
    // MAVSDK's unsubscribe_position() with a stale handle.
    EXPECT_NO_THROW(telemetry.unsubscribe_position());
}

TEST(TelemetryInfo, DestructorUnsubscribesActivePositionSubscription) {
    constexpr uint16_t port = kBasePort + 4;
    auto fake = make_fake_autopilot(/*system_id=*/2, port);

    auto core = WardConnection::create_shared_core();
    WardConnection ward(core, listen_url(port), /*expected_system_id=*/2);
    ASSERT_EQ(ward.connect(), WardConnection::ConnectResult::kSuccess);

    EXPECT_NO_THROW({
        TelemetryInfo telemetry(ward);
        telemetry.subscribe_position();
        telemetry.subscribe_flight_mode();
        telemetry.subscribe_battery();
        // Destructor runs here with all three subscriptions still active.
    });
}
