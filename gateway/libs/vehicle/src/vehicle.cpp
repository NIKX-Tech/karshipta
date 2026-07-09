//
// Created by amir abkhoshk on 09/07/2026.
//

#include "vehicle.h"

#include <algorithm>
#include <stdexcept>
#include <thread>

#include <spdlog/spdlog.h>

VehicleConnection::VehicleConnection(std::shared_ptr<mavsdk::Mavsdk> mavsdk_instance,
                                      const std::string& drone_url)
    : mavsdk_instance(std::move(mavsdk_instance)) {
    setDroneUrl(drone_url);
}

VehicleConnection::~VehicleConnection() {
    disconnect();
}

std::shared_ptr<mavsdk::Mavsdk> VehicleConnection::createSharedCore() {
    return std::make_shared<mavsdk::Mavsdk>(
        mavsdk::Mavsdk::Configuration{mavsdk::ComponentType::GroundStation});
}

void VehicleConnection::setDroneUrl(const std::string& drone_url) {
    this->connection_url = validateDroneUrl(drone_url);
}

std::string VehicleConnection::getDroneUrl() const {
    return this->connection_url;
}
std::string VehicleConnection::validateDroneUrl(const std::string& drone_url) {
    if (drone_url.empty()) {
        throw std::invalid_argument("Drone URL cannot be empty");
    }
    return drone_url;
}

bool VehicleConnection::connect() {
    if (!connection_added) {
        const auto [connection_result, handle] =
            mavsdk_instance->add_any_connection_with_handle(connection_url);

        if (connection_result != mavsdk::ConnectionResult::Success) {
            spdlog::error(
                "failed to add connection {}: result={}",
                connection_url,
                static_cast<int>(connection_result));
            return false;
        }

        connection_handle = handle;
        connection_added = true;
    }

    auto discovered_system = mavsdk_instance->first_autopilot(kAutopilotDiscoveryTimeoutS);
    if (!discovered_system) {
        spdlog::warn(
            "no autopilot discovered on {} within {}s", connection_url, kAutopilotDiscoveryTimeoutS);
        return false;
    }

    this->system = discovered_system.value();
    spdlog::info("connected to {}", connection_url);
    return true;
}

bool VehicleConnection::connectWithRetry(
    const std::stop_token& stop_token, std::chrono::milliseconds retry_interval) {
    constexpr auto kPollInterval = std::chrono::milliseconds(100);

    while (!stop_token.stop_requested()) {
        if (connect()) {
            return true;
        }

        spdlog::warn(
            "retrying connection to {} in {}ms", connection_url, retry_interval.count());

        auto remaining = retry_interval;
        while (remaining.count() > 0 && !stop_token.stop_requested()) {
            const auto tick = std::min(remaining, kPollInterval);
            std::this_thread::sleep_for(tick);
            remaining -= tick;
        }
    }

    return false;
}

void VehicleConnection::disconnect() {
    if (!system) {
        return;
    }

    // mavsdk_instance and system are shared_ptrs: any plugin object (DroneActions,
    // TelemetryInfo, ...) that called getMavsdk()/getSystem() holds its own reference,
    // so resetting ours here is safe no matter which object destructs first — the
    // underlying Mavsdk/System stay alive until every holder has released them.
    spdlog::info("disconnected from {}", connection_url);
    system.reset();

    if (connection_added) {
        mavsdk_instance->remove_connection(connection_handle);
        connection_added = false;
    }
}

bool VehicleConnection::isConnected() const {
    return system != nullptr && system->is_connected();
}

std::shared_ptr<mavsdk::System> VehicleConnection::getSystem() const {
    return system;
}

std::shared_ptr<mavsdk::Mavsdk> VehicleConnection::getMavsdk() const {
    return mavsdk_instance;
}

mavsdk::System::IsConnectedHandle VehicleConnection::subscribeConnectionState(
    const mavsdk::System::IsConnectedCallback& callback) {
    if (!system) {
        throw std::logic_error("subscribeConnectionState() called before a successful connect()");
    }
    return system->subscribe_is_connected(callback);
}

void VehicleConnection::unsubscribeConnectionState(mavsdk::System::IsConnectedHandle handle) {
    if (system) {
        system->unsubscribe_is_connected(handle);
    }
}
