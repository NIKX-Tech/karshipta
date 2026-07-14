//
// Created by amir abkhoshk on 13/07/2026.
//

#ifndef KARSHIPTA_GATEWAY_VEHICLE_MANAGER_H
#define KARSHIPTA_GATEWAY_VEHICLE_MANAGER_H
#include <string>
#include <memory>
#include <map>
#include <thread>
#include <mavsdk/mavsdk.h>
#include <karshipta/v1/fleet.pb.h>
#include <vehicle_connection.h>
#include <vehicle_actions.h>
#include <telemetry.h>
#include <command_executor.h>
#include <transport.h>

struct VehicleConfig {
    std::string vehicle_id;
    std::string connection_url;
    unsigned int system_id;
};

struct ManagedVehicle {
    unsigned int system_id;
    std::unique_ptr<VehicleConnection> connection;
    std::unique_ptr<TelemetryInfo> telemetry;
    std::unique_ptr<VehicleActions> actions;
    std::unique_ptr<CommandExecutor> executor;
    // Last: its loop calls connection->connect_with_retry()/is_connected() on
    // every iteration, so it must stop and join before connection (and the
    // other members above it) are torn down. Empty (not joinable) until
    // VehicleManager::start() launches it.
    std::jthread reconnect_worker;
};

class VehicleManager {
public:
    VehicleManager(std::shared_ptr<mavsdk::Mavsdk> mavsdk, Transport& tp);
    // No custom cleanup: managed_vehicles_ destroys each ManagedVehicle's
    // unique_ptr members in declaration order (executor, then actions,
    // telemetry, connection), which already stops/joins the executor's worker
    // before the actions_/telemetry_ it references are torn down.
    ~VehicleManager() = default;

    // managed_vehicles_ holds non-copyable ManagedVehicles (unique_ptr
    // members), so copy was already unusable; declared explicitly rather than
    // left implicit so it isn't a surprise at some future call site. Move is
    // deleted too: nothing today needs to relocate a VehicleManager, and
    // transport_ being a reference means move-assignment could never rebind it
    // anyway.
    VehicleManager(const VehicleManager&) = delete;
    VehicleManager& operator=(const VehicleManager&) = delete;
    VehicleManager(VehicleManager&&) = delete;
    VehicleManager& operator=(VehicleManager&&) = delete;

    // Builds the VehicleConnection/TelemetryInfo/VehicleActions/CommandExecutor
    // graph for cfg and registers it under cfg.vehicle_id. Does not connect to
    // the vehicle; construction only. Returns false, adding nothing, if
    // vehicle_id is already registered or system_id is already bound to a
    // different vehicle_id.
    bool add_vehicle(const VehicleConfig& cfg);

    // Returns the registered vehicle with id vehicle_id, or nullptr if none
    // exists. Const so callers can inspect but not reach into the object graph
    // and mutate it out from under the manager.
    [[nodiscard]] const ManagedVehicle* get_vehicle(const std::string& vehicle_id) const;

    // Routes command to the CommandExecutor of the vehicle named
    // command.vehicle_id(). If no such vehicle is registered, synthesizes a
    // REJECTED CommandAck itself (gateway rule 5: no silent drop) and
    // broadcasts it, since there is no executor to ask.
    void dispatch_command(const karshipta::v1::Command& command) const;

    // Launches reconnect_worker for the vehicle with this id. Returns false if
    // vehicle_id is unknown, or if that vehicle's worker is already running
    // (idempotent, not a silent restart).
    bool start(const std::string& vehicle_id);

    // Calls start() for every currently registered vehicle.
    void start_all();

    // Requests reconnect_worker for vehicle_id to stop and joins it. The
    // vehicle stays registered (still in managed_vehicles_), just no longer
    // trying to connect; distinct from remove_vehicle(). Returns false if
    // vehicle_id is unknown. Not yet implemented.
    bool stop(const std::string& vehicle_id);

    // Calls stop() for every currently registered vehicle. Not yet
    // implemented.
    void stop_all();

    // Stops the vehicle first if running, then erases it from
    // managed_vehicles_. Must reject (return false) if the vehicle is armed
    // or airborne, per fleet.proto's comment on RemoveVehicle. Not yet
    // implemented.
    bool remove_vehicle(const std::string& vehicle_id);

    // Wire-level entry points: translate an AddVehicle/RemoveVehicle request
    // from a console client into the calls above and produce the
    // VehicleConfigAck to send back. Not yet implemented.
    karshipta::v1::VehicleConfigAck handle_add_vehicle(const karshipta::v1::AddVehicle& request);
    karshipta::v1::VehicleConfigAck handle_remove_vehicle(const karshipta::v1::RemoveVehicle& request);

private:
    std::shared_ptr<mavsdk::Mavsdk> mavsdk_;
    Transport& transport_;
    std::map<std::string, ManagedVehicle> managed_vehicles_;

    // Wraps ack in an Envelope and broadcasts it. Shared by add_vehicle()'s
    // per-vehicle CommandExecutor ack callback and dispatch_command()'s
    // unknown-vehicle rejection, so both acks reach the wire the same way.
    void broadcast_command_ack(const karshipta::v1::CommandAck& ack) const;

    // Body of vehicle.reconnect_worker, run on start()'s jthread. Connects,
    // waits while connected, and on drop connects again, until stop_token is
    // cancelled.
    void run_reconnect_loop(ManagedVehicle& vehicle, std::stop_token stop_token);
};





#endif  // KARSHIPTA_GATEWAY_VEHICLE_MANAGER_H
