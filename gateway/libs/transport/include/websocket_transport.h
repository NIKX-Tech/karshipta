//
// Created by amir abkhoshk on 10/07/2026.
//

#ifndef KARSHIPTA_GATEWAY_WEBSOCKET_TRANSPORT_H
#define KARSHIPTA_GATEWAY_WEBSOCKET_TRANSPORT_H

#include "transport.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

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
    WebsocketTransport(std::string host, uint16_t port);
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

    void on_receive(ReceiveCallback callback) override;
    void on_connect(ConnectCallback callback) override;
    void on_disconnect(DisconnectCallback callback) override;

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
    // The ix::WebSocket* stays valid for the connection's lifetime (owned by
    // the server); erased on disconnect before the underlying object dies.
    std::unordered_map<ClientId, ix::WebSocket*> clients_;
    // Reverse lookup of the above, needed because IXWebSocket's message
    // callback only identifies the connection by its ix::WebSocket&.
    std::unordered_map<ix::WebSocket*, ClientId> client_ids_;
    std::atomic<ClientId> next_client_id_{1};

    ReceiveCallback on_receive_;
    ConnectCallback on_connect_;
    DisconnectCallback on_disconnect_;
};

#endif  // KARSHIPTA_GATEWAY_WEBSOCKET_TRANSPORT_H
