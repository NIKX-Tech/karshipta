#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include <karshipta/v1/envelope.pb.h>

#include "transport.h"
#include "vehicle_manager.h"
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
    auto vehicle_manager = VehicleManager::create(*transport, kPersistencePath);

    transport->on_connect(
        [&vehicle_manager](const Transport::ClientId client) { vehicle_manager->send_vehicle_info(client); });

    transport->on_receive([&vehicle_manager, &transport](const Transport::ClientId client,
                                                          const std::vector<uint8_t>& bytes) {
        karshipta::v1::Envelope envelope;
        if (!envelope.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
            spdlog::warn("undecodable {} byte frame from client {}", bytes.size(), client);
            return;
        }
        switch (envelope.payload_case()) {
            case karshipta::v1::Envelope::kCommand:
                vehicle_manager->dispatch_command(envelope.command());
                break;
            case karshipta::v1::Envelope::kAddVehicle: {
                const auto ack = vehicle_manager->handle_add_vehicle(envelope.add_vehicle());
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_vehicle_config_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
                break;
            }
            case karshipta::v1::Envelope::kRemoveVehicle: {
                const auto ack = vehicle_manager->handle_remove_vehicle(envelope.remove_vehicle());
                karshipta::v1::Envelope ack_envelope;
                *ack_envelope.mutable_vehicle_config_ack() = ack;
                transport->broadcast(serialize_envelope(ack_envelope));
                break;
            }
            case karshipta::v1::Envelope::kMissionUpload:
                spdlog::warn("mission upload from client {} ignored: missions land in M5", client);
                break;
            default:
                spdlog::warn("unexpected downstream payload kind {} from client {}",
                             static_cast<int>(envelope.payload_case()), client);
                break;
        }
    });
    transport->start();

    if (vehicle_manager->restore_and_start() == 0) {
        // First run, nothing persisted yet: seed the same default SITL
        // vehicle earlier milestones connected to, so `cmake --build && run`
        // still works out of the box with no console/test-client needed.
        karshipta::v1::AddVehicle seed;
        seed.set_vehicle_id("sitl-1");
        seed.set_connection_url("udp://:14540");
        seed.set_type(karshipta::v1::VEHICLE_TYPE_MULTIROTOR);
        vehicle_manager->handle_add_vehicle(seed);
    }
    vehicle_manager->start_publishing();

    // Runs until externally killed: with a dynamic fleet there's no longer
    // one hardcoded vehicle whose disconnect should end the process. No
    // signal handling (Ctrl+C/SIGTERM) exists yet to force_stop_all() a
    // still-flying fleet first before exiting; a pre-existing gap (see
    // VehicleManager's destructor comment), not something this loop adds.
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
