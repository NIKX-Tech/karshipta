#include "vehicle_manager.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

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
