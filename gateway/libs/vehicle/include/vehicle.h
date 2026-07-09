#ifndef KARSHIPTA_GATEWAY_VEHICLE_CONNECTION_H_
#define KARSHIPTA_GATEWAY_VEHICLE_CONNECTION_H_

#include <chrono>
#include <memory>
#include <stop_token>
#include <string>

#include <mavsdk/mavsdk.h>

// Owns the discovery/link-state of a single vehicle on a Mavsdk core that is
// SHARED across the fleet. Does not own the Mavsdk core itself — that belongs
// to whoever constructs VehicleConnection (the future VehicleManager) and is
// expected to outlive every VehicleConnection built on top of it. Does not
// touch telemetry or commands either; callers use getSystem() to hand the
// discovered System to whatever plugin they need.
class VehicleConnection {

    public:
        VehicleConnection()=delete;
        VehicleConnection(const VehicleConnection&) = delete;
        VehicleConnection& operator=(const VehicleConnection&) = delete;
        VehicleConnection(VehicleConnection&&) = default;
        VehicleConnection& operator=(VehicleConnection&&) = default;
        // Disconnects if still connected, so every VehicleConnection logs a
        // clean disconnect regardless of how it goes out of scope.
        ~VehicleConnection();

        // mavsdk_instance: a core shared with other VehicleConnections. Not created
        // here, not destroyed here — this object only ever adds/removes its own
        // connection_url on it. Caller must keep the core alive at least as long
        // as this VehicleConnection.
        VehicleConnection(std::shared_ptr<mavsdk::Mavsdk> mavsdk_instance,
                           const std::string& drone_url);

        // Builds a Mavsdk core configured the way every VehicleConnection expects
        // (ComponentType::GroundStation). Callers building a fleet share the one
        // instance this returns across every VehicleConnection instead of each
        // constructing their own.
        static std::shared_ptr<mavsdk::Mavsdk> createSharedCore();

        // Validates and replaces the connection URL used by a subsequent connect().
        void setDroneUrl(const std::string& drone_url);
        // Returns the currently configured connection URL.
        [[nodiscard]] std::string getDroneUrl() const;

        // Single attempt: adds connection_url to the shared core (only once, ever —
        // later calls just re-wait for discovery) and blocks up to
        // kAutopilotDiscoveryTimeoutS waiting for an autopilot heartbeat
        // (first_autopilot()). Returns false on socket failure or if no autopilot is
        // discovered in time.
        //
        // NOTE: first_autopilot() returns whichever autopilot the shared core has
        // seen, not specifically the one from connection_url — MAVSDK does not
        // correlate a ConnectionHandle to the System it produces. This is only
        // correct if the caller (VehicleManager) connects vehicles one at a time,
        // never concurrently, on a given shared core.
        bool connect();
        // Calls connect() repeatedly, sleeping retry_interval between attempts, until
        // it succeeds or stop_token is cancelled. Returns false only if cancelled
        // before a connection was established.
        bool connectWithRetry(const std::stop_token& stop_token,
                               std::chrono::milliseconds retry_interval = kDefaultRetryInterval);
        // Drops the discovered System handle and removes connection_url from the
        // shared core. Note: the underlying Mavsdk core itself is NOT destroyed
        // here, so any DroneActions/TelemetryInfo already holding a System/Mavsdk
        // shared_ptr can keep operating, and other VehicleConnections sharing the
        // same core are unaffected. Safe to call when already disconnected (no-op).
        void disconnect();
        // True only while a System was discovered AND MAVSDK currently reports the
        // heartbeat link as up; becomes false the instant the link drops, even if
        // disconnect() was never explicitly called.
        [[nodiscard]] bool isConnected() const;
        // Returns the discovered System handle (shared_ptr; null if not connected).
        [[nodiscard]] std::shared_ptr<mavsdk::System> getSystem() const;
        // Returns the Mavsdk core handle (shared_ptr; always valid once constructed).
        [[nodiscard]] std::shared_ptr<mavsdk::Mavsdk> getMavsdk() const;

        // Registers for live is-connected transitions (link up/down) on the discovered
        // System. Only valid after a successful connect(); throws std::logic_error
        // otherwise. Caller owns the returned handle and must unsubscribeConnectionState()
        // it before the System goes away (disconnect() invalidates any subscription).
        mavsdk::System::IsConnectedHandle subscribeConnectionState(
            const mavsdk::System::IsConnectedCallback& callback);
        // Unsubscribes a handle previously returned by subscribeConnectionState().
        // No-op if already disconnected.
        void unsubscribeConnectionState(mavsdk::System::IsConnectedHandle handle);

    private:
        static constexpr double kAutopilotDiscoveryTimeoutS = 3.0;
        static constexpr std::chrono::milliseconds kDefaultRetryInterval{2000};

        // The MAVLink connection URL (e.g. "udpin://0.0.0.0:14540").
        std::string connection_url;
        // The Mavsdk core instance, shared with other VehicleConnections. Not owned.
        std::shared_ptr<mavsdk::Mavsdk> mavsdk_instance;
        // The discovered autopilot System, set by connect(), cleared by disconnect().
        std::shared_ptr<mavsdk::System> system;

        // Set by connect() the first time add_any_connection_with_handle() succeeds;
        // guards against re-adding connection_url to the shared core on every retry.
        bool connection_added = false;
        mavsdk::Mavsdk::ConnectionHandle connection_handle{};

        // Throws std::invalid_argument if drone_url is empty; otherwise returns it unchanged.
        static std::string validateDroneUrl(const std::string& drone_url);
};

#endif //KARSHIPTA_GATEWAY_VEHICLE_CONNECTION_H_
