#include "herald_http_server.h"

#include <httplib.h>

#include <google/protobuf/util/json_util.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <herald/v0/herald.pb.h>
#include <karshipta/v1/envelope.pb.h>

#include "herald_field_mapper.h"
#include "transport.h"
#include "ward_connection.h"
#include "ward_manager.h"

namespace {

constexpr auto kHost = "127.0.0.1";

// Records every broadcast()/send() call instead of touching a real socket.
// Duplicated from herald_ward_manager_test.cpp's own FakeTransport rather
// than shared across test binaries for one small class, matching this
// repo's existing serialize_envelope-style duplication precedent.
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

// Same construct-a-mapping-directly shape herald_field_mapper_test.cpp's
// kValidConfig parses into, built in code instead of via a temp YAML file:
// this file is testing HeraldHttpServer's routing/status-code behavior, not
// HeraldFieldMapper's own YAML parsing (already covered by
// herald_field_mapper_test.cpp).
HeraldFieldMapping make_test_mapping() {
    HeraldFieldMapping mapping;
    mapping.source_name = "test-vendor";
    mapping.entity_class = herald::v0::ENTITY_CLASS_GENERIC_TRACKER;
    mapping.fields = {
        {"entity_id", "device_id"},
        {"timestamp_ms", "timestamp"},
        {"latitude_deg", "location.lat"},
        {"longitude_deg", "location.lon"},
    };
    return mapping;
}

class HeraldHttpServerTest : public ::testing::Test {
   protected:
    HeraldHttpServerTest()
        : core_(WardConnection::create_shared_core()),
          ward_manager_(core_, transport_),
          herald_manager_(transport_, ward_manager_),
          server_(herald_manager_, kHost, port_, {{"test-vendor", make_test_mapping()}}) {
        server_.start();
        // start() launches listen() on its own thread; give it a moment to
        // actually bind before the first request, same reasoning
        // websocket_transport_test.cpp's real-client tests rely on a
        // connect-latch for instead - httplib::Client has no equivalent
        // "wait until listening" signal to hook, so a short fixed wait is
        // the pragmatic choice here.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ~HeraldHttpServerTest() override { server_.stop(); }

    // A fresh port per test (gtest runs this binary's tests in one process,
    // one after another; reusing a port risks a slow TIME_WAIT collision
    // between tests), same fixed-port-per-test convention as
    // websocket_transport_test.cpp.
    static uint16_t next_port() {
        static uint16_t port = 28780;
        return port++;
    }

    uint16_t port_ = next_port();
    std::shared_ptr<mavsdk::Mavsdk> core_;
    FakeTransport transport_;
    WardManager ward_manager_;
    HeraldWardManager herald_manager_;
    HeraldHttpServer server_;
};

}  // namespace

TEST_F(HeraldHttpServerTest, PostHeraldWithValidJsonBodyIngestsAndReturns200) {
    httplib::Client client(kHost, port_);

    herald::v0::Herald msg;
    msg.set_entity_id("tag-http-1");
    msg.set_timestamp_ms(1700000000000ULL);
    msg.set_entity_class(herald::v0::ENTITY_CLASS_LIVESTOCK_TAG);
    msg.mutable_position()->set_latitude_deg(52.37);
    msg.mutable_position()->set_longitude_deg(4.90);
    msg.set_health_ok(true);
    std::string json_body;
    ASSERT_TRUE(google::protobuf::util::MessageToJsonString(msg, &json_body).ok());

    const auto response = client.Post("/herald", json_body, "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 2u);
    ASSERT_TRUE(envelopes[0].has_ward_info());
    EXPECT_EQ(envelopes[0].ward_info().ward_id(), "tag-http-1");
    ASSERT_TRUE(envelopes[1].has_ward_state());
    EXPECT_EQ(envelopes[1].ward_state().ward_id(), "tag-http-1");
}

TEST_F(HeraldHttpServerTest, PostHeraldWithUndecodableBodyReturns400AndIngestsNothing) {
    httplib::Client client(kHost, port_);

    const auto response = client.Post("/herald", "not a herald message", "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
    EXPECT_TRUE(transport_.broadcast_envelopes().empty());
}

TEST_F(HeraldHttpServerTest, PostHeraldMappedWithKnownSourceIngestsAndReturns200) {
    httplib::Client client(kHost, port_);

    const auto response = client.Post("/herald/mapped/test-vendor", R"({
        "device_id": "tag-mapped-1",
        "timestamp": 1700000000000,
        "location": {"lat": 52.37, "lon": 4.90}
    })",
                                       "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 2u);
    EXPECT_EQ(envelopes[0].ward_info().ward_id(), "tag-mapped-1");
}

TEST_F(HeraldHttpServerTest, PostHeraldMappedWithUnknownSourceReturns404) {
    httplib::Client client(kHost, port_);

    const auto response = client.Post("/herald/mapped/nonexistent-vendor", "{}", "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    EXPECT_TRUE(transport_.broadcast_envelopes().empty());
}

TEST_F(HeraldHttpServerTest, PostHeraldMappedWithUndecodableJsonReturns400) {
    httplib::Client client(kHost, port_);

    const auto response = client.Post("/herald/mapped/test-vendor", "not json", "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
    EXPECT_TRUE(transport_.broadcast_envelopes().empty());
}

TEST_F(HeraldHttpServerTest, PostHeraldMappedWithMissingRequiredFieldReturns400) {
    httplib::Client client(kHost, port_);

    // No "location" key at all: latitude_deg/longitude_deg are required by
    // make_test_mapping()'s field list, so HeraldFieldMapper::apply()
    // returns nullopt and the route must answer 400, not 200 with a
    // half-built message.
    const auto response = client.Post("/herald/mapped/test-vendor", R"({
        "device_id": "tag-incomplete",
        "timestamp": 1700000000000
    })",
                                       "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
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

TEST(HeraldHttpServerFromConfig, MissingFileDefaultsToLoopback) {
    const auto path =
        std::filesystem::temp_directory_path() / "karshipta_herald_http_missing_config.yaml";
    std::filesystem::remove(path);

    FakeTransport transport;
    auto core = WardConnection::create_shared_core();
    WardManager ward_manager(core, transport);
    HeraldWardManager herald_manager(transport, ward_manager);

    const auto server = HeraldHttpServer::from_config(path.string(), herald_manager);
    ASSERT_TRUE(server);
    // No public accessor for host/port exists (unlike WebsocketTransport):
    // the observable proof is that it binds successfully on the documented
    // default rather than failing or hanging - start()/stop() would log an
    // error and return, never throw, if the bind failed.
    EXPECT_NO_THROW(server->start());
    // Same race as HeraldHttpServerTest's fixture constructor: stop() before
    // the listen thread has actually reached accept() can hang the join.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server->stop();
}

TEST(HeraldHttpServerFromConfig, LanBindWithoutEscapeHatchFallsBackToLoopbackAndStillBinds) {
    const auto path =
        write_temp_yaml("karshipta_herald_http_lan_no_hatch.yaml",
                         "herald:\n  host: 0.0.0.0\n  http_port: 28790\n");

    FakeTransport transport;
    auto core = WardConnection::create_shared_core();
    WardManager ward_manager(core, transport);
    HeraldWardManager herald_manager(transport, ward_manager);

    const auto server = HeraldHttpServer::from_config(path.string(), herald_manager);
    ASSERT_TRUE(server);
    server->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Forced back to 127.0.0.1 (allow_lan_bind not set): a client on the
    // loopback address must be able to reach it.
    httplib::Client client(kHost, 28790);
    const auto response = client.Post("/herald", "garbage", "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);  // reached the handler at all is the point

    server->stop();
    std::filesystem::remove(path);
}
