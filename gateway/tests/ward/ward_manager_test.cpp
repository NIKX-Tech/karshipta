#include "ward_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <mavsdk/plugins/telemetry_server/telemetry_server.hpp>

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
    // Not exercised by these tests; satisfies the interface.
    void disconnect(ClientId /*client*/) override {}

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

    // Every Envelope send() to this specific client so far, parsed, in
    // order - the send()-to-requester counterpart of broadcast_envelopes()
    // above, for GetLogEntries/DownloadLogFile (karshipta#249), which are
    // answered via send(), not broadcast() (see
    // WardManager::handle_get_log_entries()'s own doc comment for why).
    [[nodiscard]] std::vector<karshipta::v1::Envelope> sent_envelopes(ClientId client) const {
        std::lock_guard lock(mutex_);
        std::vector<karshipta::v1::Envelope> envelopes;
        for (const auto& [sent_client, bytes] : sent_) {
            if (sent_client != client) continue;
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
                // 10ms, not 50ms: a shorter gap between publishes means less
                // time for a dropped/reordered UDP packet or CI-runner
                // scheduling jitter to leave MAVSDK's cached in_air() reading
                // stale, directly shrinking the window
                // HandleRemoveWardRejectsWhileAirborne's own poll below is
                // racing against (see its comment - this test has flaked in
                // CI three times despite two rounds of widening that poll's
                // own margin, gcc specifically).
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

private:
    std::shared_ptr<mavsdk::Mavsdk> core_;
    mavsdk::TelemetryServer telemetry_server_;
    std::jthread publisher_;
};

// A fake autopilot bound to a chosen port/system id, used by the MV-01/02/03
// multi-vehicle tests below (karshipta#270) - several of these exist at
// once in each of those tests, which plain FakeInAirAutopilot alone does
// not need to track (ward_id/port/system_id all come from the test's own
// loop variables there).
struct MultiVehicleFake {
    uint16_t port;
    uint8_t system_id;
    std::unique_ptr<FakeInAirAutopilot> autopilot;
};

// Dispatches every fake's destruction onto its own detached thread, the
// same reasoning as WardConnection::disconnect()'s own karshipta#274 fix:
// each fake is a raw mavsdk::Mavsdk core (constructed directly, not through
// WardConnection), so it has none of that fix's protection against
// karshipta#273's vendored remove_connection() hazard on its own. This is
// not a hypothetical concern: the first version of
// ThreeWardsConnectConcurrentlyEachToOwnSystemId below destroyed its three
// fakes inline at scope-end (safe for one fake alone, per
// ManagerDestructsCleanlyRightAfterWardFirstConnects above - but tearing
// down three real MAVSDK cores synchronously, one after another, on a
// loaded CI runner is a much larger window) and timed out in CI's clang
// job on the very first PR run - never reproduced locally even under
// added CPU load, consistent with a real GitHub-hosted runner (2 vCPUs)
// being meaningfully more constrained than any machine this was developed
// on, the same conclusion karshipta#237 reached for an unrelated test.
// Found via 100-run stress testing against MAVSDK v4.0.6 under added CPU
// load: a detach-and-forget thread can still be mid-teardown of a real
// Mavsdk core when the whole test BINARY's own main() returns and the
// process starts exiting, racing the C++ runtime's own static-destructor
// sequence - libc++abi aborted with "mutex lock failed: Invalid argument"
// on roughly a third of runs, always during one of the three MV tests
// below. Never seen under v3.17.1's slower, dedicated-thread-join-based
// teardown; v4's faster Asio-based path just closes the race window this
// detach pattern always had. g_pending_fake_teardowns +
// FakeTeardownWaitEnvironment below give every still-running teardown
// thread a bounded chance to finish once, after all tests have run,
// rather than per-test - preserves the reason this is detached at all
// (avoiding a timeout on a loaded CI runner tearing down several cores
// synchronously within one test), without racing process exit.
std::atomic<int> g_pending_fake_teardowns{0};

void detach_fake_teardown(std::vector<MultiVehicleFake> fakes) {
    for (auto& fake : fakes) {
        g_pending_fake_teardowns.fetch_add(1, std::memory_order_relaxed);
        std::thread([autopilot = std::move(fake.autopilot)]() mutable {
            autopilot.reset();
            g_pending_fake_teardowns.fetch_sub(1, std::memory_order_relaxed);
        }).detach();
    }
}

// Registered via static init (AddGlobalTestEnvironment must run before
// RUN_ALL_TESTS(), which this test binary's gtest_main-provided main()
// calls); TearDown() runs once, after every test has finished, right
// before main() returns.
class FakeTeardownWaitEnvironment : public ::testing::Environment {
public:
    void TearDown() override {
        constexpr auto kWaitBound = std::chrono::seconds(5);
        const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
        while (g_pending_fake_teardowns.load(std::memory_order_relaxed) > 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        // Flagged by an independent review of this fix: hitting the bound
        // used to return silently, meaning the exact race this mechanism
        // exists to prevent (a detached teardown thread still running when
        // the process starts exiting) could recur completely undetected.
        // ADD_FAILURE() here fails the overall test binary (gtest runs
        // global-environment TearDown() failures into RUN_ALL_TESTS()'s
        // exit code), so hitting the bound is loud, not silent.
        if (const auto remaining = g_pending_fake_teardowns.load(std::memory_order_relaxed);
            remaining > 0) {
            ADD_FAILURE() << remaining
                          << " fake teardown(s) still pending after the " << kWaitBound.count()
                          << "s bound - a detached teardown thread may still be running when "
                             "this process exits.";
        }
    }
};

[[maybe_unused]] ::testing::Environment* const g_fake_teardown_wait_env =
    ::testing::AddGlobalTestEnvironment(new FakeTeardownWaitEnvironment());

// RAII guard around a test's fakes: dispatches detach_fake_teardown() in
// its own destructor, so it runs on every exit path, not just one reached
// by falling off the end of a test body. That distinction is the whole
// point of this type, found by a second reviewer's independent pass
// before this PR shipped: gtest's ASSERT_* macros expand to a bare
// `return;` on failure, which still unwinds through ordinary local
// destructors - an explicit detach_fake_teardown() call placed only at
// the bottom of a test body is skipped entirely on that path, leaving
// `fakes` to destruct inline right when a real regression is being
// reported. That's exactly the hang this whole mechanism exists to
// prevent, on exactly the path that matters most. Declared right after
// `fakes` is fully populated in each test; holds a reference, not a copy,
// so the mid-test explicit drop in MV-02/MV-03 (which moves one fake's
// autopilot out directly) is unaffected.
class DetachFakesOnExit {
public:
    explicit DetachFakesOnExit(std::vector<MultiVehicleFake>& fakes) : fakes_(fakes) {}
    ~DetachFakesOnExit() { detach_fake_teardown(std::move(fakes_)); }
    DetachFakesOnExit(const DetachFakesOnExit&) = delete;
    DetachFakesOnExit& operator=(const DetachFakesOnExit&) = delete;

private:
    std::vector<MultiVehicleFake>& fakes_;
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

// push_geofence() is called by FleetManager on every zone mutation
// (karshipta#247) and again by run_reconnect_loop on every reconnect; this
// test only exercises the no-connection path (same "never-started ward"
// pattern as AddWardRegistersAndListsId above) - a never-connected ward's
// WardActions::ensure_geofence() always fails, so the real assertion here is
// that pushing to it is a logged no-op, never a crash, and that an empty
// zone list (no zones created yet) is just as safe to push as a populated
// one.
TEST_F(WardManagerTest, PushGeofenceDoesNotCrashWithNoConnectedWards) {
    auto manager = make_manager();
    EXPECT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24993")));

    EXPECT_NO_THROW(manager.push_geofence({}));

    karshipta::v1::Zone zone;
    zone.set_zone_id("zone-1");
    zone.set_name("test keep-in");
    zone.set_type(karshipta::v1::ZONE_TYPE_KEEP_IN);
    for (const auto& [lat, lon] : {std::pair{47.0, 8.0}, std::pair{47.0, 8.1}, std::pair{47.1, 8.1}}) {
        auto* vertex = zone.add_vertices();
        vertex->set_latitude_deg(lat);
        vertex->set_longitude_deg(lon);
    }
    EXPECT_NO_THROW(manager.push_geofence({zone}));
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

// handle_get_params()/handle_set_param() return their response value
// directly rather than broadcasting it (karshipta#211, mirrors the Zone/
// Fleet handler shape, not dispatch_command()'s broadcast-an-ack shape),
// so these tests assert on the returned ParamList/ParamAck directly - no
// transport_.broadcast_envelopes() involved.
TEST_F(WardManagerTest, HandleGetParamsRejectsUnknownWardWithReason) {
    auto manager = make_manager();

    karshipta::v1::GetParams request;
    request.set_request_id("req-1");
    request.set_ward_id("does-not-exist");
    const auto response = manager.handle_get_params(request);

    EXPECT_EQ(response.request_id(), "req-1");
    EXPECT_EQ(response.status(), karshipta::v1::PARAM_LIST_STATUS_REJECTED);
    EXPECT_NE(response.message().find("does-not-exist"), std::string::npos);
    EXPECT_EQ(response.params_size(), 0);
}

// A registered-but-never-connected ward's get_all_params() would otherwise
// return an empty AllParams (WardActions::get_all_params()'s own doc
// comment) - indistinguishable from "this vehicle genuinely has zero
// parameters" unless handle_get_params() checks connection state itself
// first, which this test exercises directly (same "never-started ward"
// pattern as AddWardRegistersAndListsId).
TEST_F(WardManagerTest, HandleGetParamsRejectsUnconnectedWardWithReason) {
    auto manager = make_manager();
    ASSERT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24994")));

    karshipta::v1::GetParams request;
    request.set_request_id("req-1");
    request.set_ward_id("alpha-1");
    const auto response = manager.handle_get_params(request);

    EXPECT_EQ(response.status(), karshipta::v1::PARAM_LIST_STATUS_REJECTED);
    EXPECT_NE(response.message().find("not connected"), std::string::npos);
}

TEST_F(WardManagerTest, HandleSetParamRejectsUnknownWardWithReason) {
    auto manager = make_manager();

    karshipta::v1::SetParam request;
    request.set_request_id("req-1");
    request.set_ward_id("does-not-exist");
    request.mutable_param()->set_name("MPC_LAND_SPEED");
    request.mutable_param()->set_type(karshipta::v1::PARAM_TYPE_FLOAT);
    request.mutable_param()->set_float_value(0.5f);
    const auto ack = manager.handle_set_param(request);

    EXPECT_EQ(ack.request_id(), "req-1");
    EXPECT_EQ(ack.status(), karshipta::v1::PARAM_ACK_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("does-not-exist"), std::string::npos);
}

// Request-shape check ("does this even carry a param"), deliberately
// ordered before the connection check in handle_set_param() itself - see
// that method's own comment - so this is testable without a live
// connection, same as HandleGetParamsRejectsUnknownWardWithReason above.
TEST_F(WardManagerTest, HandleSetParamRejectsRequestWithNoParam) {
    auto manager = make_manager();
    ASSERT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24995")));

    karshipta::v1::SetParam request;
    request.set_request_id("req-1");
    request.set_ward_id("alpha-1");
    const auto ack = manager.handle_set_param(request);

    EXPECT_EQ(ack.status(), karshipta::v1::PARAM_ACK_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("no param"), std::string::npos);
}

TEST_F(WardManagerTest, HandleSetParamRejectsUnconnectedWardWithReason) {
    auto manager = make_manager();
    ASSERT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24996")));

    karshipta::v1::SetParam request;
    request.set_request_id("req-1");
    request.set_ward_id("alpha-1");
    request.mutable_param()->set_name("MPC_LAND_SPEED");
    request.mutable_param()->set_type(karshipta::v1::PARAM_TYPE_FLOAT);
    request.mutable_param()->set_float_value(0.5f);
    const auto ack = manager.handle_set_param(request);

    EXPECT_EQ(ack.status(), karshipta::v1::PARAM_ACK_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("not connected"), std::string::npos);
}

// handle_get_log_entries()/handle_download_log_file() are void, not
// value-returning like handle_get_params()/handle_set_param() above: a
// result only ever comes back later, asynchronously, from the ward's own
// LogDownloadExecutor worker thread (karshipta#249). Only the two
// synchronous-reject cases below (unknown ward, unconnected ward) are
// testable without a live connection - same limitation as the exercised
// GetParams/SetParam rejection tests above - so these inspect what
// FakeTransport's send() captured for the requesting client instead of a
// return value.
TEST_F(WardManagerTest, HandleGetLogEntriesRejectsUnknownWardWithReason) {
    auto manager = make_manager();

    karshipta::v1::GetLogEntries request;
    request.set_request_id("req-1");
    request.set_ward_id("does-not-exist");
    manager.handle_get_log_entries(/*client=*/1, request);

    const auto sent = transport_.sent_envelopes(1);
    ASSERT_EQ(sent.size(), 1U);
    ASSERT_TRUE(sent.front().has_log_entry_list());
    const auto& response = sent.front().log_entry_list();
    EXPECT_EQ(response.request_id(), "req-1");
    EXPECT_EQ(response.status(), karshipta::v1::LOG_ENTRY_LIST_STATUS_REJECTED);
    EXPECT_NE(response.message().find("does-not-exist"), std::string::npos);
    EXPECT_EQ(response.entries_size(), 0);
}

TEST_F(WardManagerTest, HandleGetLogEntriesRejectsUnconnectedWardWithReason) {
    auto manager = make_manager();
    ASSERT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24997")));

    karshipta::v1::GetLogEntries request;
    request.set_request_id("req-1");
    request.set_ward_id("alpha-1");
    manager.handle_get_log_entries(/*client=*/1, request);

    const auto sent = transport_.sent_envelopes(1);
    ASSERT_EQ(sent.size(), 1U);
    const auto& response = sent.front().log_entry_list();
    EXPECT_EQ(response.status(), karshipta::v1::LOG_ENTRY_LIST_STATUS_REJECTED);
    EXPECT_NE(response.message().find("not connected"), std::string::npos);
}

TEST_F(WardManagerTest, HandleDownloadLogFileRejectsUnknownWardWithReason) {
    auto manager = make_manager();

    karshipta::v1::DownloadLogFile request;
    request.set_request_id("req-1");
    request.set_ward_id("does-not-exist");
    request.set_log_id(3);
    manager.handle_download_log_file(/*client=*/1, request);

    const auto sent = transport_.sent_envelopes(1);
    ASSERT_EQ(sent.size(), 1U);
    ASSERT_TRUE(sent.front().has_download_log_file_ack());
    const auto& ack = sent.front().download_log_file_ack();
    EXPECT_EQ(ack.request_id(), "req-1");
    EXPECT_EQ(ack.log_id(), 3U);
    EXPECT_EQ(ack.status(), karshipta::v1::LOG_DOWNLOAD_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("does-not-exist"), std::string::npos);
}

TEST_F(WardManagerTest, HandleDownloadLogFileRejectsUnconnectedWardWithReason) {
    auto manager = make_manager();
    ASSERT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24998")));

    karshipta::v1::DownloadLogFile request;
    request.set_request_id("req-1");
    request.set_ward_id("alpha-1");
    request.set_log_id(3);
    manager.handle_download_log_file(/*client=*/1, request);

    const auto sent = transport_.sent_envelopes(1);
    ASSERT_EQ(sent.size(), 1U);
    const auto& ack = sent.front().download_log_file_ack();
    EXPECT_EQ(ack.status(), karshipta::v1::LOG_DOWNLOAD_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("not connected"), std::string::npos);
}

// handle_start_calibration()/handle_cancel_calibration() are void, same
// shape as handle_get_log_entries()/handle_download_log_file() above and
// for the same reason: the real result (zero or more RUNNING
// CalibrationProgress messages, then one terminal one) only ever comes
// back later, from the ward's own CalibrationExecutor worker thread
// (karshipta#248). Only the synchronous-reject cases below (unknown
// ward, unconnected ward) are testable without a live connection.
TEST_F(WardManagerTest, HandleStartCalibrationRejectsUnknownWardWithReason) {
    auto manager = make_manager();

    karshipta::v1::StartCalibration request;
    request.set_request_id("req-1");
    request.set_ward_id("does-not-exist");
    request.set_calibration_type(karshipta::v1::CALIBRATION_TYPE_GYRO);
    manager.handle_start_calibration(/*client=*/1, request);

    const auto sent = transport_.sent_envelopes(1);
    ASSERT_EQ(sent.size(), 1U);
    ASSERT_TRUE(sent.front().has_calibration_progress());
    const auto& progress = sent.front().calibration_progress();
    EXPECT_EQ(progress.request_id(), "req-1");
    EXPECT_EQ(progress.calibration_type(), karshipta::v1::CALIBRATION_TYPE_GYRO);
    EXPECT_EQ(progress.status(), karshipta::v1::CALIBRATION_STATUS_REJECTED);
    EXPECT_NE(progress.message().find("does-not-exist"), std::string::npos);
}

TEST_F(WardManagerTest, HandleStartCalibrationRejectsUnconnectedWardWithReason) {
    auto manager = make_manager();
    ASSERT_TRUE(manager.add_ward(make_config("alpha-1", "udpin://127.0.0.1:24999")));

    karshipta::v1::StartCalibration request;
    request.set_request_id("req-1");
    request.set_ward_id("alpha-1");
    request.set_calibration_type(karshipta::v1::CALIBRATION_TYPE_LEVEL_HORIZON);
    manager.handle_start_calibration(/*client=*/1, request);

    const auto sent = transport_.sent_envelopes(1);
    ASSERT_EQ(sent.size(), 1U);
    const auto& progress = sent.front().calibration_progress();
    EXPECT_EQ(progress.status(), karshipta::v1::CALIBRATION_STATUS_REJECTED);
    EXPECT_NE(progress.message().find("not connected"), std::string::npos);
}

TEST_F(WardManagerTest, HandleCancelCalibrationOnUnknownWardSendsNothing) {
    auto manager = make_manager();

    karshipta::v1::CancelCalibration request;
    request.set_request_id("req-1");
    request.set_ward_id("does-not-exist");
    request.set_calibration_request_id("original-req");
    // No ack channel exists for Cancel at all (see CalibrationExecutor::
    // cancel()'s own doc comment) - this only needs to not crash and not
    // send anything for an unknown ward.
    manager.handle_cancel_calibration(/*client=*/1, request);

    EXPECT_TRUE(transport_.sent_envelopes(1).empty());
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
    //
    // A single true read is not enough: MAVSDK's cached in_air() value can
    // flicker back to false for a stretch (a reordered/dropped
    // EXTENDED_SYS_STATE packet over the loopback UDP link, or MAVSDK's own
    // internal dispatch lagging under a loaded CI runner), seen in practice
    // as this exact test intermittently hanging - more often under gcc's
    // build than clang's, consistent with a genuine timing-sensitive race
    // rather than a logic bug: remove_ward_impl's own is_in_air() check
    // (ward_manager.cpp) runs moments after this loop's last read, and a
    // flip to false in that window sends it down the real blocking-disarm
    // path this test means to avoid entirely. A fixed consecutive-read
    // count (tried first, still flaked - the flicker can outlast even 5
    // reads 50ms apart) is the wrong shape for "wait for it to settle";
    // require it to read true continuously for a real stretch of wall-clock
    // time instead, which stays correct regardless of how long any single
    // flicker turns out to be.
    //
    // 2s stable / 30s deadline (the first version of this fix) still
    // flaked once more in CI after landing; widened further and paired
    // with FakeInAirAutopilot's own publish interval dropping from 50ms to
    // 10ms above (less time for any single gap to matter). CMakeLists.txt's
    // gtest_discover_tests TIMEOUT also moved from 60s to 90s (suite-wide -
    // gtest_discover_tests has no clean way to set a per-test override
    // before discovery runs) so this poll's own budget isn't competing
    // with the ctest-level kill switch for the same seconds.
    constexpr auto kRequiredStableDuration = std::chrono::milliseconds(4000);
    constexpr auto kPollDeadline = std::chrono::seconds(60);
    std::optional<std::chrono::steady_clock::time_point> stable_since;
    bool settled = false;
    const auto deadline = std::chrono::steady_clock::now() + kPollDeadline;
    while (std::chrono::steady_clock::now() < deadline) {
        if (manager.is_in_air("alpha-air")) {
            const auto now = std::chrono::steady_clock::now();
            if (!stable_since) stable_since = now;
            if (now - *stable_since >= kRequiredStableDuration) {
                settled = true;
                break;
            }
        } else {
            stable_since.reset();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(settled) << "fake autopilot's in-air state never settled true in TelemetryInfo";

    const auto remove_ack =
        manager.handle_remove_ward(make_remove_request("req-remove", "alpha-air"));

    EXPECT_EQ(remove_ack.status(), karshipta::v1::WARD_CONFIG_STATUS_REJECTED);
    EXPECT_NE(remove_ack.message().find("air"), std::string::npos) << remove_ack.message();
    EXPECT_EQ(manager.list_ward_ids().size(), 1u);  // still registered, not removed
}

// karshipta#270 (regression): WardManager's own destructor used to run
// into a genuine shutdown-ordering bug, not a test artifact - geofence_mutex_
// was declared (in ward_manager.h) AFTER managed_wards_, so members destroy
// in reverse declaration order meant geofence_mutex_ was destroyed BEFORE
// managed_wards_ - but managed_wards_ is what actually stops and joins every
// ward's reconnect_worker, and run_reconnect_loop locks geofence_mutex_ on
// every (re)connect. A reconnect_worker still inside that lock_guard at
// shutdown could lock an already-destroyed mutex. Reproduced with 100%
// reliability by ending the test scope immediately after is_connected()
// first turns true (the exact moment run_reconnect_loop is about to reach
// that lock) - a test that instead stays alive for several seconds past
// connect (HandleRemoveWardRejectsWhileAirborne above) never hit it, since
// the reconnect_worker had long since moved past that section by the time
// its destructor ran. Fixed by reordering geofence_mutex_/cached_geofence_
// to before wards_mutex_/managed_wards_ in ward_manager.h, the same
// reasoning publish_worker_ already correctly applies there. This test
// failed with "mutex lock failed: Invalid argument" on every run before
// that fix and is green and fast (not multi-second) after it.
TEST_F(WardManagerTest, ManagerDestructsCleanlyRightAfterWardFirstConnects) {
    constexpr uint16_t port = 25300;
    constexpr uint8_t system_id = 10;
    FakeInAirAutopilot fake(system_id, port);

    auto manager = make_manager();
    const auto ack = manager.handle_add_ward(make_add_request(
        "req-mv01-a", "mv01-a", "udpin://127.0.0.1:" + std::to_string(port), system_id));
    ASSERT_EQ(ack.status(), karshipta::v1::WARD_CONFIG_STATUS_ACCEPTED);

    bool connected = false;
    for (int attempt = 0; attempt < 100 && !connected; ++attempt) {
        connected = manager.is_connected("mv01-a");
        if (!connected) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(connected) << "fake autopilot never discovered";
    // manager and fake go out of scope right here, with run_reconnect_loop
    // most likely still inside (or about to enter) the geofence push it
    // does on every connect - exactly the window the bug lived in.
}

// karshipta#270, MV-01 (Yigitcan Ozturk's proposed regression matrix on
// #270: three wards connect concurrently, each binding to its own expected
// system id). The single-ward fallout of originally attempting this test is
// already covered above (ManagerDestructsCleanlyRightAfterWardFirstConnects,
// named after this one since that is what it was trying to be before the
// destructor bug it found got its own focused regression test); this is the
// real three-ward version, now that #271 (the destructor-ordering bug) is
// fixed. This test never tears any ward down mid-test, only at scope-end,
// where disconnect()'s own detached-thread dispatch (karshipta#274) means a
// stuck remove_connection() cannot block this test's exit either - but that
// fix lives in WardConnection, not in the three fakes, which are raw
// mavsdk::Mavsdk cores with no such protection; see detach_fake_teardown's
// own comment for why their teardown is dispatched the same way, and why
// this is not a hypothetical precaution. Verified locally with the
// detached fake teardown in place: 8 consecutive runs under added CPU
// load, all green, ~4.4s each (see detach_fake_teardown's own comment for
// why "under added load" matters here and wasn't enough on its own before
// that fix).
TEST_F(WardManagerTest, ThreeWardsConnectConcurrentlyEachToOwnSystemId) {
    std::vector<MultiVehicleFake> fakes;
    for (const auto& [port, system_id] :
         std::vector<std::pair<uint16_t, uint8_t>>{{25310, 21}, {25311, 22}, {25312, 23}}) {
        fakes.push_back(
            MultiVehicleFake{port, system_id, std::make_unique<FakeInAirAutopilot>(system_id, port)});
    }
    DetachFakesOnExit detach_guard(fakes);

    auto manager = make_manager();
    for (size_t i = 0; i < fakes.size(); ++i) {
        const auto ward_id = "mv01-" + std::to_string(i);
        const auto ack = manager.handle_add_ward(make_add_request(
            "req-" + ward_id, ward_id, "udpin://127.0.0.1:" + std::to_string(fakes[i].port),
            fakes[i].system_id));
        ASSERT_EQ(ack.status(), karshipta::v1::WARD_CONFIG_STATUS_ACCEPTED) << ward_id;
    }

    // Poll all three together rather than one at a time: sequential polling
    // would hide any cross-ward interference in WardManager's shared
    // discovery/dispatch path, which is the entire point of this test over
    // the single-ward version above.
    std::array<bool, 3> connected{false, false, false};
    for (int attempt = 0; attempt < 100 && !(connected[0] && connected[1] && connected[2]);
         ++attempt) {
        for (size_t i = 0; i < fakes.size(); ++i) {
            connected[i] = manager.is_connected("mv01-" + std::to_string(i));
        }
        if (!(connected[0] && connected[1] && connected[2])) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    for (size_t i = 0; i < fakes.size(); ++i) {
        EXPECT_TRUE(connected[i]) << "mv01-" << i << " never connected";
    }

    // Each ward bound to its OWN system id, not a neighbor's: list_status()
    // exposes connected per ward_id, and each ward_id's connection_url
    // targets a distinct fake, so a cross-wiring bug would show as one of
    // the three staying permanently unconnected (its real fake's packets
    // arriving on the wrong ward) rather than a wrong-but-connected state -
    // already ruled out by the ASSERT above, which requires all three.
    const auto statuses = manager.list_status();
    ASSERT_EQ(statuses.size(), 3u);
    for (const auto& status : statuses) {
        EXPECT_TRUE(status.connected) << status.ward_id;
    }
}

// karshipta#270, MV-02 (Yigitcan Ozturk's proposed matrix on #270): one
// ward loses its link while the other two remain connected and keep
// reporting their own state, unaffected.
//
// "Losing a link" here means destroying that one ward's fake autopilot, so
// the real WardConnection simply stops hearing heartbeats and MAVSDK's own
// timeout (not anything karshipta drives directly) eventually flips
// is_connected() to false - the same mechanism a real radio dropout would
// trigger, not a synthetic shortcut. The fake's destruction is dispatched
// onto its own detached thread before waiting on anything, deliberately
// mirroring WardConnection::disconnect()'s own karshipta#274 fix: the fake
// is a raw mavsdk::Mavsdk core, constructed directly rather than through
// WardConnection, so it has no equivalent protection against #273's
// MavsdkImpl::remove_connection() hazard on its own - destroying it inline
// on this thread would reintroduce exactly the risk #274 fixed, just on
// the test's side of the connection instead of karshipta's. The other two
// fakes, never dropped, still get the same detached teardown at the end
// of this test for the reason detach_fake_teardown's own comment gives -
// tearing down several real MAVSDK cores inline at once is the actual
// risk, not just the one this test deliberately drops. Verified locally
// with the detached fake teardown in place: 8 consecutive runs under
// added CPU load, all green, ~5.4s each.
TEST_F(WardManagerTest, OneWardLosesLinkWhileOthersStayConnected) {
    std::vector<MultiVehicleFake> fakes;
    for (const auto& [port, system_id] :
         std::vector<std::pair<uint16_t, uint8_t>>{{25320, 31}, {25321, 32}, {25322, 33}}) {
        fakes.push_back(
            MultiVehicleFake{port, system_id, std::make_unique<FakeInAirAutopilot>(system_id, port)});
    }
    DetachFakesOnExit detach_guard(fakes);

    auto manager = make_manager();
    for (size_t i = 0; i < fakes.size(); ++i) {
        const auto ward_id = "mv02-" + std::to_string(i);
        const auto ack = manager.handle_add_ward(make_add_request(
            "req-" + ward_id, ward_id, "udpin://127.0.0.1:" + std::to_string(fakes[i].port),
            fakes[i].system_id));
        ASSERT_EQ(ack.status(), karshipta::v1::WARD_CONFIG_STATUS_ACCEPTED) << ward_id;
    }

    auto all_connected = [&manager] {
        for (size_t i = 0; i < 3; ++i) {
            if (!manager.is_connected("mv02-" + std::to_string(i))) return false;
        }
        return true;
    };
    for (int attempt = 0; attempt < 100 && !all_connected(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(all_connected()) << "not all three wards connected before the drop";

    // Drop mv02-1's link. See this test's own doc comment for why the
    // fake's destruction runs on a detached thread rather than inline here.
    std::thread([fake = std::move(fakes[1].autopilot)]() mutable { fake.reset(); }).detach();

    bool dropped = false;
    for (int attempt = 0; attempt < 200 && !dropped; ++attempt) {
        dropped = !manager.is_connected("mv02-1");
        if (!dropped) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(dropped) << "mv02-1 never registered as disconnected";

    // The other two must not have been affected at all: still connected,
    // still their own status entries, no cross-ward interference from the
    // drop or from mv02-1's own reconnect_worker now retrying in the
    // background.
    EXPECT_TRUE(manager.is_connected("mv02-0"));
    EXPECT_TRUE(manager.is_connected("mv02-2"));
    const auto statuses = manager.list_status();
    ASSERT_EQ(statuses.size(), 3u);
}

// karshipta#270, MV-03 (Yigitcan Ozturk's proposed matrix on #270): the
// disconnected ward reconnects under the same identity without duplicate
// registration or cross-vehicle state contamination. Builds directly on
// MV-02 above (same drop mechanism and the same reasoning for why the old
// fake's destruction is dispatched to a detached thread); the new step
// here is standing a fresh fake back up on the dropped ward's own port and
// system id and confirming WardManager's existing reconnect_worker (still
// running the whole time, never stopped or re-started) picks it back up
// on its own, with no new ward_id ever created and the other two wards
// never affected. This is closer to #273's own documented residual gap
// than MV-01/MV-02 are (a connect racing a teardown, now both involving
// the same ward's own reconnect path rather than a different ward's), so
// it is the one most likely to surface anything left. The two wards never
// dropped also get the same detached teardown as the others at the end of
// this test, for detach_fake_teardown's own reasons. Verified locally
// with the detached fake teardown in place: 8 consecutive runs under
// added CPU load, all green, ~7.4s each.
TEST_F(WardManagerTest, DisconnectedWardReconnectsUnderSameIdentity) {
    std::vector<MultiVehicleFake> fakes;
    for (const auto& [port, system_id] :
         std::vector<std::pair<uint16_t, uint8_t>>{{25330, 41}, {25331, 42}, {25332, 43}}) {
        fakes.push_back(
            MultiVehicleFake{port, system_id, std::make_unique<FakeInAirAutopilot>(system_id, port)});
    }
    DetachFakesOnExit detach_guard(fakes);

    auto manager = make_manager();
    for (size_t i = 0; i < fakes.size(); ++i) {
        const auto ward_id = "mv03-" + std::to_string(i);
        const auto ack = manager.handle_add_ward(make_add_request(
            "req-" + ward_id, ward_id, "udpin://127.0.0.1:" + std::to_string(fakes[i].port),
            fakes[i].system_id));
        ASSERT_EQ(ack.status(), karshipta::v1::WARD_CONFIG_STATUS_ACCEPTED) << ward_id;
    }

    auto all_connected = [&manager] {
        for (size_t i = 0; i < 3; ++i) {
            if (!manager.is_connected("mv03-" + std::to_string(i))) return false;
        }
        return true;
    };
    for (int attempt = 0; attempt < 100 && !all_connected(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(all_connected()) << "not all three wards connected before the drop";

    const uint16_t dropped_port = fakes[1].port;
    const uint8_t dropped_system_id = fakes[1].system_id;
    std::thread([fake = std::move(fakes[1].autopilot)]() mutable { fake.reset(); }).detach();

    bool dropped = false;
    for (int attempt = 0; attempt < 200 && !dropped; ++attempt) {
        dropped = !manager.is_connected("mv03-1");
        if (!dropped) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(dropped) << "mv03-1 never registered as disconnected";

    // Stand a fresh fake back up on the SAME port and system id. The
    // existing reconnect_worker for mv03-1 (started once, never stopped)
    // is already looping on connect_with_retry() in the background and
    // should find this without any further action from the test.
    fakes[1].autopilot = std::make_unique<FakeInAirAutopilot>(dropped_system_id, dropped_port);

    bool reconnected = false;
    for (int attempt = 0; attempt < 100 && !reconnected; ++attempt) {
        reconnected = manager.is_connected("mv03-1");
        if (!reconnected) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(reconnected) << "mv03-1 never reconnected under its original identity";

    // No duplicate registration: still exactly the 3 original ward_ids,
    // nothing new created by the reconnect.
    const auto ids = manager.list_ward_ids();
    EXPECT_EQ(ids.size(), 3u);
    for (const auto& ward_id : {"mv03-0", "mv03-1", "mv03-2"}) {
        EXPECT_NE(std::find(ids.begin(), ids.end(), ward_id), ids.end()) << ward_id;
    }

    // No cross-vehicle contamination: the two wards that were never
    // touched are still exactly as connected as they were before any of
    // this happened.
    EXPECT_TRUE(manager.is_connected("mv03-0"));
    EXPECT_TRUE(manager.is_connected("mv03-2"));
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

TEST_F(WardManagerTest, RejectViewerEnvelopeRejectsGetLogEntriesWithResponse) {
    auto manager = make_manager();

    karshipta::v1::GetLogEntries request;
    request.set_request_id("req-viewer");
    request.set_ward_id("alpha-1");
    karshipta::v1::Envelope envelope;
    *envelope.mutable_get_log_entries() = request;

    manager.reject_viewer_envelope(/*client=*/7, envelope);

    // Sent only to the viewer client, not broadcast - same reasoning as the
    // live kGetLogEntries case this mirrors.
    const auto sent = transport_.sent_envelopes(7);
    ASSERT_EQ(sent.size(), 1u);
    ASSERT_TRUE(sent.front().has_log_entry_list());
    const auto& response = sent.front().log_entry_list();
    EXPECT_EQ(response.request_id(), "req-viewer");
    EXPECT_EQ(response.status(), karshipta::v1::LOG_ENTRY_LIST_STATUS_REJECTED);
    EXPECT_EQ(response.message(), "read-only session");
    EXPECT_TRUE(transport_.broadcast_envelopes().empty());
}

TEST_F(WardManagerTest, RejectViewerEnvelopeRejectsDownloadLogFileWithAck) {
    auto manager = make_manager();

    karshipta::v1::DownloadLogFile request;
    request.set_request_id("req-viewer");
    request.set_ward_id("alpha-1");
    request.set_log_id(2);
    karshipta::v1::Envelope envelope;
    *envelope.mutable_download_log_file() = request;

    manager.reject_viewer_envelope(/*client=*/7, envelope);

    const auto sent = transport_.sent_envelopes(7);
    ASSERT_EQ(sent.size(), 1u);
    ASSERT_TRUE(sent.front().has_download_log_file_ack());
    const auto& ack = sent.front().download_log_file_ack();
    EXPECT_EQ(ack.request_id(), "req-viewer");
    EXPECT_EQ(ack.log_id(), 2u);
    EXPECT_EQ(ack.status(), karshipta::v1::LOG_DOWNLOAD_STATUS_REJECTED);
    EXPECT_EQ(ack.message(), "read-only session");
    EXPECT_TRUE(transport_.broadcast_envelopes().empty());
}

TEST_F(WardManagerTest, RejectViewerEnvelopeRejectsStartCalibrationWithProgress) {
    auto manager = make_manager();

    karshipta::v1::StartCalibration request;
    request.set_request_id("req-viewer");
    request.set_ward_id("alpha-1");
    request.set_calibration_type(karshipta::v1::CALIBRATION_TYPE_MAGNETOMETER);
    karshipta::v1::Envelope envelope;
    *envelope.mutable_start_calibration() = request;

    manager.reject_viewer_envelope(/*client=*/7, envelope);

    const auto sent = transport_.sent_envelopes(7);
    ASSERT_EQ(sent.size(), 1u);
    ASSERT_TRUE(sent.front().has_calibration_progress());
    const auto& progress = sent.front().calibration_progress();
    EXPECT_EQ(progress.request_id(), "req-viewer");
    EXPECT_EQ(progress.calibration_type(), karshipta::v1::CALIBRATION_TYPE_MAGNETOMETER);
    EXPECT_EQ(progress.status(), karshipta::v1::CALIBRATION_STATUS_REJECTED);
    EXPECT_EQ(progress.message(), "read-only session");
    EXPECT_TRUE(transport_.broadcast_envelopes().empty());
}

TEST_F(WardManagerTest, RejectViewerEnvelopeIgnoresCancelCalibrationSilently) {
    auto manager = make_manager();

    karshipta::v1::CancelCalibration request;
    request.set_request_id("req-viewer");
    request.set_ward_id("alpha-1");
    request.set_calibration_request_id("some-run");
    karshipta::v1::Envelope envelope;
    *envelope.mutable_cancel_calibration() = request;

    // No ack channel exists for Cancel at all - a viewer's attempt is
    // logged and otherwise silently ignored, same as a stale/mistargeted
    // cancel from a real operator (CalibrationExecutor::cancel()'s own
    // doc comment).
    manager.reject_viewer_envelope(/*client=*/7, envelope);

    EXPECT_TRUE(transport_.sent_envelopes(7).empty());
    EXPECT_TRUE(transport_.broadcast_envelopes().empty());
}

// ---------- Flight mode mapping ----------

// mavsdk::Telemetry::FlightMode's mapping onto karshipta.v1.FlightMode
// (ward_manager_internal::to_proto_flight_mode) is a plain switch with a
// default case, unlike herald_ward_manager.cpp's to_ward_class(), which was
// already exhaustively tested. A default silently absorbs a mode MAVSDK adds
// in a future SDK bump into FLIGHT_MODE_UNKNOWN with no compiler warning and
// no test failure; this test pins every mode MAVSDK 3.17 actually defines to
// its expected proto value, so a new mode shows up here as a case this list
// doesn't cover, not as a silent behavior change.
TEST(WardManagerFlightModeMapping, MapsEveryKnownFlightModeToExpectedProtoValue) {
    using mavsdk::Telemetry;
    const std::vector<std::pair<Telemetry::FlightMode, karshipta::v1::FlightMode>> cases = {
        {Telemetry::FlightMode::Unknown, karshipta::v1::FLIGHT_MODE_UNKNOWN},
        {Telemetry::FlightMode::Ready, karshipta::v1::FLIGHT_MODE_UNKNOWN},
        {Telemetry::FlightMode::Takeoff, karshipta::v1::FLIGHT_MODE_TAKEOFF},
        {Telemetry::FlightMode::Hold, karshipta::v1::FLIGHT_MODE_HOLD},
        {Telemetry::FlightMode::Mission, karshipta::v1::FLIGHT_MODE_MISSION},
        {Telemetry::FlightMode::ReturnToLaunch, karshipta::v1::FLIGHT_MODE_RETURN},
        {Telemetry::FlightMode::Land, karshipta::v1::FLIGHT_MODE_LAND},
        {Telemetry::FlightMode::Offboard, karshipta::v1::FLIGHT_MODE_OFFBOARD},
        {Telemetry::FlightMode::FollowMe, karshipta::v1::FLIGHT_MODE_UNKNOWN},
        {Telemetry::FlightMode::Manual, karshipta::v1::FLIGHT_MODE_MANUAL},
        {Telemetry::FlightMode::Altctl, karshipta::v1::FLIGHT_MODE_UNKNOWN},
        {Telemetry::FlightMode::Posctl, karshipta::v1::FLIGHT_MODE_POSITION},
        {Telemetry::FlightMode::Acro, karshipta::v1::FLIGHT_MODE_UNKNOWN},
        {Telemetry::FlightMode::Stabilized, karshipta::v1::FLIGHT_MODE_UNKNOWN},
        {Telemetry::FlightMode::Rattitude, karshipta::v1::FLIGHT_MODE_UNKNOWN},
    };

    for (const auto& [mavsdk_mode, expected] : cases) {
        EXPECT_EQ(ward_manager_internal::to_proto_flight_mode(mavsdk_mode), expected)
            << "mavsdk::Telemetry::FlightMode index " << static_cast<int>(mavsdk_mode);
    }
}
