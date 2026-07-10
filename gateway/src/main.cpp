//
// Created by amir abkhoshk on 09/07/2026.
//

#include <cstdlib>

#include <spdlog/spdlog.h>

#include "vehicle.h"

namespace {
constexpr auto kConnectionUrl = "udp://:14540";
}  // namespace

int main() {
    auto mavsdk = VehicleConnection::create_shared_core();
    VehicleConnection vehicle(mavsdk, kConnectionUrl);

    if (!vehicle.connect()) {
        spdlog::error("failed to connect to {}", vehicle.get_drone_url());
        return EXIT_FAILURE;
    }

    spdlog::info(
        "connected to {} (is_connected={})", vehicle.get_drone_url(), vehicle.is_connected());

    vehicle.disconnect();
    spdlog::info("is_connected after disconnect: {}", vehicle.is_connected());

    return EXIT_SUCCESS;
}
