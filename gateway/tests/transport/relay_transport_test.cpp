#include "relay_transport.h"
#include "websocket_transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr auto kHost = "127.0.0.1";

// Latch with a deadline so a broken transport fails the test instead of
// hanging it.
class Signal {
public:
    void notify() {
        {
            std::lock_guard lock(mutex_);
            fired_ = true;
        }
        cv_.notify_all();
    }
    bool wait(std::chrono::seconds timeout = std::chrono::seconds(5)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return fired_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool fired_ = false;
};

}  // namespace

// RelayTransport is a scaffold (gateway/docs/relay-transport.md): the Noise
// XX handshake and pairing relayly requires are not implemented yet. These
// tests stand a plain WebsocketTransport in for a real relayly server and
// only exercise the outbound-connect plumbing against it on loopback.
TEST(RelayTransport, ConnectsOutSendsAndReceivesFrames) {
    constexpr uint16_t port = 28768;
    WebsocketTransport fake_relay(kHost, port);

    Signal relay_connected, relay_received;
    std::atomic<Transport::ClientId> relay_side_id{0};
    std::atomic<size_t> relay_received_size{0};
    fake_relay.on_connect([&](const Transport::ClientId id) {
        relay_side_id = id;
        relay_connected.notify();
    });
    fake_relay.on_receive([&](Transport::ClientId, const std::vector<uint8_t>& bytes) {
        relay_received_size = bytes.size();
        relay_received.notify();
    });
    fake_relay.start();
    ASSERT_TRUE(fake_relay.is_running());

    RelayTransport transport("ws://" + std::string(kHost) + ":" + std::to_string(port),
                              RelayCredentials{"test-device", ""});
    Signal transport_connected, transport_received;
    std::atomic<Transport::ClientId> peer_id{0};
    std::atomic<size_t> transport_received_size{0};
    transport.on_connect([&](const Transport::ClientId id) {
        peer_id = id;
        transport_connected.notify();
    });
    transport.on_receive([&](Transport::ClientId, const std::vector<uint8_t>& bytes) {
        transport_received_size = bytes.size();
        transport_received.notify();
    });
    transport.start();
    ASSERT_TRUE(transport.is_running());

    ASSERT_TRUE(transport_connected.wait());
    ASSERT_TRUE(relay_connected.wait());

    transport.send(peer_id.load(), {0x01, 0x02, 0x03, 0x04});
    ASSERT_TRUE(relay_received.wait());
    EXPECT_EQ(relay_received_size, 4u);

    fake_relay.send(relay_side_id.load(), {0x10, 0x20, 0x30});
    ASSERT_TRUE(transport_received.wait());
    EXPECT_EQ(transport_received_size, 3u);

    transport.stop();
    EXPECT_FALSE(transport.is_running());
    fake_relay.stop();
}

TEST(RelayTransport, StopIsIdempotentAndStartAfterStopWorks) {
    constexpr uint16_t port = 28770;
    WebsocketTransport fake_relay(kHost, port);
    fake_relay.start();

    RelayTransport transport("ws://" + std::string(kHost) + ":" + std::to_string(port),
                              RelayCredentials{"test-device", ""});
    Signal connected;
    transport.on_connect([&](Transport::ClientId) { connected.notify(); });
    transport.start();
    ASSERT_TRUE(connected.wait());
    ASSERT_TRUE(transport.is_running());

    transport.stop();
    EXPECT_NO_THROW(transport.stop());
    EXPECT_FALSE(transport.is_running());

    transport.start();
    EXPECT_TRUE(transport.is_running());

    transport.stop();
    fake_relay.stop();
}

TEST(RelayTransport, RoleIsAlwaysOperator) {
    // No per-peer role concept exists yet (see the class comment and
    // transport.h): any client id, connected or not, reads as kOperator.
    RelayTransport transport("ws://" + std::string(kHost) + ":1", RelayCredentials{"test-device", ""});
    EXPECT_EQ(transport.role(/*client=*/0), Transport::ClientRole::kOperator);
    EXPECT_EQ(transport.role(/*client=*/42), Transport::ClientRole::kOperator);
}

TEST(RelayTransport, FromConfigMissingFileDefaultsToEmptyCredentials) {
    const auto path = std::filesystem::temp_directory_path() /
                       "karshipta_relay_test_missing_credentials.yaml";
    std::filesystem::remove(path);

    const auto transport = RelayTransport::from_config("ws://127.0.0.1:1", path.string());

    EXPECT_TRUE(transport->credentials().device_id.empty());
    EXPECT_TRUE(transport->credentials().private_key_path.empty());
}

TEST(RelayTransport, FromConfigLoadsFieldsFromFile) {
    const auto path =
        std::filesystem::temp_directory_path() / "karshipta_relay_test_credentials.yaml";
    {
        std::ofstream out(path, std::ios::trunc);
        out << "device_id: gateway-1\n";
        out << "private_key_path: /etc/karshipta/relay_device.key\n";
    }

    const auto transport = RelayTransport::from_config("ws://127.0.0.1:1", path.string());

    EXPECT_EQ(transport->credentials().device_id, "gateway-1");
    EXPECT_EQ(transport->credentials().private_key_path, "/etc/karshipta/relay_device.key");

    std::filesystem::remove(path);
}
