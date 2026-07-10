//
// Created by amir abkhoshk on 09/07/2026.
//

#include "vehicle.h"

#include <algorithm>
#include <atomic>
#include <future>
#include <stdexcept>
#include <thread>

#include <spdlog/spdlog.h>

VehicleConnection::VehicleConnection(std::shared_ptr<mavsdk::Mavsdk> mavsdk,
                                      const std::string& drone_url,
                                      std::optional<uint32_t> expected_system_id)
    : mavsdk_(std::move(mavsdk)), expected_system_id_(expected_system_id) {
    set_drone_url(drone_url);
}

VehicleConnection::~VehicleConnection() {
    disconnect();
}

std::shared_ptr<mavsdk::Mavsdk> VehicleConnection::create_shared_core() {
    return std::make_shared<mavsdk::Mavsdk>(
        mavsdk::Mavsdk::Configuration{mavsdk::ComponentType::GroundStation});
}

void VehicleConnection::set_drone_url(const std::string& drone_url) {
    this->connection_url_ = validate_drone_url(drone_url);
}

std::string VehicleConnection::get_drone_url() const {
    return this->connection_url_;
}
std::string VehicleConnection::validate_drone_url(const std::string& drone_url) {
    if (drone_url.empty()) {
        throw std::invalid_argument("Drone URL cannot be empty");
    }
    return drone_url;
}

std::optional<std::shared_ptr<mavsdk::System>> VehicleConnection::find_system(
    uint32_t system_id) const {
    for (const auto& candidate : mavsdk_->systems()) {
        if (candidate->get_system_id() == system_id) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<std::shared_ptr<mavsdk::System>> VehicleConnection::wait_for_system(
    uint32_t system_id) const {
    if (auto existing = find_system(system_id)) {
        return existing;
    }

    auto promise = std::make_shared<std::promise<std::shared_ptr<mavsdk::System>>>();
    auto fulfilled = std::make_shared<std::atomic_bool>(false);
    auto future = promise->get_future();

    const auto handle = mavsdk_->subscribe_on_new_system([this, system_id, promise, fulfilled]() {
        if (fulfilled->exchange(true)) {
            return;
        }
        if (auto found = find_system(system_id)) {
            promise->set_value(*found);
        } else {
            fulfilled->store(false);
        }
    });

    const auto status =
        future.wait_for(std::chrono::duration<double>(kAutopilotDiscoveryTimeoutS));
    mavsdk_->unsubscribe_on_new_system(handle);

    if (status != std::future_status::ready) {
        return std::nullopt;
    }
    return future.get();
}

VehicleConnection::ConnectResult VehicleConnection::connect() {
    if (!connection_added_) {
        const auto [connection_result, handle] =
            mavsdk_->add_any_connection_with_handle(connection_url_);

        if (connection_result != mavsdk::ConnectionResult::Success) {
            spdlog::error(
                "failed to add connection {}: result={}",
                connection_url_,
                static_cast<int>(connection_result));
            return ConnectResult::kSocketFailure;
        }

        connection_handle_ = handle;
        connection_added_ = true;
    }

    auto discovered_system = expected_system_id_
        ? wait_for_system(*expected_system_id_)
        : mavsdk_->first_autopilot(kAutopilotDiscoveryTimeoutS);

    if (!discovered_system) {
        spdlog::warn(
            "no matching autopilot discovered on {} within {}s",
            connection_url_,
            kAutopilotDiscoveryTimeoutS);
        return ConnectResult::kDiscoveryTimeout;
    }

    this->system_ = discovered_system.value();
    spdlog::info(
        "connected to {} (system_id={})", connection_url_, system_->get_system_id());
    return ConnectResult::kSuccess;
}

bool VehicleConnection::connect_with_retry(
    const std::stop_token& stop_token, std::chrono::milliseconds retry_interval) {
    constexpr auto kPollInterval = std::chrono::milliseconds(100);

    while (!stop_token.stop_requested()) {
        if (connect() == ConnectResult::kSuccess) {
            return true;
        }

        spdlog::warn(
            "retrying connection to {} in {}ms", connection_url_, retry_interval.count());

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
    if (!system_) {
        return;
    }

    // Cancel every subscription this instance handed out before dropping system_:
    // MAVSDK invokes those callbacks on its own threads, and a callback firing after
    // this object is gone would be a use-after-free.
    for (const auto& handle : connection_state_handles_) {
        system_->unsubscribe_is_connected(handle);
    }
    connection_state_handles_.clear();

    // mavsdk_ and system_ are shared_ptrs: any plugin object (DroneActions,
    // TelemetryInfo, ...) that called get_mavsdk()/get_system() holds its own
    // reference, so resetting ours here is safe no matter which object destructs
    // first; the underlying Mavsdk/System stay alive until every holder has
    // released them.
    spdlog::info("disconnected from {}", connection_url_);
    system_.reset();

    if (connection_added_) {
        mavsdk_->remove_connection(connection_handle_);
        connection_added_ = false;
    }
}

bool VehicleConnection::is_connected() const {
    return system_ != nullptr && system_->is_connected();
}

std::shared_ptr<mavsdk::System> VehicleConnection::get_system() const {
    return system_;
}

std::shared_ptr<mavsdk::Mavsdk> VehicleConnection::get_mavsdk() const {
    return mavsdk_;
}

mavsdk::System::IsConnectedHandle VehicleConnection::subscribe_connection_state(
    const mavsdk::System::IsConnectedCallback& callback) {
    if (!system_) {
        throw std::logic_error("subscribe_connection_state() called before a successful connect()");
    }
    const auto handle = system_->subscribe_is_connected(callback);
    connection_state_handles_.push_back(handle);
    return handle;
}

void VehicleConnection::unsubscribe_connection_state(mavsdk::System::IsConnectedHandle handle) {
    if (!system_) {
        return;
    }
    system_->unsubscribe_is_connected(handle);
    std::erase(connection_state_handles_, handle);
}