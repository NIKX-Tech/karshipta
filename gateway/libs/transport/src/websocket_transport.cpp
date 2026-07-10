//
// Created by amir abkhoshk on 10/07/2026.
//

#include "websocket_transport.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <spdlog/spdlog.h>

WebsocketTransport::WebsocketTransport(std::string host, const uint16_t port)
    : host_(std::move(host)), port_(port) {}

WebsocketTransport::~WebsocketTransport() {
    stop();
}

void WebsocketTransport::start() {
    if (running_) return;

    ix::initNetSystem();
    server_ = std::make_unique<ix::WebSocketServer>(port_, host_);
    server_->setOnClientMessageCallback(
        [this](const std::shared_ptr<ix::ConnectionState>& connection_state, ix::WebSocket& web_socket,
               const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open: {
                    const ClientId id = next_client_id_.fetch_add(1);
                    {
                        std::lock_guard lock(clients_mutex_);
                        clients_[id] = &web_socket;
                        client_ids_[&web_socket] = id;
                    }
                    spdlog::info("client {} connected from {}", id, connection_state->getRemoteIp());
                    if (on_connect_) on_connect_(id);
                    break;
                }
                case ix::WebSocketMessageType::Close: {
                    ClientId id = 0;
                    bool found = false;
                    {
                        std::lock_guard lock(clients_mutex_);
                        if (const auto it = client_ids_.find(&web_socket); it != client_ids_.end()) {
                            id = it->second;
                            found = true;
                            clients_.erase(id);
                            client_ids_.erase(it);
                        }
                    }
                    if (found) {
                        spdlog::info("client {} disconnected", id);
                        if (on_disconnect_) on_disconnect_(id);
                    }
                    break;
                }
                case ix::WebSocketMessageType::Message: {
                    if (!msg->binary) {
                        spdlog::warn("ignoring non-binary frame from a client");
                        break;
                    }
                    ClientId id = 0;
                    bool found = false;
                    {
                        std::lock_guard lock(clients_mutex_);
                        if (const auto it = client_ids_.find(&web_socket); it != client_ids_.end()) {
                            id = it->second;
                            found = true;
                        }
                    }
                    if (found && on_receive_) {
                        on_receive_(id, std::vector<uint8_t>(msg->str.begin(), msg->str.end()));
                    }
                    break;
                }
                default:
                    break;
            }
        });

    if (!server_->listenAndStart()) {
        spdlog::error("websocket server failed to start on {}:{}", host_, port_);
        server_.reset();
        ix::uninitNetSystem();
        return;
    }

    running_ = true;
    spdlog::info("websocket server listening on ws://{}:{}", host_, port_);
}

void WebsocketTransport::stop() {
    if (!running_) return;

    server_->stop();
    server_.reset();
    {
        std::lock_guard lock(clients_mutex_);
        clients_.clear();
        client_ids_.clear();
    }
    ix::uninitNetSystem();
    running_ = false;
    spdlog::info("websocket server stopped");
}

bool WebsocketTransport::is_running() const {
    return running_;
}

void WebsocketTransport::send(const ClientId client, const std::vector<uint8_t>& bytes) {
    ix::WebSocket* web_socket = nullptr;
    {
        std::lock_guard lock(clients_mutex_);
        if (const auto it = clients_.find(client); it != clients_.end()) {
            web_socket = it->second;
        }
    }
    if (!web_socket) return;
    web_socket->sendBinary(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void WebsocketTransport::broadcast(const std::vector<uint8_t>& bytes) {
    std::vector<ix::WebSocket*> targets;
    {
        std::lock_guard lock(clients_mutex_);
        targets.reserve(clients_.size());
        for (const auto& [id, web_socket] : clients_) {
            targets.push_back(web_socket);
        }
    }
    const std::string payload(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    for (auto* web_socket : targets) {
        web_socket->sendBinary(payload);
    }
}

void WebsocketTransport::on_receive(ReceiveCallback callback) {
    on_receive_ = std::move(callback);
}

void WebsocketTransport::on_connect(ConnectCallback callback) {
    on_connect_ = std::move(callback);
}

void WebsocketTransport::on_disconnect(DisconnectCallback callback) {
    on_disconnect_ = std::move(callback);
}
