#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include <karshipta/v1/envelope.pb.h>

#include "transport.h"
#include "ward_manager.h"
#include "websocket_transport.h"

namespace {
// Both relative to the repo root, matching how the binary is documented to
// be run (docs/quickstart-windows.md, gateway/CLAUDE.local.md).
// gateway/config/ is a tracked directory (see .gitkeep).
constexpr auto kNetworkConfigPath = "gateway/config/gateway.yaml";
// Generated state, not operator-managed; gitignored (gateway/.gitignore).
constexpr auto kPersistencePath = "gateway/config/fleet_state.yaml";

std::vector<uint8_t> serialize_envelope(const karshipta::v1::Envelope& envelope) {
    std::vector<uint8_t> bytes(envelope.ByteSizeLong());
    if (!envelope.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()))) {
        spdlog::error("failed to serialize an Envelope of {} bytes", bytes.size());
        bytes.clear();
    }
    return bytes;
}
}  // namespace

int main() {
    auto transport = WebsocketTransport::from_config(kNetworkConfigPath);
    auto ward_manager = WardManager::create(*transport, kPersistencePath);

    transport->on_connect(
        [&ward_manager](const Transport::ClientId client) { ward_manager->send_ward_info(client); });

    transport->on_receive([&ward_manager, &transport](const Transport::ClientId client,
                                                          const std::vector<uint8_t>& bytes) {
        karshipta::v1::Envelope envelope;
        if (!envelope.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
            spdlog::warn("undecodable {} byte frame from client {}", bytes.size(), client);
            return;
        }
        switch (envelope.payload_case()) {
            case karshipta::v1::Envelope::kCommand:
                ward_manager->dispatch_command(envelope.command());
                break;
            case karshipta::v1::Envelope::kAddWard: {
                const auto ack = ward_manager->handle_add_ward(envelope.add_ward());
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_ward_config_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
                break;
            }
            case karshipta::v1::Envelope::kRemoveWard: {
                const auto ack = ward_manager->handle_remove_ward(envelope.remove_ward());
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_ward_config_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
                break;
            }
            case karshipta::v1::Envelope::kMissionUpload:
                ward_manager->handle_mission_upload(envelope.mission_upload());
                break;
            case karshipta::v1::Envelope::kMissionFileUpload:
                ward_manager->handle_mission_file_upload(envelope.mission_file_upload());
                break;
            case karshipta::v1::Envelope::kMissionDownloadRequest:
                ward_manager->handle_mission_download_request(envelope.mission_download_request());
                break;
            default:
                spdlog::warn("unexpected downstream payload kind {} from client {}",
                             static_cast<int>(envelope.payload_case()), client);
                break;
        }
    });
    transport->start();

    if (ward_manager->restore_and_start() == 0) {
        // First run, nothing persisted yet: seed the same default SITL
        // ward earlier milestones connected to, so `cmake --build && run`
        // still works out of the box with no console/test-client needed.
        karshipta::v1::AddWard seed;
        seed.set_ward_id("sitl-1");
        seed.set_connection_url("udp://:14540");
        seed.set_ward_class(karshipta::v1::WARD_CLASS_MULTIROTOR);
        ward_manager->handle_add_ward(seed);
    }
    ward_manager->start_publishing();

    // Runs until externally killed: with a dynamic fleet there's no longer
    // one hardcoded ward whose disconnect should end the process. No
    // signal handling (Ctrl+C/SIGTERM) exists yet to force_stop_all() a
    // still-flying fleet first before exiting; a pre-existing gap (see
    // WardManager's destructor comment), not something this loop adds.
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
