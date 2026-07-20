#include "fleet_manager.h"

#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <karshipta/v1/envelope.pb.h>

#include "transport.h"
#include "ward_connection.h"
#include "ward_manager.h"

namespace {

// Records every broadcast()/send() call instead of touching a real socket.
// Verbatim copy of ward_manager_test.cpp's FakeTransport: no shared test-util
// header exists in this repo yet, and this class is small enough that
// duplicating it per test file matches the existing precedent (see also
// main.cpp's/ward_manager.cpp's own separate serialize_envelope copies).
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

    [[nodiscard]] ClientRole role(ClientId /*client*/) const override { return ClientRole::kOperator; }

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

    // Every Envelope send() so far (targeted at any client), parsed. Unlike
    // ward_manager_test.cpp's copy of this class, this file's
    // send_fleet_zone_snapshot() test needs to inspect send() traffic too.
    [[nodiscard]] std::vector<karshipta::v1::Envelope> sent_envelopes() const {
        std::lock_guard lock(mutex_);
        std::vector<karshipta::v1::Envelope> envelopes;
        for (const auto& [client, bytes] : sent_) {
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

class FleetManagerTest : public ::testing::Test {
   protected:
    // Declaration order matters here: members initialize in this order, and
    // manager_ takes transport_ by reference, so transport_ must come first.
    FakeTransport transport_;
    // ":memory:" mirrors FleetZoneStoreTest: a fresh, private, in-process
    // database per test, never touching disk.
    FleetManager manager_{transport_, std::filesystem::path(":memory:")};
};

}  // namespace

// ---------- Fleet ----------

TEST_F(FleetManagerTest, CreateFleetAcceptsAndBroadcastsFleet) {
    karshipta::v1::CreateFleet request;
    request.set_request_id("req-1");
    request.set_name("Inspection Team");
    request.set_description("Roof surveys");

    const auto ack = manager_.handle_create_fleet(request);
    EXPECT_EQ(ack.request_id(), "req-1");
    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_ACK_STATUS_ACCEPTED);
    EXPECT_FALSE(ack.fleet_id().empty());

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_fleet());
    EXPECT_EQ(envelopes.front().fleet().fleet_id(), ack.fleet_id());
    EXPECT_EQ(envelopes.front().fleet().name(), "Inspection Team");
}

TEST_F(FleetManagerTest, RenameFleetRejectsUnknownIdWithReason) {
    karshipta::v1::RenameFleet request;
    request.set_request_id("req-1");
    request.set_fleet_id("ghost");
    request.set_name("New Name");

    const auto ack = manager_.handle_rename_fleet(request);
    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_ACK_STATUS_REJECTED);
    EXPECT_EQ(ack.fleet_id(), "ghost");
    EXPECT_NE(ack.message().find("ghost"), std::string::npos) << ack.message();
    EXPECT_TRUE(transport_.broadcast_envelopes().empty());
}

TEST_F(FleetManagerTest, DeleteFleetAcceptedAckCarriesFleetIdForEveryClient) {
    karshipta::v1::CreateFleet create;
    create.set_request_id("req-1");
    create.set_name("Team A");
    const auto fleet_id = manager_.handle_create_fleet(create).fleet_id();

    karshipta::v1::DeleteFleet request;
    request.set_request_id("req-2");
    request.set_fleet_id(fleet_id);
    const auto ack = manager_.handle_delete_fleet(request);

    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_ACK_STATUS_ACCEPTED);
    // fleet_id on the ack itself is how a client other than the requester
    // (which has no local Fleet object to send back) learns what to drop.
    EXPECT_EQ(ack.fleet_id(), fleet_id);
}

TEST_F(FleetManagerTest, AddThenRemoveWardFromFleetBroadcastsUpdatedMembership) {
    karshipta::v1::CreateFleet create;
    create.set_request_id("req-1");
    create.set_name("Team A");
    const auto fleet_id = manager_.handle_create_fleet(create).fleet_id();

    karshipta::v1::AddWardToFleet add;
    add.set_request_id("req-2");
    add.set_fleet_id(fleet_id);
    add.set_ward_id("alpha-1");
    const auto add_ack = manager_.handle_add_ward_to_fleet(add);
    EXPECT_EQ(add_ack.status(), karshipta::v1::FLEET_ACK_STATUS_ACCEPTED);

    auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 2u);  // create, then add
    ASSERT_TRUE(envelopes.back().has_fleet());
    ASSERT_EQ(envelopes.back().fleet().ward_ids_size(), 1);
    EXPECT_EQ(envelopes.back().fleet().ward_ids(0), "alpha-1");

    karshipta::v1::RemoveWardFromFleet remove;
    remove.set_request_id("req-3");
    remove.set_fleet_id(fleet_id);
    remove.set_ward_id("alpha-1");
    const auto remove_ack = manager_.handle_remove_ward_from_fleet(remove);
    EXPECT_EQ(remove_ack.status(), karshipta::v1::FLEET_ACK_STATUS_ACCEPTED);

    envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 3u);
    EXPECT_EQ(envelopes.back().fleet().ward_ids_size(), 0);
}

// ---------- Zone ----------

TEST_F(FleetManagerTest, CreateZoneRejectsFewerThanThreeVertices) {
    karshipta::v1::CreateZone request;
    request.set_request_id("req-1");
    request.set_name("No-fly");
    request.set_type(karshipta::v1::ZONE_TYPE_KEEP_OUT);
    auto* a = request.add_vertices();
    a->set_latitude_deg(1.0);
    a->set_longitude_deg(1.0);
    auto* b = request.add_vertices();
    b->set_latitude_deg(2.0);
    b->set_longitude_deg(1.0);

    const auto ack = manager_.handle_create_zone(request);
    EXPECT_EQ(ack.status(), karshipta::v1::ZONE_ACK_STATUS_REJECTED);
    EXPECT_TRUE(transport_.broadcast_envelopes().empty());
}

TEST_F(FleetManagerTest, CreateZoneAcceptsAndBroadcastsZone) {
    karshipta::v1::CreateZone request;
    request.set_request_id("req-1");
    request.set_name("No-fly");
    request.set_type(karshipta::v1::ZONE_TYPE_KEEP_OUT);
    for (const auto& [lat, lon] : {std::pair{1.0, 1.0}, std::pair{2.0, 1.0}, std::pair{1.5, 2.0}}) {
        auto* vertex = request.add_vertices();
        vertex->set_latitude_deg(lat);
        vertex->set_longitude_deg(lon);
    }
    request.set_altitude_max_m(120.0f);

    const auto ack = manager_.handle_create_zone(request);
    EXPECT_EQ(ack.status(), karshipta::v1::ZONE_ACK_STATUS_ACCEPTED);
    EXPECT_FALSE(ack.zone_id().empty());

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_zone());
    EXPECT_EQ(envelopes.front().zone().zone_id(), ack.zone_id());
    EXPECT_EQ(envelopes.front().zone().vertices_size(), 3);
}

TEST_F(FleetManagerTest, UpdateZoneRejectsUnknownId) {
    karshipta::v1::UpdateZone request;
    request.set_request_id("req-1");
    request.set_zone_id("ghost");
    request.set_name("New Name");
    request.set_type(karshipta::v1::ZONE_TYPE_KEEP_IN);

    const auto ack = manager_.handle_update_zone(request);
    EXPECT_EQ(ack.status(), karshipta::v1::ZONE_ACK_STATUS_REJECTED);
    EXPECT_EQ(ack.zone_id(), "ghost");
}

TEST_F(FleetManagerTest, DeleteZoneAcceptedAckCarriesZoneId) {
    karshipta::v1::CreateZone create;
    create.set_request_id("req-1");
    create.set_name("Staging");
    create.set_type(karshipta::v1::ZONE_TYPE_KEEP_IN);
    for (const auto& [lat, lon] : {std::pair{1.0, 1.0}, std::pair{2.0, 1.0}, std::pair{1.5, 2.0}}) {
        auto* vertex = create.add_vertices();
        vertex->set_latitude_deg(lat);
        vertex->set_longitude_deg(lon);
    }
    const auto zone_id = manager_.handle_create_zone(create).zone_id();

    karshipta::v1::DeleteZone request;
    request.set_request_id("req-2");
    request.set_zone_id(zone_id);
    const auto ack = manager_.handle_delete_zone(request);
    EXPECT_EQ(ack.status(), karshipta::v1::ZONE_ACK_STATUS_ACCEPTED);
    EXPECT_EQ(ack.zone_id(), zone_id);
}

// ---------- Fleet-wide mission assignment ----------

TEST_F(FleetManagerTest, FleetMissionAssignmentRejectsUnknownFleetWithGatewayEvent) {
    karshipta::v1::FleetMissionAssignment request;
    request.set_request_id("req-1");
    request.set_fleet_id("ghost");
    request.add_ward_ids("alpha-1");

    auto core = WardConnection::create_shared_core();
    WardManager ward_manager(core, transport_);
    manager_.handle_fleet_mission_assignment(request, ward_manager);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_event());
    const auto& event = envelopes.front().event();
    EXPECT_TRUE(event.ward_id().empty());  // gateway-level, not ward-scoped
    EXPECT_EQ(event.code(), "FLEET_MISSION_ASSIGNMENT_REJECTED");
    EXPECT_NE(event.message().find("ghost"), std::string::npos) << event.message();
}

TEST_F(FleetManagerTest, FleetMissionAssignmentRejectsEmptyWardSelection) {
    karshipta::v1::CreateFleet create;
    create.set_request_id("req-1");
    create.set_name("Team A");
    const auto fleet_id = manager_.handle_create_fleet(create).fleet_id();

    karshipta::v1::FleetMissionAssignment request;
    request.set_request_id("req-2");
    request.set_fleet_id(fleet_id);

    auto core = WardConnection::create_shared_core();
    WardManager ward_manager(core, transport_);
    manager_.handle_fleet_mission_assignment(request, ward_manager);

    const auto envelopes = transport_.broadcast_envelopes();
    // create_fleet's own broadcast, then the rejection event.
    ASSERT_EQ(envelopes.size(), 2u);
    ASSERT_TRUE(envelopes.back().has_event());
    EXPECT_EQ(envelopes.back().event().code(), "FLEET_MISSION_ASSIGNMENT_REJECTED");
}

TEST_F(FleetManagerTest, FleetMissionAssignmentFansOutRejectingEachUnknownWard) {
    karshipta::v1::CreateFleet create;
    create.set_request_id("req-1");
    create.set_name("Team A");
    const auto fleet_id = manager_.handle_create_fleet(create).fleet_id();

    karshipta::v1::FleetMissionAssignment request;
    request.set_request_id("req-2");
    request.set_fleet_id(fleet_id);
    request.add_ward_ids("alpha-1");
    request.add_ward_ids("alpha-2");
    request.set_mission_name("Sweep");

    auto core = WardConnection::create_shared_core();
    WardManager ward_manager(core, transport_);
    manager_.handle_fleet_mission_assignment(request, ward_manager);

    const auto envelopes = transport_.broadcast_envelopes();
    // create_fleet's own broadcast, then one MISSION_UPLOAD_REJECTED event
    // per selected ward (neither alpha-1 nor alpha-2 is a registered ward).
    ASSERT_EQ(envelopes.size(), 3u);
    EXPECT_TRUE(envelopes[1].has_event());
    EXPECT_EQ(envelopes[1].event().code(), "MISSION_UPLOAD_REJECTED");
    EXPECT_TRUE(envelopes[2].has_event());
    EXPECT_EQ(envelopes[2].event().code(), "MISSION_UPLOAD_REJECTED");
}

// ---------- Viewer rejection ----------

TEST_F(FleetManagerTest, RejectViewerEnvelopeAnswersCreateFleetWithRejectedAck) {
    karshipta::v1::Envelope envelope;
    auto* request = envelope.mutable_create_fleet();
    request->set_request_id("req-1");
    request->set_name("Team A");

    manager_.reject_viewer_envelope(/*client=*/1, envelope);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_fleet_ack());
    const auto& ack = envelopes.front().fleet_ack();
    EXPECT_EQ(ack.request_id(), "req-1");
    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_ACK_STATUS_REJECTED);
    EXPECT_EQ(ack.message(), "read-only session");
}

TEST_F(FleetManagerTest, RejectViewerEnvelopeAnswersFleetMissionAssignmentWithGatewayEvent) {
    karshipta::v1::Envelope envelope;
    auto* request = envelope.mutable_fleet_mission_assignment();
    request->set_request_id("req-1");
    request->set_fleet_id("team-a");

    manager_.reject_viewer_envelope(/*client=*/1, envelope);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_event());
    EXPECT_EQ(envelopes.front().event().code(), "FLEET_MISSION_ASSIGNMENT_REJECTED");
    EXPECT_EQ(envelopes.front().event().message(), "read-only session");
}

// ---------- Snapshot ----------

TEST_F(FleetManagerTest, SendFleetZoneSnapshotSendsOneEnvelopePerEntity) {
    karshipta::v1::CreateFleet create_fleet;
    create_fleet.set_request_id("req-1");
    create_fleet.set_name("Team A");
    (void)manager_.handle_create_fleet(create_fleet);

    karshipta::v1::CreateZone create_zone;
    create_zone.set_request_id("req-2");
    create_zone.set_name("No-fly");
    create_zone.set_type(karshipta::v1::ZONE_TYPE_KEEP_OUT);
    for (const auto& [lat, lon] : {std::pair{1.0, 1.0}, std::pair{2.0, 1.0}, std::pair{1.5, 2.0}}) {
        auto* vertex = create_zone.add_vertices();
        vertex->set_latitude_deg(lat);
        vertex->set_longitude_deg(lon);
    }
    (void)manager_.handle_create_zone(create_zone);

    manager_.send_fleet_zone_snapshot(/*client=*/1);

    const auto sent = transport_.sent_envelopes();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_TRUE(sent[0].has_fleet());
    EXPECT_EQ(sent[0].fleet().name(), "Team A");
    EXPECT_TRUE(sent[1].has_zone());
    EXPECT_EQ(sent[1].zone().name(), "No-fly");
}
