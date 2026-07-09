#include <cstdlib>

#include <spdlog/spdlog.h>

#include "vehicle.h"

namespace {
constexpr auto kConnectionUrl = "udp://:14540";
}  // namespace

int main() {
    auto mavsdk = VehicleConnection::createSharedCore();
    VehicleConnection vehicle(mavsdk, kConnectionUrl);

    if (!vehicle.connect()) {
        spdlog::error("failed to connect to {}", vehicle.getDroneUrl());
        return EXIT_FAILURE;
    }

    spdlog::info("connected to {} (isConnected={})", vehicle.getDroneUrl(), vehicle.isConnected());

    vehicle.disconnect();
    spdlog::info("isConnected after disconnect: {}", vehicle.isConnected());

    return EXIT_SUCCESS;
}
