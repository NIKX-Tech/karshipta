#include "ward_manager.h"

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
#include "ward_connection.h"

namespace {

// Records every broadcast()/send() call instead of touching a real socket,
// so tests can inspect exactly what WardManager tried to deliver without
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
    // parse (there should be none; WardManager only ever broadcasts
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
// (yaml-cpp is linked PRIVATE to the ward library on purpose).
std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

// Stands in for PX4 SITL, reporting itself permanently airborne: an
// independent Mavsdk core that heartbeats as an autopilot (same pattern as
// ward_connection_test.cpp's make_fake_autopilot) and continuously
// republishes EXTENDED_SYS_STATE/InAir over its outbound connection, so
// WardManager's ground-safety guard (verify_grounded_and_disarm) sees a
// ward it must refuse to remove regardless of when the client's
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
// command_executor_test.cpp's "never-connected ward" pattern - nothing
// here calls connect()/start(), so no real network or SITL is needed.
class WardManagerTest : public ::testing::Test {
protected:
    WardManagerTest() : core_(WardConnection::create_shared_core()) {}

    WardManager make_manager(std::filesystem::path persistence_path = {}) {
        return WardManager(core_, transport_, std::move(persistence_path));
    }

    static WardConfig make_config(const std::string& ward_id, const std::string& connection_url,
                                      unsigned int system_id = 0,
                                      karshipta::v1::WardClass ward_class =
                                          karshipta::v1::WARD_CLASS_UNSPECIFIED,
                                      const std::string& name = "") {
        WardConfig cfg;
        cfg.ward_id = ward_id;
        cfg.connection_url = connection_url;
        cfg.system_id = system_id;
        cfg.ward_class = ward_class;
        cfg.name = name;
        return cfg;
    }

    std::shared_ptr<mavsdk::Mavsdk> core_;
    FakeTransport transport_;
};

// Fixture for persistence tests: gives every test its own temp file, cleaned
// up on both ends so a crashed prior run can't leak state into this one.
class WardManagerPersistenceTest : public WardManagerTest {
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

TEST_F(WardManagerTest, AddWardRegistersAndListsId) {
    auto manager = make_manager();
    EXPECT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24991")));

    const auto ids = manager.list_ward_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids.front(), "alpha-1");

    const auto statuses = manager.list_status();
    ASSERT_EQ(statuses.size(), 1u);
    EXPECT_EQ(statuses.front().ward_id, "alpha-1");
    EXPECT_FALSE(statuses.front().started);
    EXPECT_FALSE(statuses.front().connected);
}

TEST_F(WardManagerTest, AddWardRejectsEmptyId) {
    auto manager = make_manager();
    EXPECT_FALSE(manager.add_ward(make_config("", "udpin://127.0.0.1:24991")));
    EXPECT_TRUE(manager.list_ward_ids().empty());
}

TEST_F(WardManagerTest, AddWardRejectsDuplicateId) {
    auto manager = make_manager();
    EXPECT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24991")));
    EXPECT_FALSE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24992")));
    EXPECT_EQ(manager.list_ward_ids().size(), 1u);
}

TEST_F(WardManagerTest, AddWardRejectsDuplicateSystemId) {
    auto manager = make_manager();
    EXPECT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24991", 5)));
    EXPECT_FALSE(manager.add_ward(make_config("alpha-2", "udpin://127.0.0.1:24992", 5)));
    EXPECT_EQ(manager.list_ward_ids().size(), 1u);
}

TEST_F(WardManagerTest, DispatchCommandRejectsUnknownWardWithReason) {
    auto manager = make_manager();

    karshipta::v1::Command command;
    command.set_command_id("cmd-1");
    command.set_ward_id("does-not-exist");
    manager.dispatch_command(command);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_command_ack());
    const auto& ack = envelopes.front().command_ack();
    EXPECT_EQ(ack.command_id(), "cmd-1");
    EXPECT_EQ(ack.status(), karshipta::v1::COMMAND_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("does-not-exist"), std::string::npos);
}

TEST_F(WardManagerTest, HandleMissionUploadRejectsUnknownWardWithEvent) {
    auto manager = make_manager();

    karshipta::v1::Mission mission;
    mission.set_mission_id("mission-1");
    mission.set_ward_id("does-not-exist");
    manager.handle_mission_upload(mission);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_event());
    const auto& event = envelopes.front().event();
    EXPECT_EQ(event.code(), "MISSION_UPLOAD_REJECTED");
    EXPECT_EQ(event.ward_id(), "does-not-exist");
    EXPECT_NE(event.message().find("does-not-exist"), std::string::npos);
}

TEST_F(WardManagerTest, HandleMissionFileUploadRejectsUnknownWardWithEvent) {
    auto manager = make_manager();

    karshipta::v1::MissionFileUpload upload;
    upload.set_ward_id("does-not-exist");
    upload.set_format(karshipta::v1::MISSION_FILE_FORMAT_QGC_PLAN);
    upload.set_raw_content("{}");
    manager.handle_mission_file_upload(upload);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_event());
    EXPECT_EQ(envelopes.front().event().code(), "MISSION_UPLOAD_REJECTED");
}

TEST_F(WardManagerTest, HandleMissionDownloadRequestRejectsUnknownWardWithEvent) {
    auto manager = make_manager();

    karshipta::v1::MissionDownloadRequest request;
    request.set_ward_id("does-not-exist");
    manager.handle_mission_download_request(request);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_event());
    EXPECT_EQ(envelopes.front().event().code(), "MISSION_DOWNLOAD_REJECTED");
}

TEST_F(WardManagerTest, RemoveWardRemovesNeverStartedWard) {
    auto manager = make_manager();
    ASSERT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24991")));
    EXPECT_TRUE(manager.remove_ward("alpha-1"));
    EXPECT_TRUE(manager.list_ward_ids().empty());
}

// Regression test for gateway issue #70 (per-ward locking replacing the
// single fleet-wide wards_mutex_). Before this change, stop_worker()'s join
// of a reconnect_worker stuck mid-discovery ran under the same lock every
// other ward's calls needed, so one ward's up-to-kAutopilotDiscoveryTimeoutS
// (~3s) worst case stalled the whole fleet.
TEST_F(WardManagerTest, StopOnOneWardDoesNotBlockIsStartedOnAnother) {
    auto manager = make_manager();
    // Nothing ever listens on either port: connect_with_retry()'s first
    // attempt blocks for the full, deterministic kAutopilotDiscoveryTimeoutS
    // (3s), rather than racing a real autopilot's actual response time.
    ASSERT_TRUE(manager.add_ward(make_config("slow-ward", "udpin://127.0.0.1:24994")));
    ASSERT_TRUE(manager.add_ward(make_config("fast-ward", "udpin://127.0.0.1:24995")));
    ASSERT_TRUE(manager.start("slow-ward"));

    // stop() on a ward that has never connected: link_state() reads
    // kNeverDiscovered (distinct from kLinkDown), which passes stop()'s
    // guard, and is_in_air() defaults false, so stop() proceeds all the way
    // to stop_worker()'s join - the exact path this test exercises. Runs on
    // its own thread since it blocks for the discovery window.
    std::thread slow_stop([&manager] { manager.stop("slow-ward"); });

    // Give the reconnect worker a moment to actually enter its first
    // connect() attempt before racing it from the assertion below.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto before = std::chrono::steady_clock::now();
    const bool fast_started = manager.is_started("fast-ward");
    const auto elapsed = std::chrono::steady_clock::now() - before;

    slow_stop.join();

    EXPECT_FALSE(fast_started);  // never start()ed; this call's speed is what's under test
    EXPECT_LT(elapsed, std::chrono::milliseconds(500))
        << "is_started() on an unrelated ward must not block behind another "
           "ward's slow stop_worker() join";
}

TEST_F(WardManagerPersistenceTest, PersistsOnAddAndRoundTripsOnReload) {
    {
        auto manager = make_manager(path_);
        ASSERT_TRUE(manager.add_ward(
            make_config("alpha-1", "udpin://127.0.0.1:24991", 5,
                        karshipta::v1::WARD_CLASS_MULTIROTOR, "Alpha One")));
        ASSERT_TRUE(manager.add_ward(
            make_config("alpha-2", "udpin://127.0.0.1:24992", 7,
                        karshipta::v1::WARD_CLASS_FIXED_WING, "Alpha Two")));
    }
    ASSERT_TRUE(std::filesystem::exists(path_));

    const auto contents = read_file(path_);
    EXPECT_NE(contents.find("alpha-1"), std::string::npos);
    EXPECT_NE(contents.find("alpha-2"), std::string::npos);
    EXPECT_NE(contents.find("Alpha One"), std::string::npos);
    EXPECT_NE(contents.find("WARD_CLASS_MULTIROTOR"), std::string::npos);

    auto reloaded = make_manager(path_);
    EXPECT_EQ(reloaded.load_persisted(), 2u);
    auto ids = reloaded.list_ward_ids();
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], "alpha-1");
    EXPECT_EQ(ids[1], "alpha-2");

    // Proves mavlink_system_id specifically survived the round trip (not
    // just ward_id): a third ward colliding with alpha-1's system_id
    // (5) must be rejected exactly like it would against a freshly added one.
    EXPECT_FALSE(reloaded.add_ward(make_config("alpha-3", "udpin://127.0.0.1:24993", 5)));
}

TEST_F(WardManagerPersistenceTest, RemovePersistsRemoval) {
    auto manager = make_manager(path_);
    ASSERT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24991", 5)));
    ASSERT_TRUE(manager.add_ward(make_config("alpha-2", "udpin://127.0.0.1:24992", 7)));
    ASSERT_TRUE(manager.remove_ward("alpha-1"));

    const auto contents = read_file(path_);
    EXPECT_EQ(contents.find("alpha-1"), std::string::npos);
    EXPECT_NE(contents.find("alpha-2"), std::string::npos);

    auto reloaded = make_manager(path_);
    EXPECT_EQ(reloaded.load_persisted(), 1u);
    const auto ids = reloaded.list_ward_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids.front(), "alpha-2");
}

TEST_F(WardManagerPersistenceTest, LoadPersistedOnMissingFileReturnsZero) {
    ASSERT_FALSE(std::filesystem::exists(path_));
    auto manager = make_manager(path_);
    EXPECT_EQ(manager.load_persisted(), 0u);
    EXPECT_TRUE(manager.list_ward_ids().empty());
}

TEST_F(WardManagerPersistenceTest, MalformedEntryIsSkippedNotFatal) {
    {
        std::ofstream out(path_);
        out << "wards:\n"
            << "  - ward_id: good-1\n"
            << "    connection_url: \"udpin://127.0.0.1:24991\"\n"
            << "    mavlink_system_id: 1\n"
            << "    name: \"\"\n"
            << "    ward_class: WARD_CLASS_UNSPECIFIED\n"
            << "  - ward_id: bad-1\n"
            << "    mavlink_system_id: 2\n";  // missing required connection_url
    }

    auto manager = make_manager(path_);
    EXPECT_EQ(manager.load_persisted(), 1u);
    const auto ids = manager.list_ward_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids.front(), "good-1");
}

TEST_F(WardManagerPersistenceTest, LoadPersistedDoesNotRewriteFile) {
    {
        auto manager = make_manager(path_);
        ASSERT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24991", 5)));
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

karshipta::v1::AddWard make_add_request(const std::string& request_id,
                                            const std::string& ward_id,
                                            const std::string& connection_url,
                                            uint32_t mavlink_system_id = 0) {
    karshipta::v1::AddWard request;
    request.set_request_id(request_id);
    request.set_ward_id(ward_id);
    request.set_connection_url(connection_url);
    request.set_mavlink_system_id(mavlink_system_id);
    return request;
}

karshipta::v1::RemoveWard make_remove_request(const std::string& request_id,
                                                  const std::string& ward_id) {
    karshipta::v1::RemoveWard request;
    request.set_request_id(request_id);
    request.set_ward_id(ward_id);
    return request;
}

}  // namespace

TEST_F(WardManagerPersistenceTest, HandleAddWardAcceptsPersistsStartsAndEmitsEvent) {
    auto manager = make_manager(path_);

    const auto ack =
        manager.handle_add_ward(make_add_request("req-1", "alpha-1", "udpin://127.0.0.1:25290"));

    EXPECT_EQ(ack.request_id(), "req-1");
    EXPECT_EQ(ack.ward_id(), "alpha-1");
    EXPECT_EQ(ack.status(), karshipta::v1::WARD_CONFIG_STATUS_ACCEPTED);

    const auto ids = manager.list_ward_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids.front(), "alpha-1");
    EXPECT_TRUE(manager.is_started("alpha-1"));  // reconnect-forever underway

    EXPECT_NE(read_file(path_).find("alpha-1"), std::string::npos);

    const auto event = find_event(transport_.broadcast_envelopes(), "WARD_ADDED");
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->ward_id(), "alpha-1");
    EXPECT_EQ(event->severity(), karshipta::v1::SEVERITY_INFO);
}

TEST_F(WardManagerTest, HandleAddWardRejectsDuplicateIdWithReasonAndNoEvent) {
    auto manager = make_manager();
    ASSERT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24991")));

    const auto ack = manager.handle_add_ward(
        make_add_request("req-2", "alpha-1", "udpin://127.0.0.1:24992"));

    EXPECT_EQ(ack.request_id(), "req-2");
    EXPECT_EQ(ack.status(), karshipta::v1::WARD_CONFIG_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("already registered"), std::string::npos);
    EXPECT_FALSE(find_event(transport_.broadcast_envelopes(), "WARD_ADDED").has_value());
}

TEST_F(WardManagerTest, HandleRemoveWardRejectsUnknownWardWithReason) {
    auto manager = make_manager();

    const auto ack = manager.handle_remove_ward(make_remove_request("req-3", "ghost"));

    EXPECT_EQ(ack.request_id(), "req-3");
    EXPECT_EQ(ack.ward_id(), "ghost");
    EXPECT_EQ(ack.status(), karshipta::v1::WARD_CONFIG_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("unknown"), std::string::npos);
    EXPECT_FALSE(find_event(transport_.broadcast_envelopes(), "WARD_REMOVED").has_value());
}

TEST_F(WardManagerPersistenceTest, HandleRemoveWardAcceptsPersistsRemovalAndEmitsEvent) {
    auto manager = make_manager(path_);
    ASSERT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24991", 5)));
    ASSERT_NE(read_file(path_).find("alpha-1"), std::string::npos);

    const auto ack = manager.handle_remove_ward(make_remove_request("req-4", "alpha-1"));

    EXPECT_EQ(ack.request_id(), "req-4");
    EXPECT_EQ(ack.status(), karshipta::v1::WARD_CONFIG_STATUS_ACCEPTED);
    EXPECT_TRUE(manager.list_ward_ids().empty());
    EXPECT_EQ(read_file(path_).find("alpha-1"), std::string::npos);

    const auto event = find_event(transport_.broadcast_envelopes(), "WARD_REMOVED");
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->ward_id(), "alpha-1");
}

TEST_F(WardManagerTest, HandleRemoveWardRejectsWhileAirborne) {
    constexpr uint16_t port = 25291;
    constexpr uint8_t system_id = 42;
    FakeInAirAutopilot fake(system_id, port);

    auto manager = make_manager();
    const auto add_ack = manager.handle_add_ward(make_add_request(
        "req-add", "alpha-air", "udpin://127.0.0.1:" + std::to_string(port), system_id));
    ASSERT_EQ(add_ack.status(), karshipta::v1::WARD_CONFIG_STATUS_ACCEPTED);

    bool connected = false;
    for (int attempt = 0; attempt < 100 && !connected; ++attempt) {
        connected = manager.is_connected("alpha-air");
        if (!connected) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(connected) << "fake autopilot never discovered";
    // The fake republishes InAir every 50ms; poll rather than a fixed sleep,
    // since a fixed wait races a slow/loaded CI runner: if it fires before
    // TelemetryInfo has actually cached an in-air reading,
    // handle_remove_ward() below falls through to the real grounded-removal
    // path (a genuine blocking MAVSDK disarm RPC against a fake autopilot
    // that never acks it), hanging until ctest's TIMEOUT kills the test.
    bool in_air = false;
    for (int attempt = 0; attempt < 100 && !in_air; ++attempt) {
        in_air = manager.is_in_air("alpha-air");
        if (!in_air) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(in_air) << "fake autopilot's in-air state never reached TelemetryInfo";

    const auto remove_ack =
        manager.handle_remove_ward(make_remove_request("req-remove", "alpha-air"));

    EXPECT_EQ(remove_ack.status(), karshipta::v1::WARD_CONFIG_STATUS_REJECTED);
    EXPECT_NE(remove_ack.message().find("air"), std::string::npos) << remove_ack.message();
    EXPECT_EQ(manager.list_ward_ids().size(), 1u);  // still registered, not removed
}

TEST_F(WardManagerTest, RejectViewerEnvelopeRejectsCommandWithAckAndEvent) {
    auto manager = make_manager();

    karshipta::v1::Command command;
    command.set_command_id("cmd-viewer-1");
    command.set_ward_id("alpha-1");
    karshipta::v1::Envelope envelope;
    *envelope.mutable_command() = command;

    manager.reject_viewer_envelope(/*client=*/7, envelope);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 2u);
    ASSERT_TRUE(envelopes[0].has_command_ack());
    const auto& ack = envelopes[0].command_ack();
    EXPECT_EQ(ack.command_id(), "cmd-viewer-1");
    EXPECT_EQ(ack.ward_id(), "alpha-1");
    EXPECT_EQ(ack.status(), karshipta::v1::COMMAND_STATUS_REJECTED);
    EXPECT_EQ(ack.message(), "read-only session");

    ASSERT_TRUE(envelopes[1].has_event());
    const auto& event = envelopes[1].event();
    EXPECT_EQ(event.ward_id(), "alpha-1");
    EXPECT_EQ(event.severity(), karshipta::v1::SEVERITY_WARNING);
    EXPECT_EQ(event.message(), "read-only session");
}

TEST_F(WardManagerTest, RejectViewerEnvelopeRejectsAddWardWithAck) {
    auto manager = make_manager();

    karshipta::v1::Envelope envelope;
    *envelope.mutable_add_ward() = make_add_request("req-viewer", "alpha-2", "udpin://127.0.0.1:24993");

    manager.reject_viewer_envelope(/*client=*/7, envelope);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_ward_config_ack());
    const auto& ack = envelopes.front().ward_config_ack();
    EXPECT_EQ(ack.request_id(), "req-viewer");
    EXPECT_EQ(ack.ward_id(), "alpha-2");
    EXPECT_EQ(ack.status(), karshipta::v1::WARD_CONFIG_STATUS_REJECTED);
    EXPECT_EQ(ack.message(), "read-only session");
    EXPECT_TRUE(manager.list_ward_ids().empty());  // never reached add_ward_impl
}

TEST_F(WardManagerTest, RejectViewerEnvelopeRejectsRemoveWardWithAck) {
    auto manager = make_manager();
    ASSERT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24991")));

    karshipta::v1::Envelope envelope;
    *envelope.mutable_remove_ward() = make_remove_request("req-viewer", "alpha-1");

    manager.reject_viewer_envelope(/*client=*/7, envelope);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_ward_config_ack());
    EXPECT_EQ(envelopes.front().ward_config_ack().status(),
              karshipta::v1::WARD_CONFIG_STATUS_REJECTED);
    EXPECT_EQ(envelopes.front().ward_config_ack().message(), "read-only session");
    EXPECT_EQ(manager.list_ward_ids().size(), 1u);  // never reached remove_ward_impl
}

TEST_F(WardManagerTest, RejectViewerEnvelopeRejectsMissionUploadWithEvent) {
    auto manager = make_manager();

    karshipta::v1::Mission mission;
    mission.set_mission_id("mission-viewer");
    mission.set_ward_id("alpha-1");
    karshipta::v1::Envelope envelope;
    *envelope.mutable_mission_upload() = mission;

    manager.reject_viewer_envelope(/*client=*/7, envelope);

    const auto event = find_event(transport_.broadcast_envelopes(), "MISSION_UPLOAD_REJECTED");
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->ward_id(), "alpha-1");
    EXPECT_EQ(event->message(), "read-only session");
}

TEST_F(WardManagerTest, RejectViewerEnvelopeRejectsMissionDownloadRequestWithEvent) {
    auto manager = make_manager();

    karshipta::v1::MissionDownloadRequest request;
    request.set_ward_id("alpha-1");
    karshipta::v1::Envelope envelope;
    *envelope.mutable_mission_download_request() = request;

    manager.reject_viewer_envelope(/*client=*/7, envelope);

    const auto event = find_event(transport_.broadcast_envelopes(), "MISSION_DOWNLOAD_REJECTED");
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->ward_id(), "alpha-1");
    EXPECT_EQ(event->message(), "read-only session");
}
