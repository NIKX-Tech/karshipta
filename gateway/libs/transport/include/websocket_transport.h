#ifndef KARSHIPTA_GATEWAY_WEBSOCKET_TRANSPORT_H
#define KARSHIPTA_GATEWAY_WEBSOCKET_TRANSPORT_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "transport.h"

namespace ix {
class WebSocketServer;
class WebSocket;
}  // namespace ix

// Plain WebSocket server implementation of Transport (BRIEF.md M2), built on
// IXWebSocket. Binary frames only, no TLS: this is the local/simulated-fleet
// transport, not the relay transport that replaces it in M4. Callers only see
// Transport; no ix:: type crosses that boundary.
class WebsocketTransport final : public Transport {
   public:
    // host/port to listen on, e.g. ("0.0.0.0", 8765) for ws://localhost:8765.
    // Prefer from_config() at call sites: this constructor does not apply the
    // safe-bind policy (see from_config's comment) and will happily bind
    // wherever asked, including a bare LAN-reachable address.
    WebsocketTransport(std::string host, uint16_t port);

    // Builds host/port/from a YAML config file at config_path (keys:
    // websocket.host, websocket.port, websocket.allow_lan_bind), enforcing a
    // safe-by-default bind policy: a non-loopback host (anything other than
    // 127.0.0.1/localhost/::1, including 0.0.0.0) is only honored if
    // websocket.allow_lan_bind is true, and doing so logs a loud startup
    // warning since the resulting server has no authentication (BRIEF.md
    // M4/gateway hardening issue #16). Cross-machine access is meant to go
    // through the relay transport instead (see gateway/docs/relay-transport.md),
    // not a LAN-exposed plain websocket. A missing config file, or a
    // non-loopback host with the escape hatch left off, falls back to
    // (127.0.0.1, 8765) with a logged reason.
    static std::unique_ptr<WebsocketTransport> from_config(const std::string& config_path);

    // Stops the server if still running, so no client callback can fire
    // against a destroyed WebsocketTransport.
    ~WebsocketTransport() override;

    WebsocketTransport(const WebsocketTransport&) = delete;
    WebsocketTransport& operator=(const WebsocketTransport&) = delete;
    WebsocketTransport(WebsocketTransport&&) = delete;
    WebsocketTransport& operator=(WebsocketTransport&&) = delete;

    void start() override;
    void stop() override;
    [[nodiscard]] bool is_running() const override;

    void send(ClientId client, const std::vector<uint8_t>& bytes) override;
    void broadcast(const std::vector<uint8_t>& bytes) override;

    // Not synchronized against IXWebSocket's callback threads: must be called
    // before start(), never while the server is running.
    void on_receive(ReceiveCallback callback) override;
    void on_connect(ConnectCallback callback) override;
    void on_disconnect(DisconnectCallback callback) override;

    // The host/port this instance was built with, whether via the
    // constructor or from_config(). Read-only: fixed for the instance's
    // lifetime, same as RelayTransport::credentials().
    [[nodiscard]] const std::string& host() const { return host_; }
    [[nodiscard]] uint16_t port() const { return port_; }

   private:
    std::string host_;
    uint16_t port_;
    std::unique_ptr<ix::WebSocketServer> server_;
    std::atomic<bool> running_{false};

    // Guards clients_/client_ids_ against concurrent access: IXWebSocket invokes
    // the per-connection message callback on its own thread pool, one thread per
    // connection, so connect/disconnect/send can all race with each other.
    mutable std::mutex clients_mutex_;
    // Live connections keyed by the ClientId this transport assigned them.
    // Holds a shared_ptr matching the one the server keeps in its own client
    // set: send()/broadcast() copy this out under the lock, so a concurrent
    // disconnect erasing the server's copy cannot free the socket out from
    // under an in-flight sendBinary().
    std::unordered_map<ClientId, std::shared_ptr<ix::WebSocket>> clients_;
    // Reverse lookup of the above, needed because IXWebSocket's message
    // callback only identifies the connection by its ix::WebSocket&. Keyed
    // by raw pointer purely for identity; ownership lives in clients_.
    std::unordered_map<ix::WebSocket*, ClientId> client_ids_;
    std::atomic<ClientId> next_client_id_{1};

    ReceiveCallback on_receive_;
    ConnectCallback on_connect_;
    DisconnectCallback on_disconnect_;
};

#endif  // KARSHIPTA_GATEWAY_WEBSOCKET_TRANSPORT_H
