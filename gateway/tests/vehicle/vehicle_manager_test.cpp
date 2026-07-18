#include "vehicle_manager.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <mavsdk/plugins/telemetry_server/telemetry_server.h>

#include <karshipta/v1/envelope.pb.h>

#include "transport.h"
#include "vehicle_connection.h"

namespace {

// Records every broadcast()/send() call instead of touching a real socket,
// so tests can inspect exactly what VehicleManager tried to deliver without
// standing up a WebSocket client. on_connect()/on_receive()/on_disconnect()
// are recorded but never invoked: nothing in this test file drives them.
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

    // reject_viewer_envelope() itself does not consult role() (the caller
    // already knows the sender is a viewer); nothing in this file's tests
    // needs a non-default role, so this is always kOperator.
    [[nodiscard]] ClientRole role(ClientId /*client*/) const override {
        return ClientRole::kOperator;
    }

    // Every Envelope broadcast so far, parsed. Skips frames that fail to
    // parse (there should be none; VehicleManager only ever broadcasts
    // envelopes it just serialized itself).
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
    std::vector<std::pair<ClientId, std::vector<uint8_t>>> sent_;
    std::vector<std::vector<uint8_t>> broadcast_;
    ReceiveCallback receive_callback_;
    ConnectCallback connect_callback_;
    DisconnectCallback disconnect_callback_;
};

// Slurps a small text file whole; only used to inspect persisted YAML
// content directly, without adding a yaml-cpp dependency to the test binary
// (yaml-cpp is linked PRIVATE to the vehicle library on purpose).
std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

// Stands in for PX4 SITL, reporting itself permanently airborne: an
// independent Mavsdk core that heartbeats as an autopilot (same pattern as
// vehicle_connection_test.cpp's make_fake_autopilot) and continuously
// republishes EXTENDED_SYS_STATE/InAir over its outbound connection, so
// VehicleManager's ground-safety guard (verify_grounded_and_disarm) sees a
// vehicle it must refuse to remove regardless of when the client's
// TelemetryInfo subscribes relative to any single publish call.
class FakeInAirAutopilot {
public:
    FakeInAirAutopilot(uint8_t system_id, uint16_t port)
        : core_(std::make_shared<mavsdk::Mavsdk>(
              mavsdk::Mavsdk::Configuration{system_id, /*component_id=*/1,
                                             /*always_send_heartbeats=*/true})),
          telemetry_server_(core_->server_component()) {
        const auto result = core_->add_any_connection("udpout://127.0.0.1:" + std::to_string(port));
        EXPECT_EQ(result, mavsdk::ConnectionResult::Success);
        publisher_ = std::jthread([this](const std::stop_token& stop_token) {
            while (!stop_token.stop_requested()) {
                telemetry_server_.publish_extended_sys_state(
                    mavsdk::TelemetryServer::VtolState::Undefined,
                    mavsdk::TelemetryServer::LandedState::InAir);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }

private:
    std::shared_ptr<mavsdk::Mavsdk> core_;
    mavsdk::TelemetryServer telemetry_server_;
    std::jthread publisher_;
};

// Base fixture: a shared Mavsdk core and a FakeTransport, matching
// command_executor_test.cpp's "never-connected vehicle" pattern - nothing
// here calls connect()/start(), so no real network or SITL is needed.
class VehicleManagerTest : public ::testing::Test {
protected:
    VehicleManagerTest() : core_(VehicleConnection::create_shared_core()) {}

    VehicleManager make_manager(std::filesystem::path persistence_path = {}) {
        return VehicleManager(core_, transport_, std::move(persistence_path));
    }

    static VehicleConfig make_config(const std::string& vehicle_id, const std::string& connection_url,
                                      unsigned int system_id = 0,
                                      karshipta::v1::VehicleType type =
                                          karshipta::v1::VEHICLE_TYPE_UNSPECIFIED,
                                      const std::string& name = "") {
        VehicleConfig cfg;
        cfg.vehicle_id = vehicle_id;
        cfg.connection_url = connection_url;
        cfg.system_id = system_id;
        cfg.type = type;
        cfg.name = name;
        return cfg;
    }

    std::shared_ptr<mavsdk::Mavsdk> core_;
    FakeTransport transport_;
};

// Fixture for persistence tests: gives every test its own temp file, cleaned
// up on both ends so a crashed prior run can't leak state into this one.
class VehicleManagerPersistenceTest : public VehicleManagerTest {
protected:
    void SetUp() override {
        const auto* test_info = ::testing::UnitTest::GetInstance()->current_test_info();
        path_ = std::filesystem::temp_directory_path() /
                (std::string("karshipta_vm_test_") + test_info->name() + ".yaml");
        std::filesystem::remove(path_);
    }
    void TearDown() override { std::filesystem::remove(path_); }

    std::filesystem::path path_;
};

}  // namespace

TEST_F(VehicleManagerTest, AddVehicleRegistersAndListsId) {
    auto manager = make_manager();
    EXPECT_TRUE(manager.add_vehicle(make_config("alpha-1", "udpin://127.0.0.1:24991")));

    const auto ids = manager.list_vehicle_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids.front(), "alpha-1");

    const auto statuses = manager.list_status();
    ASSERT_EQ(statuses.size(), 1u);
    EXPECT_EQ(statuses.front().vehicle_id, "alpha-1");
    EXPECT_FALSE(statuses.front().started);
    EXPECT_FALSE(statuses.front().connected);
}

TEST_F(VehicleManagerTest, AddVehicleRejectsEmptyId) {
    auto manager = make_manager();
    EXPECT_FALSE(manager.add_vehicle(make_config("", "udpin://127.0.0.1:24991")));
    EXPECT_TRUE(manager.list_vehicle_ids().empty());
}

TEST_F(VehicleManagerTest, AddVehicleRejectsDuplicateId) {
    auto manager = make_manager();
    EXPECT_TRUE(manager.add_vehicle(make_config("alpha-1", "udpin://127.0.0.1:24991")));
    EXPECT_FALSE(manager.add_vehicle(make_config("alpha-1", "udpin://127.0.0.1:24992")));
    EXPECT_EQ(manager.list_vehicle_ids().size(), 1u);
}

TEST_F(VehicleManagerTest, AddVehicleRejectsDuplicateSystemId) {
    auto manager = make_manager();
    EXPECT_TRUE(manager.add_vehicle(make_config("alpha-1", "udpin://127.0.0.1:24991", 5)));
    EXPECT_FALSE(manager.add_vehicle(make_config("alpha-2", "udpin://127.0.0.1:24992", 5)));
    EXPECT_EQ(manager.list_vehicle_ids().size(), 1u);
}

TEST_F(VehicleManagerTest, DispatchCommandRejectsUnknownVehicleWithReason) {
    auto manager = make_manager();

    karshipta::v1::Command command;
    command.set_command_id("cmd-1");
    command.set_vehicle_id("does-not-exist");
    manager.dispatch_command(command);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_command_ack());
    const auto& ack = envelopes.front().command_ack();
    EXPECT_EQ(ack.command_id(), "cmd-1");
    EXPECT_EQ(ack.status(), karshipta::v1::COMMAND_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("does-not-exist"), std::string::npos);
}

TEST_F(VehicleManagerTest, HandleMissionUploadRejectsUnknownVehicleWithEvent) {
    auto manager = make_manager();

    karshipta::v1::Mission mission;
    mission.set_mission_id("mission-1");
    mission.set_vehicle_id("does-not-exist");
    manager.handle_mission_upload(mission);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_event());
    const auto& event = envelopes.front().event();
    EXPECT_EQ(event.code(), "MISSION_UPLOAD_REJECTED");
    EXPECT_EQ(event.vehicle_id(), "does-not-exist");
    EXPECT_NE(event.message().find("does-not-exist"), std::string::npos);
}

TEST_F(VehicleManagerTest, HandleMissionFileUploadRejectsUnknownVehicleWithEvent) {
    auto manager = make_manager();

    karshipta::v1::MissionFileUpload upload;
    upload.set_vehicle_id("does-not-exist");
    upload.set_format(karshipta::v1::MISSION_FILE_FORMAT_QGC_PLAN);
    upload.set_raw_content("{}");
    manager.handle_mission_file_upload(upload);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_event());
    EXPECT_EQ(envelopes.front().event().code(), "MISSION_UPLOAD_REJECTED");
}

TEST_F(VehicleManagerTest, HandleMissionDownloadRequestRejectsUnknownVehicleWithEvent) {
    auto manager = make_manager();

    karshipta::v1::MissionDownloadRequest request;
    request.set_vehicle_id("does-not-exist");
    manager.handle_mission_download_request(request);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_event());
    EXPECT_EQ(envelopes.front().event().code(), "MISSION_DOWNLOAD_REJECTED");
}

TEST_F(VehicleManagerTest, RemoveVehicleRemovesNeverStartedVehicle) {
    auto manager = make_manager();
    ASSERT_TRUE(manager.add_vehicle(make_config("alpha-1", "udpin://127.0.0.1:24991")));
    EXPECT_TRUE(manager.remove_vehicle("alpha-1"));
    EXPECT_TRUE(manager.list_vehicle_ids().empty());
}

TEST_F(VehicleManagerPersistenceTest, PersistsOnAddAndRoundTripsOnReload) {
    {
        auto manager = make_manager(path_);
        ASSERT_TRUE(manager.add_vehicle(
            make_config("alpha-1", "udpin://127.0.0.1:24991", 5,
                        karshipta::v1::VEHICLE_TYPE_MULTIROTOR, "Alpha One")));
        ASSERT_TRUE(manager.add_vehicle(
            make_config("alpha-2", "udpin://127.0.0.1:24992", 7,
                        karshipta::v1::VEHICLE_TYPE_FIXED_WING, "Alpha Two")));
    }
    ASSERT_TRUE(std::filesystem::exists(path_));

    const auto contents = read_file(path_);
    EXPECT_NE(contents.find("alpha-1"), std::string::npos);
    EXPECT_NE(contents.find("alpha-2"), std::string::npos);
    EXPECT_NE(contents.find("Alpha One"), std::string::npos);
    EXPECT_NE(contents.find("VEHICLE_TYPE_MULTIROTOR"), std::string::npos);

    auto reloaded = make_manager(path_);
    EXPECT_EQ(reloaded.load_persisted(), 2u);
    auto ids = reloaded.list_vehicle_ids();
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], "alpha-1");
    EXPECT_EQ(ids[1], "alpha-2");

    // Proves mavlink_system_id specifically survived the round trip (not
    // just vehicle_id): a third vehicle colliding with alpha-1's system_id
    // (5) must be rejected exactly like it would against a freshly added one.
    EXPECT_FALSE(reloaded.add_vehicle(make_config("alpha-3", "udpin://127.0.0.1:24993", 5)));
}

TEST_F(VehicleManagerPersistenceTest, RemovePersistsRemoval) {
    auto manager = make_manager(path_);
    ASSERT_TRUE(manager.add_vehicle(make_config("alpha-1", "udpin://127.0.0.1:24991", 5)));
    ASSERT_TRUE(manager.add_vehicle(make_config("alpha-2", "udpin://127.0.0.1:24992", 7)));
    ASSERT_TRUE(manager.remove_vehicle("alpha-1"));

    const auto contents = read_file(path_);
    EXPECT_EQ(contents.find("alpha-1"), std::string::npos);
    EXPECT_NE(contents.find("alpha-2"), std::string::npos);

    auto reloaded = make_manager(path_);
    EXPECT_EQ(reloaded.load_persisted(), 1u);
    const auto ids = reloaded.list_vehicle_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids.front(), "alpha-2");
}

TEST_F(VehicleManagerPersistenceTest, LoadPersistedOnMissingFileReturnsZero) {
    ASSERT_FALSE(std::filesystem::exists(path_));
    auto manager = make_manager(path_);
    EXPECT_EQ(manager.load_persisted(), 0u);
    EXPECT_TRUE(manager.list_vehicle_ids().empty());
}

TEST_F(VehicleManagerPersistenceTest, MalformedEntryIsSkippedNotFatal) {
    {
        std::ofstream out(path_);
        out << "vehicles:\n"
            << "  - vehicle_id: good-1\n"
            << "    connection_url: \"udpin://127.0.0.1:24991\"\n"
            << "    mavlink_system_id: 1\n"
            << "    name: \"\"\n"
            << "    type: VEHICLE_TYPE_UNSPECIFIED\n"
            << "  - vehicle_id: bad-1\n"
            << "    mavlink_system_id: 2\n";  // missing required connection_url
    }

    auto manager = make_manager(path_);
    EXPECT_EQ(manager.load_persisted(), 1u);
    const auto ids = manager.list_vehicle_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids.front(), "good-1");
}

TEST_F(VehicleManagerPersistenceTest, LoadPersistedDoesNotRewriteFile) {
    {
        auto manager = make_manager(path_);
        ASSERT_TRUE(manager.add_vehicle(make_config("alpha-1", "udpin://127.0.0.1:24991", 5)));
    }
    const auto before = read_file(path_);

    auto reloaded = make_manager(path_);
    EXPECT_EQ(reloaded.load_persisted(), 1u);
    const auto after = read_file(path_);

    EXPECT_EQ(before, after);
}

namespace {

// First Event among broadcast_envelopes() with this code, if any.
std::optional<karshipta::v1::Event> find_event(
    const std::vector<karshipta::v1::Envelope>& envelopes, const std::string& code) {
    for (const auto& envelope : envelopes) {
        if (envelope.has_event() && envelope.event().code() == code) {
            return envelope.event();
        }
    }
    return std::nullopt;
}

karshipta::v1::AddVehicle make_add_request(const std::string& request_id,
                                            const std::string& vehicle_id,
                                            const std::string& connection_url,
                                            uint32_t mavlink_system_id = 0) {
    karshipta::v1::AddVehicle request;
    request.set_request_id(request_id);
    request.set_vehicle_id(vehicle_id);
    request.set_connection_url(connection_url);
    request.set_mavlink_system_id(mavlink_system_id);
    return request;
}

karshipta::v1::RemoveVehicle make_remove_request(const std::string& request_id,
                                                  const std::string& vehicle_id) {
    karshipta::v1::RemoveVehicle request;
    request.set_request_id(request_id);
    request.set_vehicle_id(vehicle_id);
    return request;
}

}  // namespace

TEST_F(VehicleManagerPersistenceTest, HandleAddVehicleAcceptsPersistsStartsAndEmitsEvent) {
    auto manager = make_manager(path_);

    const auto ack =
        manager.handle_add_vehicle(make_add_request("req-1", "alpha-1", "udpin://127.0.0.1:25290"));

    EXPECT_EQ(ack.request_id(), "req-1");
    EXPECT_EQ(ack.vehicle_id(), "alpha-1");
    EXPECT_EQ(ack.status(), karshipta::v1::VEHICLE_CONFIG_STATUS_ACCEPTED);

    const auto ids = manager.list_vehicle_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids.front(), "alpha-1");
    EXPECT_TRUE(manager.is_started("alpha-1"));  // reconnect-forever underway

    EXPECT_NE(read_file(path_).find("alpha-1"), std::string::npos);

    const auto event = find_event(transport_.broadcast_envelopes(), "VEHICLE_ADDED");
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->vehicle_id(), "alpha-1");
    EXPECT_EQ(event->severity(), karshipta::v1::SEVERITY_INFO);
}

TEST_F(VehicleManagerTest, HandleAddVehicleRejectsDuplicateIdWithReasonAndNoEvent) {
    auto manager = make_manager();
    ASSERT_TRUE(manager.add_vehicle(make_config("alpha-1", "udpin://127.0.0.1:24991")));

    const auto ack = manager.handle_add_vehicle(
        make_add_request("req-2", "alpha-1", "udpin://127.0.0.1:24992"));

    EXPECT_EQ(ack.request_id(), "req-2");
    EXPECT_EQ(ack.status(), karshipta::v1::VEHICLE_CONFIG_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("already registered"), std::string::npos);
    EXPECT_FALSE(find_event(transport_.broadcast_envelopes(), "VEHICLE_ADDED").has_value());
}

TEST_F(VehicleManagerTest, HandleRemoveVehicleRejectsUnknownVehicleWithReason) {
    auto manager = make_manager();

    const auto ack = manager.handle_remove_vehicle(make_remove_request("req-3", "ghost"));

    EXPECT_EQ(ack.request_id(), "req-3");
    EXPECT_EQ(ack.vehicle_id(), "ghost");
    EXPECT_EQ(ack.status(), karshipta::v1::VEHICLE_CONFIG_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("unknown"), std::string::npos);
    EXPECT_FALSE(find_event(transport_.broadcast_envelopes(), "VEHICLE_REMOVED").has_value());
}

TEST_F(VehicleManagerPersistenceTest, HandleRemoveVehicleAcceptsPersistsRemovalAndEmitsEvent) {
    auto manager = make_manager(path_);
    ASSERT_TRUE(manager.add_vehicle(make_config("alpha-1", "udpin://127.0.0.1:24991", 5)));
    ASSERT_NE(read_file(path_).find("alpha-1"), std::string::npos);

    const auto ack = manager.handle_remove_vehicle(make_remove_request("req-4", "alpha-1"));

    EXPECT_EQ(ack.request_id(), "req-4");
    EXPECT_EQ(ack.status(), karshipta::v1::VEHICLE_CONFIG_STATUS_ACCEPTED);
    EXPECT_TRUE(manager.list_vehicle_ids().empty());
    EXPECT_EQ(read_file(path_).find("alpha-1"), std::string::npos);

    const auto event = find_event(transport_.broadcast_envelopes(), "VEHICLE_REMOVED");
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->vehicle_id(), "alpha-1");
}

TEST_F(VehicleManagerTest, HandleRemoveVehicleRejectsWhileAirborne) {
    constexpr uint16_t port = 25291;
    constexpr uint8_t system_id = 42;
    FakeInAirAutopilot fake(system_id, port);

    auto manager = make_manager();
    const auto add_ack = manager.handle_add_vehicle(make_add_request(
        "req-add", "alpha-air", "udpin://127.0.0.1:" + std::to_string(port), system_id));
    ASSERT_EQ(add_ack.status(), karshipta::v1::VEHICLE_CONFIG_STATUS_ACCEPTED);

    bool connected = false;
    for (int attempt = 0; attempt < 100 && !connected; ++attempt) {
        connected = manager.is_connected("alpha-air");
        if (!connected) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(connected) << "fake autopilot never discovered";
    // The fake republishes InAir every 50ms; give the client's TelemetryInfo a
    // moment to have received at least one of those since connecting.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const auto remove_ack =
        manager.handle_remove_vehicle(make_remove_request("req-remove", "alpha-air"));

    EXPECT_EQ(remove_ack.status(), karshipta::v1::VEHICLE_CONFIG_STATUS_REJECTED);
    EXPECT_NE(remove_ack.message().find("air"), std::string::npos) << remove_ack.message();
    EXPECT_EQ(manager.list_vehicle_ids().size(), 1u);  // still registered, not removed
}

TEST_F(VehicleManagerTest, RejectViewerEnvelopeRejectsCommandWithAckAndEvent) {
    auto manager = make_manager();

    karshipta::v1::Command command;
    command.set_command_id("cmd-viewer-1");
    command.set_vehicle_id("alpha-1");
    karshipta::v1::Envelope envelope;
    *envelope.mutable_command() = command;

    manager.reject_viewer_envelope(/*client=*/7, envelope);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 2u);
    ASSERT_TRUE(envelopes[0].has_command_ack());
    const auto& ack = envelopes[0].command_ack();
    EXPECT_EQ(ack.command_id(), "cmd-viewer-1");
    EXPECT_EQ(ack.vehicle_id(), "alpha-1");
    EXPECT_EQ(ack.status(), karshipta::v1::COMMAND_STATUS_REJECTED);
    EXPECT_EQ(ack.message(), "read-only session");

    ASSERT_TRUE(envelopes[1].has_event());
    const auto& event = envelopes[1].event();
    EXPECT_EQ(event.vehicle_id(), "alpha-1");
    EXPECT_EQ(event.severity(), karshipta::v1::SEVERITY_WARNING);
    EXPECT_EQ(event.message(), "read-only session");
}

TEST_F(VehicleManagerTest, RejectViewerEnvelopeRejectsAddVehicleWithAck) {
    auto manager = make_manager();

    karshipta::v1::Envelope envelope;
    *envelope.mutable_add_vehicle() = make_add_request("req-viewer", "alpha-2", "udpin://127.0.0.1:24993");

    manager.reject_viewer_envelope(/*client=*/7, envelope);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_vehicle_config_ack());
    const auto& ack = envelopes.front().vehicle_config_ack();
    EXPECT_EQ(ack.request_id(), "req-viewer");
    EXPECT_EQ(ack.vehicle_id(), "alpha-2");
    EXPECT_EQ(ack.status(), karshipta::v1::VEHICLE_CONFIG_STATUS_REJECTED);
    EXPECT_EQ(ack.message(), "read-only session");
    EXPECT_TRUE(manager.list_vehicle_ids().empty());  // never reached add_vehicle_impl
}

TEST_F(VehicleManagerTest, RejectViewerEnvelopeRejectsRemoveVehicleWithAck) {
    auto manager = make_manager();
    ASSERT_TRUE(manager.add_vehicle(make_config("alpha-1", "udpin://127.0.0.1:24991")));

    karshipta::v1::Envelope envelope;
    *envelope.mutable_remove_vehicle() = make_remove_request("req-viewer", "alpha-1");

    manager.reject_viewer_envelope(/*client=*/7, envelope);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_vehicle_config_ack());
    EXPECT_EQ(envelopes.front().vehicle_config_ack().status(),
              karshipta::v1::VEHICLE_CONFIG_STATUS_REJECTED);
    EXPECT_EQ(envelopes.front().vehicle_config_ack().message(), "read-only session");
    EXPECT_EQ(manager.list_vehicle_ids().size(), 1u);  // never reached remove_vehicle_impl
}

TEST_F(VehicleManagerTest, RejectViewerEnvelopeRejectsMissionUploadWithEvent) {
    auto manager = make_manager();

    karshipta::v1::Mission mission;
    mission.set_mission_id("mission-viewer");
    mission.set_vehicle_id("alpha-1");
    karshipta::v1::Envelope envelope;
    *envelope.mutable_mission_upload() = mission;

    manager.reject_viewer_envelope(/*client=*/7, envelope);

    const auto event = find_event(transport_.broadcast_envelopes(), "MISSION_UPLOAD_REJECTED");
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->vehicle_id(), "alpha-1");
    EXPECT_EQ(event->message(), "read-only session");
}

TEST_F(VehicleManagerTest, RejectViewerEnvelopeRejectsMissionDownloadRequestWithEvent) {
    auto manager = make_manager();

    karshipta::v1::MissionDownloadRequest request;
    request.set_vehicle_id("alpha-1");
    karshipta::v1::Envelope envelope;
    *envelope.mutable_mission_download_request() = request;

    manager.reject_viewer_envelope(/*client=*/7, envelope);

    const auto event = find_event(transport_.broadcast_envelopes(), "MISSION_DOWNLOAD_REJECTED");
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->vehicle_id(), "alpha-1");
    EXPECT_EQ(event->message(), "read-only session");
}
