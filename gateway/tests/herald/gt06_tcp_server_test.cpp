#include "gt06_tcp_server.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXSocket.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <karshipta/v1/envelope.pb.h>

#include "transport.h"
#include "ward_connection.h"
#include "ward_manager.h"

namespace {

constexpr auto kHost = "127.0.0.1";

// Records every broadcast() call instead of touching a real socket.
// Duplicated per this repo's existing FakeTransport-per-test-file
// precedent (see herald_ward_manager_test.cpp/herald_http_server_test.cpp).
class FakeTransport : public Transport {
   public:
    void start() override { running_ = true; }
    void stop() override { running_ = false; }
    [[nodiscard]] bool is_running() const override { return running_; }

    void send(ClientId /*client*/, const std::vector<uint8_t>& /*bytes*/) override {}
    void broadcast(const std::vector<uint8_t>& bytes) override {
        std::lock_guard lock(mutex_);
        broadcast_.push_back(bytes);
    }
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

   private:
    mutable std::mutex mutex_;
    bool running_ = false;
    std::vector<std::vector<uint8_t>> broadcast_;
    ReceiveCallback receive_callback_;
    ConnectCallback connect_callback_;
    DisconnectCallback disconnect_callback_;
};

// IMEI 861845041234567, device type 0x0102, serial 0x0001 - byte-identical
// to gt06_parser_test.cpp's LoginPacketDecodesImei vector (independently
// CRC-verified there, see that file's own comment); reused rather than
// re-derived so this file only has to prove the server wires real bytes on
// a real socket into Gt06Parser + HeraldWardManager correctly, not
// re-relitigate the frame format itself.
const std::vector<uint8_t> kLoginFrame = {0x78, 0x78, 0x0F, 0x01, 0x08, 0x61, 0x84, 0x50, 0x41,
                                           0x23, 0x45, 0x67, 0x01, 0x02, 0x00, 0x01, 0x58, 0x5C,
                                           0x0D, 0x0A};
// The exact ack this login frame produces, from gt06_parser_test.cpp's own
// BuildAckForLoginProducesTheExactExpectedBytes.
const std::vector<uint8_t> kExpectedLoginAck = {0x78, 0x78, 0x05, 0x01, 0x00,
                                                 0x01, 0xD9, 0xDC, 0x0D, 0x0A};
// 2024-03-15T12:30:45 UTC, 52.37N 4.90E, 8 satellites, valid fix, serial
// 0x0002 - byte-identical to gt06_parser_test.cpp's
// LocationPacketDecodesNorthEastPosition vector.
const std::vector<uint8_t> kLocationFrame = {0x78, 0x78, 0x18, 0x10, 0x18, 0x03, 0x0F, 0x0C, 0x1E,
                                              0x2D, 0x0C, 0x08, 0x05, 0x9E, 0x62, 0x90, 0x00, 0x86,
                                              0x95, 0x20, 0x05, 0x14, 0x5A, 0x00, 0x02, 0x4E, 0xE0,
                                              0x0D, 0x0A};

// Connects, sends every frame in order, and returns everything the server
// sent back within a short window (long enough for a real loopback round
// trip, short enough not to stall the suite on a bug that stops the server
// from ever replying). A free function, not a fixture method, so both the
// TEST_F cases below and the standalone FromConfig cases (which build their
// own Gt06TcpServer, not the fixture's) can share it.
std::vector<uint8_t> send_frames_and_collect_replies(const uint16_t port,
                                                      const std::vector<std::vector<uint8_t>>& frames) {
    ix::Socket socket;
    std::string error;
    const ix::CancellationRequest never_cancel = [] { return false; };
    if (!socket.connect(kHost, port, error, never_cancel)) return {};

    for (const auto& frame : frames) {
        socket.send(reinterpret_cast<char*>(const_cast<uint8_t*>(frame.data())), frame.size());
    }

    std::vector<uint8_t> received;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::array<uint8_t, 256> chunk{};
    while (std::chrono::steady_clock::now() < deadline) {
        if (socket.isReadyToRead(200) != ix::PollResultType::ReadyForRead) continue;
        const ssize_t n = socket.recv(chunk.data(), chunk.size());
        if (n <= 0) break;
        received.insert(received.end(), chunk.begin(), chunk.begin() + n);
        // The only reply either frame in this file provokes is the one
        // login ack (kExpectedLoginAck); once that much is in, stop
        // waiting instead of blocking out the rest of the deadline.
        if (received.size() >= kExpectedLoginAck.size()) break;
    }
    socket.close();
    return received;
}

class Gt06TcpServerTest : public ::testing::Test {
   protected:
    Gt06TcpServerTest()
        : core_(WardConnection::create_shared_core()),
          ward_manager_(core_, transport_),
          herald_manager_(transport_, ward_manager_),
          server_(herald_manager_, kHost, port_) {
        server_.start();
    }

    ~Gt06TcpServerTest() override { server_.stop(); }

    static uint16_t next_port() {
        static uint16_t port = 28800;
        return port++;
    }

    uint16_t port_ = next_port();
    std::shared_ptr<mavsdk::Mavsdk> core_;
    FakeTransport transport_;
    WardManager ward_manager_;
    HeraldWardManager herald_manager_;
    Gt06TcpServer server_;
};

}  // namespace

TEST_F(Gt06TcpServerTest, LoginFrameProducesExactAckBytes) {
    const auto received = send_frames_and_collect_replies(port_, {kLoginFrame});
    EXPECT_EQ(received, kExpectedLoginAck);
}

TEST_F(Gt06TcpServerTest, LoginThenLocationIngestsHeraldWithImeiAsEntityId) {
    (void)send_frames_and_collect_replies(port_, {kLoginFrame, kLocationFrame});

    // Ingestion happens on the connection's own worker thread as frames are
    // parsed, which can trail slightly behind the ack byte-count cutoff
    // send_frames_and_collect_replies() stops reading at; poll briefly
    // rather than asserting immediately.
    std::vector<karshipta::v1::Envelope> envelopes;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        envelopes = transport_.broadcast_envelopes();
        if (envelopes.size() >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ASSERT_EQ(envelopes.size(), 2u);
    ASSERT_TRUE(envelopes[0].has_ward_info());
    EXPECT_EQ(envelopes[0].ward_info().ward_id(), "861845041234567");
    ASSERT_TRUE(envelopes[1].has_ward_state());
    const auto& state = envelopes[1].ward_state();
    EXPECT_EQ(state.ward_id(), "861845041234567");
    EXPECT_NEAR(state.position().latitude_deg(), 52.370000, 1e-4);
    EXPECT_NEAR(state.position().longitude_deg(), 4.900000, 1e-4);
    EXPECT_TRUE(state.health_ok());
}

TEST_F(Gt06TcpServerTest, LocationBeforeLoginIsDroppedNotIngested) {
    (void)send_frames_and_collect_replies(port_, {kLocationFrame});

    // Give the connection worker a moment to have processed the frame
    // (location produces no ack to synchronize on, unlike login).
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_TRUE(transport_.broadcast_envelopes().empty());
}

namespace {

std::filesystem::path write_temp_yaml(const std::string& filename, const std::string& contents) {
    const auto path = std::filesystem::temp_directory_path() / filename;
    std::ofstream out(path, std::ios::trunc);
    out << contents;
    return path;
}

}  // namespace

TEST(Gt06TcpServerFromConfig, MissingFileDefaultsToLoopbackAndStillBinds) {
    const auto path =
        std::filesystem::temp_directory_path() / "karshipta_gt06_missing_config.yaml";
    std::filesystem::remove(path);

    FakeTransport transport;
    auto core = WardConnection::create_shared_core();
    WardManager ward_manager(core, transport);
    HeraldWardManager herald_manager(transport, ward_manager);

    const auto server = Gt06TcpServer::from_config(path.string(), herald_manager);
    ASSERT_TRUE(server);
    server->start();
    server->stop();
}

TEST(Gt06TcpServerFromConfig, LanBindWithoutEscapeHatchFallsBackToLoopbackAndStillReachable) {
    const auto path = write_temp_yaml("karshipta_gt06_lan_no_hatch.yaml",
                                       "herald:\n  gt06_host: 0.0.0.0\n  gt06_port: 28810\n");

    FakeTransport transport;
    auto core = WardConnection::create_shared_core();
    WardManager ward_manager(core, transport);
    HeraldWardManager herald_manager(transport, ward_manager);

    const auto server = Gt06TcpServer::from_config(path.string(), herald_manager);
    ASSERT_TRUE(server);
    server->start();

    // Forced back to 127.0.0.1 (allow_lan_bind not set): a loopback client
    // must still be able to reach it and get the expected login ack.
    const auto received = send_frames_and_collect_replies(28810, {kLoginFrame});
    EXPECT_EQ(received, kExpectedLoginAck);

    server->stop();
    std::filesystem::remove(path);
}
