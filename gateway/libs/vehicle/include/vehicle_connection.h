#ifndef KARSHIPTA_GATEWAY_VEHICLE_CONNECTION_H_
#define KARSHIPTA_GATEWAY_VEHICLE_CONNECTION_H_

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <mavsdk/mavsdk.h>

// Owns the discovery/link-state of a single vehicle on a Mavsdk core that is
// SHARED across the fleet. Does not own the Mavsdk core itself; that belongs
// to whoever constructs VehicleConnection (the future VehicleManager) and is
// expected to outlive every VehicleConnection built on top of it. Does not
// touch telemetry or commands either; callers use get_system() to hand the
// discovered System to whatever plugin they need.
class VehicleConnection {

    public:
        // Outcome of a single connect() attempt, so callers (and eventually the
        // manager, which turns these into Event messages) can tell a socket-level
        // failure apart from "nothing answered in time".
        enum class ConnectResult {
            kSuccess,
            kSocketFailure,
            kDiscoveryTimeout,
        };

        VehicleConnection() = delete;
        VehicleConnection(const VehicleConnection&) = delete;
        VehicleConnection& operator=(const VehicleConnection&) = delete;
        // Deleted, not defaulted: subscribe_connection_state() callbacks may capture
        // `this`, so a moved connection would leave them pointing at a stale address.
        VehicleConnection(VehicleConnection&&) = delete;
        VehicleConnection& operator=(VehicleConnection&&) = delete;
        // Disconnects if still connected, so every VehicleConnection logs a
        // clean disconnect regardless of how it goes out of scope.
        ~VehicleConnection();

        // mavsdk: a core shared with other VehicleConnections. Not created here,
        // not destroyed here; this object only ever adds/removes its own
        // connection_url on it. Caller must keep the core alive at least as long
        // as this VehicleConnection.
        //
        // expected_system_id: the MAVLink system id (VehicleInfo.mavlink_system_id)
        // this connection must bind to. When set, connect() waits specifically for
        // that system, so multiple VehicleConnections can connect concurrently on
        // the same shared core. When absent, connect() falls back to
        // first_autopilot() (the single-vehicle M1 case, no config needed).
        VehicleConnection(std::shared_ptr<mavsdk::Mavsdk> mavsdk,
                           const std::string& drone_url,
                           std::optional<uint32_t> expected_system_id = std::nullopt);

        // Builds a Mavsdk core configured the way every VehicleConnection expects
        // (ComponentType::GroundStation). Callers building a fleet share the one
        // instance this returns across every VehicleConnection instead of each
        // constructing their own.
        static std::shared_ptr<mavsdk::Mavsdk> create_shared_core();

        // Validates and replaces the connection URL used by a subsequent connect().
        void set_drone_url(const std::string& drone_url);
        // Returns the currently configured connection URL.
        [[nodiscard]] std::string get_drone_url() const;

        // Single attempt: adds connection_url to the shared core (only once, ever;
        // later calls just re-wait for discovery) and blocks up to
        // kAutopilotDiscoveryTimeoutS waiting for a matching autopilot.
        //
        // If expected_system_id was configured, waits specifically for that system
        // id (subscribe_on_new_system + System::get_system_id()), so callers may
        // connect several VehicleConnections on one shared core concurrently.
        // Otherwise falls back to first_autopilot(), which only gives correct
        // results when vehicles are connected one at a time.
        ConnectResult connect();
        // Calls connect() repeatedly, sleeping retry_interval between attempts, until
        // it succeeds or stop_token is cancelled. Returns false only if cancelled
        // before a connection was established.
        bool connect_with_retry(const std::stop_token& stop_token,
                                 std::chrono::milliseconds retry_interval = kDefaultRetryInterval);
        // Cancels every subscription this instance handed out, drops the discovered
        // System handle, and removes connection_url from the shared core. Note: the
        // underlying Mavsdk core itself is NOT destroyed here, so any
        // DroneActions/TelemetryInfo already holding a System/Mavsdk shared_ptr can
        // keep operating, and other VehicleConnections sharing the same core are
        // unaffected. Safe to call when already disconnected (no-op).
        void disconnect();
        // True only while a System was discovered AND MAVSDK currently reports the
        // heartbeat link as up; becomes false the instant the link drops, even if
        // disconnect() was never explicitly called.
        [[nodiscard]] bool is_connected() const;
        // Returns the discovered System handle (shared_ptr; null if not connected).
        [[nodiscard]] std::shared_ptr<mavsdk::System> get_system() const;
        // Returns the Mavsdk core handle (shared_ptr; always valid once constructed).
        [[nodiscard]] std::shared_ptr<mavsdk::Mavsdk> get_mavsdk() const;

        // Registers for live is-connected transitions (link up/down) on the discovered
        // System. Only valid after a successful connect(); throws std::logic_error
        // otherwise. disconnect() cancels every handle this instance handed out, so
        // callers do not strictly need to call unsubscribe_connection_state()
        // themselves before tearing down, but may to cancel earlier.
        mavsdk::System::IsConnectedHandle subscribe_connection_state(
            const mavsdk::System::IsConnectedCallback& callback);
        // Unsubscribes a handle previously returned by subscribe_connection_state().
        // No-op if already disconnected.
        void unsubscribe_connection_state(mavsdk::System::IsConnectedHandle handle);

    private:
        // Named so a config value can override it later per BRIEF.md M4; serial
        // links will need a longer timeout than SITL over UDP.
        static constexpr double kAutopilotDiscoveryTimeoutS = 3.0;
        static constexpr std::chrono::milliseconds kDefaultRetryInterval{2000};

        // The MAVLink connection URL (e.g. "udpin://0.0.0.0:14540").
        std::string connection_url_;
        // The Mavsdk core instance, shared with other VehicleConnections. Not owned.
        std::shared_ptr<mavsdk::Mavsdk> mavsdk_;
        // The discovered autopilot System, set by connect(), cleared by disconnect().
        std::shared_ptr<mavsdk::System> system_;
        // MAVLink system id this connection must bind to; nullopt falls back to
        // first_autopilot()'s discovery-order behavior.
        std::optional<uint32_t> expected_system_id_;
        // Every IsConnectedHandle handed out by subscribe_connection_state(), so
        // disconnect() can cancel them all before dropping system_.
        std::vector<mavsdk::System::IsConnectedHandle> connection_state_handles_;

        // Set by connect() the first time add_any_connection_with_handle() succeeds;
        // guards against re-adding connection_url to the shared core on every retry.
        bool connection_added_ = false;
        mavsdk::Mavsdk::ConnectionHandle connection_handle_{};

        // Throws std::invalid_argument if drone_url is empty; otherwise returns it unchanged.
        static std::string validate_drone_url(const std::string& drone_url);
        // Returns the already-discovered system matching system_id, if any.
        [[nodiscard]] std::optional<std::shared_ptr<mavsdk::System>> find_system(
            uint32_t system_id) const;
        // Blocks up to kAutopilotDiscoveryTimeoutS for a system matching system_id to
        // appear on the shared core, via subscribe_on_new_system. Returns nullopt on
        // timeout.
        [[nodiscard]] std::optional<std::shared_ptr<mavsdk::System>> wait_for_system(
            uint32_t system_id) const;
};

#endif //KARSHIPTA_GATEWAY_VEHICLE_CONNECTION_H_
