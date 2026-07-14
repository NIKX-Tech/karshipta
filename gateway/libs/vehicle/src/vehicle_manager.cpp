//
// Created by amir abkhoshk on 13/07/2026.
//

#include "vehicle_manager.h"

#include <karshipta/v1/envelope.pb.h>
#include <spdlog/spdlog.h>

VehicleManager::VehicleManager(std::shared_ptr<mavsdk::Mavsdk> mavsdk, Transport& tp)
    : mavsdk_(std::move(mavsdk)), transport_(tp) {}

bool VehicleManager::add_vehicle(const VehicleConfig& cfg) {
    if (managed_vehicles_.contains(cfg.vehicle_id)) {
        spdlog::warn("add_vehicle rejected: vehicle_id '{}' already registered", cfg.vehicle_id);
        return false;
    }
    for (const auto& [id, vehicle] : managed_vehicles_) {
        if (vehicle.system_id == cfg.system_id) {
            spdlog::warn("add_vehicle rejected: system_id {} already bound to vehicle_id '{}'",
                         cfg.system_id, id);
            return false;
        }
    }

    spdlog::debug("add_vehicle: building object graph for '{}' (system_id={}, url={})",
                  cfg.vehicle_id, cfg.system_id, cfg.connection_url);

    // Each object below binds to the previous one by reference, so they must be
    // built in this order: connection first, then telemetry/actions off of it,
    // then the executor off of those. None of this connects to the vehicle yet
    // (VehicleConnection's constructor only stores state); that happens later
    // when the manager starts.
    auto connection = std::make_unique<VehicleConnection>(mavsdk_, cfg.connection_url, cfg.system_id);
    auto telemetry = std::make_unique<TelemetryInfo>(*connection);
    auto actions = std::make_unique<VehicleActions>(*connection);

    auto executor = std::make_unique<CommandExecutor>(
        *actions, *telemetry,
        [this](const karshipta::v1::CommandAck& ack) { broadcast_command_ack(ack); });

    ManagedVehicle managed{
        .system_id = cfg.system_id,
        .connection = std::move(connection),
        .telemetry = std::move(telemetry),
        .actions = std::move(actions),
        .executor = std::move(executor),
    };

    managed_vehicles_.emplace(cfg.vehicle_id, std::move(managed));
    spdlog::info("vehicle '{}' registered (system_id={}, total={})", cfg.vehicle_id, cfg.system_id,
                 managed_vehicles_.size());
    return true;
}

const ManagedVehicle* VehicleManager::get_vehicle(const std::string& vehicle_id) const {
    const auto it = managed_vehicles_.find(vehicle_id);
    return it == managed_vehicles_.end() ? nullptr : &it->second;
}

void VehicleManager::dispatch_command(const karshipta::v1::Command& command) const {
    const auto* vehicle = get_vehicle(command.vehicle_id());
    if (vehicle == nullptr) {
        spdlog::warn("dispatch_command rejected: unknown vehicle_id '{}'", command.vehicle_id());
        karshipta::v1::CommandAck ack;
        ack.set_command_id(command.command_id());
        ack.set_vehicle_id(command.vehicle_id());
        ack.set_status(karshipta::v1::COMMAND_STATUS_REJECTED);
        ack.set_message("unknown vehicle_id: " + command.vehicle_id());
        broadcast_command_ack(ack);
        return;
    }
    vehicle->executor->enqueue(command);
}

void VehicleManager::broadcast_command_ack(const karshipta::v1::CommandAck& ack) const {
    karshipta::v1::Envelope envelope;
    *envelope.mutable_command_ack() = ack;
    std::vector<uint8_t> bytes(envelope.ByteSizeLong());
    if (envelope.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()))) {
        transport_.broadcast(bytes);
    }
}

bool VehicleManager::start(const std::string& vehicle_id) {
    const auto it = managed_vehicles_.find(vehicle_id);
    if (it == managed_vehicles_.end()) {
        spdlog::warn("start rejected: unknown vehicle_id '{}'", vehicle_id);
        return false;
    }
    ManagedVehicle& vehicle = it->second;

    if (vehicle.reconnect_worker.joinable()) {
        spdlog::warn("start rejected: vehicle_id '{}' already running", vehicle_id);
        return false;
    }

    vehicle.reconnect_worker = std::jthread(
        [this, &vehicle](std::stop_token stop_token) { run_reconnect_loop(vehicle, stop_token); });
    return true;
}