#ifndef KARSHIPTA_GATEWAY_WARD_CONNECTION_H_
#define KARSHIPTA_GATEWAY_WARD_CONNECTION_H_

#include <mavsdk/mavsdk.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

// Owns the discovery/link-state of a single ward on a Mavsdk core that is
// SHARED across the fleet. Does not own the Mavsdk core itself; that belongs
// to whoever constructs WardConnection (the future WardManager) and is
// expected to outlive every WardConnection built on top of it. Does not
// touch telemetry or commands either; callers use get_system() to hand the
// discovered System to whatever plugin they need.
class WardConnection {
   public:
    // Outcome of a single connect() attempt, so callers (and eventually the
    // manager, which turns these into Event messages) can tell a socket-level
    // failure apart from "nothing answered in time".
    enum class ConnectResult {
        kSuccess,
        kSocketFailure,
        kDiscoveryTimeout,
    };

    // Single-snapshot view of the link, safe to branch on. Distinguishes
    // "was never discovered" from "was discovered but the heartbeat link is
    // currently down": is_connected() alone conflates those two, and
    // safety guards (stop/remove) must treat link-down as state-unknown,
    // not as landed-and-disarmed.
    enum class LinkState {
        kNeverDiscovered,
        kLinkDown,
        kConnected,
    };

    WardConnection() = delete;
    WardConnection(const WardConnection&) = delete;
    WardConnection& operator=(const WardConnection&) = delete;
    // Deleted, not defaulted: subscribe_connection_state() callbacks may capture
    // `this`, so a moved connection would leave them pointing at a stale address.
    WardConnection(WardConnection&&) = delete;
    WardConnection& operator=(WardConnection&&) = delete;
    // Disconnects if still connected, so every WardConnection logs a
    // clean disconnect regardless of how it goes out of scope.
    ~WardConnection();

    // mavsdk: a core shared with other WardConnections. Not created here,
    // not destroyed here; this object only ever adds/removes its own
    // connection_url on it. Caller must keep the core alive at least as long
    // as this WardConnection.
    //
    // expected_system_id: the MAVLink system id (WardInfo.mavlink_system_id)
    // this connection must bind to. When set, connect() waits specifically for
    // that system, so multiple WardConnections can connect concurrently on
    // the same shared core. When absent, connect() falls back to
    // first_autopilot() (the single-ward M1 case, no config needed).
    WardConnection(std::shared_ptr<mavsdk::Mavsdk> mavsdk, const std::string& connection_url,
                      std::optional<uint32_t> expected_system_id = std::nullopt);

    // Builds a Mavsdk core configured the way every WardConnection expects
    // (ComponentType::GroundStation). Callers building a fleet share the one
    // instance this returns across every WardConnection instead of each
    // constructing their own.
    static std::shared_ptr<mavsdk::Mavsdk> create_shared_core();

    // Validates and replaces the connection URL used by a subsequent connect().
    void set_connection_url(const std::string& connection_url);
    // Returns the currently configured connection URL.
    [[nodiscard]] std::string get_connection_url() const;

    // Single attempt: adds connection_url to the shared core (only once, ever;
    // later calls just re-wait for discovery) and blocks up to
    // kAutopilotDiscoveryTimeoutS waiting for a matching autopilot.
    //
    // If expected_system_id was configured, waits specifically for that system
    // id (subscribe_on_new_system + System::get_system_id()), so callers may
    // connect several WardConnections on one shared core concurrently.
    // Otherwise falls back to first_autopilot(), which only gives correct
    // results when wards are connected one at a time.
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
    // keep operating, and other WardConnections sharing the same core are
    // unaffected. Safe to call when already disconnected (no-op).
    void disconnect();
    // True only while a System was discovered AND MAVSDK currently reports the
    // heartbeat link as up; becomes false the instant the link drops, even if
    // disconnect() was never explicitly called. Thread-safe.
    [[nodiscard]] bool is_connected() const;
    // One atomic snapshot of discovery + link state (see LinkState). Prefer
    // this over get_system()/is_connected() pairs, which race against the
    // reconnect thread between the two reads. Thread-safe.
    [[nodiscard]] LinkState link_state() const;
    // Returns the discovered System handle (shared_ptr; null if not connected).
    // Thread-safe.
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
    // The Mavsdk core instance, shared with other WardConnections. Not owned.
    std::shared_ptr<mavsdk::Mavsdk> mavsdk_;
    // Guards system_, connection_state_handles_, connection_added_, and
    // connection_handle_: connect() runs on the manager's reconnect worker
    // thread while is_connected()/get_system()/link_state() are read from
    // manager and executor threads, and disconnect() can run concurrently
    // with a reconnect attempt (e.g. an operator-initiated remove while the
    // link is still bouncing). An unsynchronized shared_ptr assign vs copy,
    // or a racing read/write of the connection_added_ flag, is a data race
    // either way.
    mutable std::mutex system_mutex_;
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

    // Throws std::invalid_argument if connection_url is empty; otherwise returns it unchanged.
    static std::string validate_connection_url(const std::string& connection_url);
    // Returns the already-discovered system matching system_id, if any.
    [[nodiscard]] std::optional<std::shared_ptr<mavsdk::System>> find_system(
        uint32_t system_id) const;
    // Blocks up to kAutopilotDiscoveryTimeoutS for a system matching system_id to
    // appear on the shared core, via subscribe_on_new_system. Returns nullopt on
    // timeout.
    [[nodiscard]] std::optional<std::shared_ptr<mavsdk::System>> wait_for_system(
        uint32_t system_id) const;
};

#endif  // KARSHIPTA_GATEWAY_WARD_CONNECTION_H_
