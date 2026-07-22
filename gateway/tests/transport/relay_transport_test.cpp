#include "relay_transport.h"

#include <relayly/errors.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace {
// Deliberately nothing is listening here: these tests must never actually
// reach a relay server.
constexpr auto kUnreachableUrl = "ws://127.0.0.1:1";
}  // namespace

// RelayTransport now wraps relayly's real C++ SDK (Noise XX handshake,
// pairing, peer key pinning, and reconnection all happen inside
// relayly::Client). Exercising a live connect or pairing flow needs a
// running relayly server, which this hermetic unit suite does not stand up;
// see gateway/docs/relay-transport.md's "Manual verification" section for
// that. These tests cover what is reachable without one: credential
// loading, the role() stub, and the guard that refuses to connect with
// incomplete credentials.

TEST(RelayTransport, RoleIsAlwaysOperator) {
    RelayTransport transport(kUnreachableUrl, RelayCredentials{"test-device", "", "test-token"});
    EXPECT_EQ(transport.role(/*client=*/0), Transport::ClientRole::kOperator);
    EXPECT_EQ(transport.role(/*client=*/42), Transport::ClientRole::kOperator);
}

TEST(RelayTransport, StartWithoutDeviceTokenDoesNotConnect) {
    RelayTransport transport(kUnreachableUrl, RelayCredentials{"test-device", "", ""});
    transport.start();
    EXPECT_FALSE(transport.is_running());
}

TEST(RelayTransport, StartWithoutDeviceIdDoesNotConnect) {
    RelayTransport transport(kUnreachableUrl, RelayCredentials{"", "", "test-token"});
    transport.start();
    EXPECT_FALSE(transport.is_running());
}

TEST(RelayTransport, StopWithoutStartIsIdempotent) {
    RelayTransport transport(kUnreachableUrl, RelayCredentials{"test-device", "", "test-token"});
    EXPECT_NO_THROW(transport.stop());
    EXPECT_NO_THROW(transport.stop());
    EXPECT_FALSE(transport.is_running());
}

TEST(RelayTransport, PairingBeforeStartThrows) {
    RelayTransport transport(kUnreachableUrl, RelayCredentials{"test-device", "", "test-token"});
    EXPECT_THROW(transport.request_pair_code(), relayly::Error);
    EXPECT_THROW(transport.accept_pair("483921"), relayly::Error);
}

TEST(RelayTransport, FromConfigMissingFileDefaultsToEmptyCredentials) {
    const auto path = std::filesystem::temp_directory_path() /
                       "karshipta_relay_test_missing_credentials.yaml";
    std::filesystem::remove(path);

    const auto transport = RelayTransport::from_config(kUnreachableUrl, path.string());

    EXPECT_TRUE(transport->credentials().device_id.empty());
    EXPECT_TRUE(transport->credentials().private_key_path.empty());
    EXPECT_TRUE(transport->credentials().device_token.empty());
}

TEST(RelayTransport, FromConfigLoadsFieldsFromFile) {
    const auto path =
        std::filesystem::temp_directory_path() / "karshipta_relay_test_credentials.yaml";
    {
        std::ofstream out(path, std::ios::trunc);
        out << "device_id: gateway-1\n";
        out << "private_key_path: /etc/karshipta/relay_device.key\n";
        out << "device_token: test-token-value\n";
    }

    const auto transport = RelayTransport::from_config(kUnreachableUrl, path.string());

    EXPECT_EQ(transport->credentials().device_id, "gateway-1");
    EXPECT_EQ(transport->credentials().private_key_path, "/etc/karshipta/relay_device.key");
    EXPECT_EQ(transport->credentials().device_token, "test-token-value");

    std::filesystem::remove(path);
}
