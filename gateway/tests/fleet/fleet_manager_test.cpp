#include "fleet_manager.h"

#include <algorithm>
#include <filesystem>
#include <memory>
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
    // Not exercised by these tests; satisfies the interface.
    void disconnect(ClientId /*client*/) override {}

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
    // database per test, never touching disk. Fleet/Zone and FleetMission
    // each get their own in-memory database, matching production's two
    // distinct db_path/fleet_mission_db_path arguments.
    FleetManager manager_{transport_, std::filesystem::path(":memory:"),
                          std::filesystem::path(":memory:")};

    // Builds a bare WardManager (no wards registered) wired the same way
    // main.cpp wires one to manager_: its command-outcome/mission-upload-
    // outcome observers call straight into manager_'s own handlers. Every
    // FleetMission test that needs to dispatch through WardManager uses
    // this instead of a standalone WardManager, so a synchronous rejection
    // (dispatch_command()/dispatch_mission_upload_and_start() against an
    // unregistered ward_id, which every test ward_id here is) is actually
    // correlated back into manager_'s ward_states, exactly as it would be
    // in production.
    std::shared_ptr<mavsdk::Mavsdk> core_ = WardConnection::create_shared_core();
    WardManager ward_manager_{core_, transport_};

    void SetUp() override {
        ward_manager_.set_command_outcome_observer(
            [this](const karshipta::v1::CommandAck& ack) { manager_.handle_command_outcome(ack); });
        ward_manager_.set_mission_upload_outcome_observer(
            [this](const std::string& ward_id, const std::string& mission_id, bool success,
                   const std::string& message) {
                manager_.handle_mission_upload_outcome(ward_id, mission_id, success, message);
            });
    }
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

// ---------- Fleet mission ----------

namespace {
// One waypoint is enough to exercise routing; these tests care about
// per-ward state transitions, not route content.
karshipta::v1::WardMissionPlan make_plan(const std::string& ward_id) {
    karshipta::v1::WardMissionPlan plan;
    plan.set_ward_id(ward_id);
    auto* item = plan.add_items();
    item->set_seq(0);
    item->set_action(karshipta::v1::MISSION_ACTION_WAYPOINT);
    item->mutable_position()->set_latitude_deg(1.0);
    item->mutable_position()->set_longitude_deg(1.0);
    return plan;
}
}  // namespace

TEST_F(FleetManagerTest, CreateFleetMissionRejectsUnknownFleetId) {
    karshipta::v1::CreateFleetMission request;
    request.set_request_id("req-1");
    request.set_fleet_id("ghost");
    *request.add_ward_plans() = make_plan("alpha-1");

    const auto ack = manager_.handle_create_fleet_mission(request, ward_manager_);
    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("ghost"), std::string::npos) << ack.message();
    EXPECT_TRUE(transport_.broadcast_envelopes().empty());
}

TEST_F(FleetManagerTest, CreateFleetMissionRejectsEmptyWardPlans) {
    karshipta::v1::CreateFleetMission request;
    request.set_request_id("req-1");

    const auto ack = manager_.handle_create_fleet_mission(request, ward_manager_);
    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("no ward plans"), std::string::npos) << ack.message();
}

TEST_F(FleetManagerTest, CreateFleetMissionAcceptsAdHocSelectionAndMarksUnknownWardsRejected) {
    karshipta::v1::CreateFleetMission request;
    request.set_request_id("req-1");
    // fleet_id left empty: an ad-hoc selection of individual wards, not tied
    // to any saved Fleet - mirrors the console wizard's "ad hoc wards" path.
    request.set_mission_name("Sweep");
    request.set_repeat_count(2);
    *request.add_ward_plans() = make_plan("alpha-1");
    *request.add_ward_plans() = make_plan("alpha-2");

    const auto ack = manager_.handle_create_fleet_mission(request, ward_manager_);
    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_MISSION_ACK_STATUS_ACCEPTED);
    ASSERT_FALSE(ack.fleet_mission_id().empty());

    // Neither alpha-1 nor alpha-2 is a registered ward, so
    // dispatch_mission_upload_and_start() rejects both synchronously; each
    // ward's own route is genuinely independent (the actual fix over the
    // old flat-broadcast design), which is why this asserts two distinct
    // ward_ids rather than one shared route.
    const auto envelopes = transport_.broadcast_envelopes();
    const auto fleet_mission_envelope =
        std::find_if(envelopes.begin(), envelopes.end(),
                     [](const auto& envelope) { return envelope.has_fleet_mission(); });
    ASSERT_NE(fleet_mission_envelope, envelopes.end());
    const auto& mission = fleet_mission_envelope->fleet_mission();
    ASSERT_EQ(mission.ward_states_size(), 2);
    for (const auto& state : mission.ward_states()) {
        EXPECT_EQ(state.status(), karshipta::v1::WARD_MISSION_STATUS_REJECTED);
        EXPECT_NE(state.message().find("unknown ward_id"), std::string::npos) << state.message();
    }
}

TEST_F(FleetManagerTest, StopFleetMissionRejectsUnknownFleetMissionId) {
    karshipta::v1::StopFleetMission request;
    request.set_request_id("req-1");
    request.set_fleet_mission_id("ghost");

    const auto ack = manager_.handle_stop_fleet_mission(request, ward_manager_);
    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("ghost"), std::string::npos) << ack.message();
}

TEST_F(FleetManagerTest, StopFleetMissionSkipsWardsAlreadyRejectedAndReportsNothingToStop) {
    // alpha-1 is unregistered, so create's own dispatch_mission_upload_and_start()
    // rejects it synchronously - it is already REJECTED (never started)
    // before Stop ever runs, so Stop's "skip already-settled wards" guard
    // must leave it alone rather than re-dispatching a stop command to a
    // ward that was never actually flying.
    karshipta::v1::CreateFleetMission create;
    create.set_request_id("req-1");
    *create.add_ward_plans() = make_plan("alpha-1");
    const auto fleet_mission_id = manager_.handle_create_fleet_mission(create, ward_manager_).fleet_mission_id();

    karshipta::v1::StopFleetMission stop;
    stop.set_request_id("req-2");
    stop.set_fleet_mission_id(fleet_mission_id);
    // action left unspecified: defaults to RTL.
    const auto ack = manager_.handle_stop_fleet_mission(stop, ward_manager_);
    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_MISSION_ACK_STATUS_ACCEPTED);
    EXPECT_NE(ack.message().find("no active wards to stop"), std::string::npos) << ack.message();

    const auto envelopes = transport_.broadcast_envelopes();
    const karshipta::v1::FleetMission* last_fleet_mission = nullptr;
    for (auto it = envelopes.rbegin(); it != envelopes.rend(); ++it) {
        if (it->has_fleet_mission()) {
            last_fleet_mission = &it->fleet_mission();
            break;
        }
    }
    ASSERT_NE(last_fleet_mission, nullptr);
    ASSERT_EQ(last_fleet_mission->ward_states_size(), 1);
    // Still REJECTED, not bumped to STOPPING: nothing was actually dispatched.
    EXPECT_EQ(last_fleet_mission->ward_states(0).status(), karshipta::v1::WARD_MISSION_STATUS_REJECTED);
    EXPECT_EQ(last_fleet_mission->status(), karshipta::v1::FLEET_MISSION_STATUS_ACTIVE);
}

TEST_F(FleetManagerTest, RemoveFleetMissionAcceptsWhenEveryWardIsRejectedNeverStarted) {
    // The unregistered ward is immediately REJECTED by create (never
    // started, nothing flying) - REJECTED counts as settled for Remove's
    // gate, same as STOPPED would.
    karshipta::v1::CreateFleetMission create;
    create.set_request_id("req-1");
    *create.add_ward_plans() = make_plan("alpha-1");
    const auto create_ack = manager_.handle_create_fleet_mission(create, ward_manager_);
    ASSERT_EQ(create_ack.status(), karshipta::v1::FLEET_MISSION_ACK_STATUS_ACCEPTED);

    karshipta::v1::RemoveFleetMission remove;
    remove.set_request_id("req-2");
    remove.set_fleet_mission_id(create_ack.fleet_mission_id());
    const auto ack = manager_.handle_remove_fleet_mission(remove);
    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_MISSION_ACK_STATUS_ACCEPTED);
}

TEST_F(FleetManagerTest, RemoveFleetMissionRejectsWhileAWardIsStillActive) {
    // Same registered-but-unconnected-ward technique as the Update test
    // above: dispatch_mission_upload_and_start() accepts synchronously, so
    // the ward stays UPLOADING - genuinely unsettled.
    WardConfig cfg;
    cfg.ward_id = "alpha-1";
    cfg.connection_url = "udpin://127.0.0.1:24997";
    ASSERT_TRUE(ward_manager_.add_ward(cfg));

    karshipta::v1::CreateFleetMission create;
    create.set_request_id("req-1");
    *create.add_ward_plans() = make_plan("alpha-1");
    const auto create_ack = manager_.handle_create_fleet_mission(create, ward_manager_);
    ASSERT_EQ(create_ack.status(), karshipta::v1::FLEET_MISSION_ACK_STATUS_ACCEPTED);

    karshipta::v1::RemoveFleetMission remove;
    remove.set_request_id("req-2");
    remove.set_fleet_mission_id(create_ack.fleet_mission_id());
    const auto ack = manager_.handle_remove_fleet_mission(remove);
    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("still active"), std::string::npos) << ack.message();
}

TEST_F(FleetManagerTest, RemoveFleetMissionRejectsUnknownFleetMissionId) {
    karshipta::v1::RemoveFleetMission request;
    request.set_request_id("req-1");
    request.set_fleet_mission_id("ghost");

    const auto ack = manager_.handle_remove_fleet_mission(request);
    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
}

TEST_F(FleetManagerTest, UpdateFleetMissionRoutesRejectsWhileAWardIsStillActive) {
    // A registered-but-unconnected ward (add_ward() only builds the object
    // graph, see its own doc comment - no SITL needed) makes
    // dispatch_mission_upload_and_start() accept synchronously, so the ward
    // stays UPLOADING (not REJECTED) - genuinely unsettled, the same as a
    // real in-flight ACTIVE mission would be for this gate's purposes.
    WardConfig cfg;
    cfg.ward_id = "alpha-1";
    cfg.connection_url = "udpin://127.0.0.1:24996";
    ASSERT_TRUE(ward_manager_.add_ward(cfg));

    karshipta::v1::CreateFleetMission create;
    create.set_request_id("req-1");
    *create.add_ward_plans() = make_plan("alpha-1");
    const auto create_ack = manager_.handle_create_fleet_mission(create, ward_manager_);
    ASSERT_EQ(create_ack.status(), karshipta::v1::FLEET_MISSION_ACK_STATUS_ACCEPTED);

    karshipta::v1::UpdateFleetMissionRoutes update;
    update.set_request_id("req-2");
    update.set_fleet_mission_id(create_ack.fleet_mission_id());
    *update.add_ward_plans() = make_plan("alpha-1");
    const auto ack = manager_.handle_update_fleet_mission_routes(update, ward_manager_);
    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("still active"), std::string::npos) << ack.message();
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

TEST_F(FleetManagerTest, RejectViewerEnvelopeAnswersCreateFleetMissionWithRejectedAck) {
    karshipta::v1::Envelope envelope;
    auto* request = envelope.mutable_create_fleet_mission();
    request->set_request_id("req-1");

    manager_.reject_viewer_envelope(/*client=*/1, envelope);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_fleet_mission_ack());
    const auto& ack = envelopes.front().fleet_mission_ack();
    EXPECT_EQ(ack.request_id(), "req-1");
    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
    EXPECT_EQ(ack.message(), "read-only session");
}

TEST_F(FleetManagerTest, RejectViewerEnvelopeAnswersStopFleetMissionWithRejectedAck) {
    karshipta::v1::Envelope envelope;
    auto* request = envelope.mutable_stop_fleet_mission();
    request->set_request_id("req-1");
    request->set_fleet_mission_id("team-a-mission");

    manager_.reject_viewer_envelope(/*client=*/1, envelope);

    const auto envelopes = transport_.broadcast_envelopes();
    ASSERT_EQ(envelopes.size(), 1u);
    ASSERT_TRUE(envelopes.front().has_fleet_mission_ack());
    const auto& ack = envelopes.front().fleet_mission_ack();
    EXPECT_EQ(ack.fleet_mission_id(), "team-a-mission");
    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_MISSION_ACK_STATUS_REJECTED);
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

// ---------- Store failure safety ----------

// A read-only containing directory forces the underlying FleetZoneStore to
// throw (see fleet_zone_store_test.cpp's matching test for why the directory,
// not the file itself, has to be the one made read-only) on every write it
// attempts. Before this fix, that exception propagated straight out of the
// handler: uncaught in main.cpp's envelope switch, it would have terminated
// the whole gateway process over one bad request. Not a fixture test (needs
// its own real file in its own directory, not manager_'s ":memory:"
// database).
TEST(FleetManagerCrashSafetyTest, StoreFailureBecomesRejectedAckInsteadOfAnUncaughtException) {
    const auto dir =
        std::filesystem::temp_directory_path() / "karshipta_fleet_manager_readonly_test_dir";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directory(dir);
    const auto path = dir / "store.db";
    const auto fleet_mission_path = dir / "fleet_missions.db";
    FakeTransport transport;
    // Both opened while the directory is still writable; this test only
    // exercises Fleet, fleet_mission_path just needs to open successfully.
    FleetManager manager(transport, path, fleet_mission_path);

    std::filesystem::permissions(dir, std::filesystem::perms::owner_write,
                                  std::filesystem::perm_options::remove);

    karshipta::v1::CreateFleet request;
    request.set_request_id("req-1");
    request.set_name("Team A");

    // If this throws, it propagates out of the TEST body uncaught: gtest
    // reports it as a crash, not a normal assertion failure, exactly
    // mirroring the uncaught-exception-kills-the-process failure mode this
    // fix closes.
    const auto ack = manager.handle_create_fleet(request);
    EXPECT_EQ(ack.status(), karshipta::v1::FLEET_ACK_STATUS_REJECTED);
    EXPECT_NE(ack.message().find("internal error"), std::string::npos) << ack.message();

    std::filesystem::permissions(dir, std::filesystem::perms::owner_write,
                                  std::filesystem::perm_options::add);
    std::filesystem::remove_all(dir);
}
