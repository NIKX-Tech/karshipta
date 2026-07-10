#include <chrono>
#include <cstdlib>
#include <thread>

#include <spdlog/spdlog.h>

#include "telemetry.h"
#include "vehicle_connection.h"

namespace {
constexpr auto kConnectionUrl = "udp://:14540";
constexpr float kPositionRateHz = 1.0f;
}  // namespace

int main() {
    auto mavsdk = VehicleConnection::create_shared_core();
    VehicleConnection vehicle(mavsdk, kConnectionUrl);

    if (vehicle.connect() != VehicleConnection::ConnectResult::kSuccess) {
        spdlog::error("failed to connect to {}", vehicle.get_connection_url());
        return EXIT_FAILURE;
    }

    spdlog::info("connected to {}", vehicle.get_connection_url());

    TelemetryInfo telemetry(vehicle);
    telemetry.set_telemetry_rate(kPositionRateHz);
    telemetry.subscribe_position();
    telemetry.subscribe_battery();

    while (vehicle.is_connected()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    spdlog::warn("link lost, exiting");
    return EXIT_SUCCESS;
}
