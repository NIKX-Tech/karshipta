#include "websocket_transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

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

std::unique_ptr<ix::WebSocket> make_client(uint16_t port) {
    auto client = std::make_unique<ix::WebSocket>();
    client->setUrl("ws://" + std::string(kHost) + ":" + std::to_string(port));
    client->disableAutomaticReconnection();
    return client;
}

}  // namespace

TEST(WebsocketTransport, ConnectDeliversFrameAndDisconnectMatchesId) {
    constexpr uint16_t port = 28765;
    WebsocketTransport transport(kHost, port);

    std::atomic<Transport::ClientId> connected_id{0};
    std::atomic<Transport::ClientId> disconnected_id{0};
    Signal connected, received, disconnected;

    transport.on_connect([&](const Transport::ClientId client) {
        connected_id = client;
        connected.notify();
        transport.send(client, {0x01, 0x02, 0x03});
    });
    transport.on_disconnect([&](const Transport::ClientId client) {
        disconnected_id = client;
        disconnected.notify();
    });
    transport.start();
    ASSERT_TRUE(transport.is_running());

    auto client = make_client(port);
    std::atomic<size_t> frame_size{0};
    client->setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message && msg->binary) {
            frame_size = msg->str.size();
            received.notify();
        }
    });
    client->start();

    ASSERT_TRUE(connected.wait());
    ASSERT_TRUE(received.wait());
    EXPECT_EQ(frame_size, 3u);

    client->stop();
    ASSERT_TRUE(disconnected.wait());
    EXPECT_EQ(disconnected_id.load(), connected_id.load());

    transport.stop();
    EXPECT_FALSE(transport.is_running());
}

TEST(WebsocketTransport, BroadcastReachesEveryClientAndReceiveRoundTrips) {
    constexpr uint16_t port = 28766;
    WebsocketTransport transport(kHost, port);

    std::atomic<int> connect_count{0};
    Signal two_connected, frame_received;
    std::atomic<size_t> received_size{0};

    transport.on_connect([&](Transport::ClientId) {
        if (connect_count.fetch_add(1) + 1 == 2) two_connected.notify();
    });
    transport.on_receive([&](Transport::ClientId, const std::vector<uint8_t>& bytes) {
        received_size = bytes.size();
        frame_received.notify();
    });
    transport.start();

    auto first = make_client(port);
    auto second = make_client(port);
    Signal first_got, second_got;
    first->setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message && msg->binary) first_got.notify();
    });
    second->setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Message && msg->binary) second_got.notify();
    });
    first->start();
    second->start();
    ASSERT_TRUE(two_connected.wait());

    transport.broadcast({0xAA, 0xBB});
    EXPECT_TRUE(first_got.wait());
    EXPECT_TRUE(second_got.wait());

    first->sendBinary(std::string("\x10\x20\x30\x40", 4));
    ASSERT_TRUE(frame_received.wait());
    EXPECT_EQ(received_size, 4u);

    first->stop();
    second->stop();
    transport.stop();
}

TEST(WebsocketTransport, StopIsIdempotentAndStartAfterStopWorks) {
    constexpr uint16_t port = 28767;
    WebsocketTransport transport(kHost, port);
    transport.start();
    ASSERT_TRUE(transport.is_running());
    transport.stop();
    EXPECT_NO_THROW(transport.stop());
    transport.start();
    EXPECT_TRUE(transport.is_running());
    transport.stop();
}
