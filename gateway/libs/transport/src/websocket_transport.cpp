#include "websocket_transport.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <cassert>
#include <filesystem>

namespace {

constexpr auto kDefaultHost = "127.0.0.1";
constexpr uint16_t kDefaultPort = 8765;

// Deliberately exact-match only (not a CIDR/subnet check): the values a
// config file is expected to spell out for "this machine only" are exactly
// these three. Anything else, including 0.0.0.0, is treated as a wider bind
// subject to the allow_lan_bind escape hatch.
bool is_loopback_host(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

// Reads the role=viewer query parameter off a connection URI (e.g.
// "/?role=viewer"), gateway issue #20. Deliberately minimal: exact key/value
// match only, no percent-decoding or multi-value handling, since this is a
// same-origin operator/console setting, not a public-facing form. Anything
// other than an exact "role=viewer" pair, including no query string at all,
// keeps the connection an operator: existing consoles that predate viewer
// mode carry no query parameter and must not lose command access.
Transport::ClientRole parse_role_from_uri(const std::string& uri) {
    const auto query_pos = uri.find('?');
    if (query_pos == std::string::npos) return Transport::ClientRole::kOperator;

    const std::string query = uri.substr(query_pos + 1);
    std::size_t start = 0;
    while (start <= query.size()) {
        const auto amp_pos = query.find('&', start);
        const std::string pair = query.substr(start, amp_pos == std::string::npos
                                                           ? std::string::npos
                                                           : amp_pos - start);
        if (pair == "role=viewer") return Transport::ClientRole::kViewer;
        if (amp_pos == std::string::npos) break;
        start = amp_pos + 1;
    }
    return Transport::ClientRole::kOperator;
}

}  // namespace

WebsocketTransport::WebsocketTransport(std::string host, const uint16_t port)
    : host_(std::move(host)), port_(port) {}

std::unique_ptr<WebsocketTransport> WebsocketTransport::from_config(
    const std::string& config_path) {
    std::string host = kDefaultHost;
    uint16_t port = kDefaultPort;
    bool allow_lan_bind = false;

    std::error_code exists_error;
    if (!std::filesystem::exists(config_path, exists_error) || exists_error) {
        spdlog::info(
            "gateway config '{}' not found; binding to the safe default {}:{}", config_path,
            kDefaultHost, kDefaultPort);
    } else {
        try {
            const YAML::Node root = YAML::LoadFile(config_path);
            if (const YAML::Node websocket = root["websocket"]; websocket) {
                if (websocket["host"]) host = websocket["host"].as<std::string>();
                if (websocket["port"]) port = websocket["port"].as<uint16_t>();
                if (websocket["allow_lan_bind"])
                    allow_lan_bind = websocket["allow_lan_bind"].as<bool>();
            }
        } catch (const YAML::Exception& parse_error) {
            spdlog::error(
                "failed to parse gateway config '{}': {}; binding to the safe default {}:{}",
                config_path, parse_error.what(), kDefaultHost, kDefaultPort);
            host = kDefaultHost;
            port = kDefaultPort;
            allow_lan_bind = false;
        }
    }

    if (!is_loopback_host(host) && !allow_lan_bind) {
        spdlog::warn(
            "gateway config '{}' requests websocket.host='{}', but websocket.allow_lan_bind is "
            "off; forcing 127.0.0.1 instead. Cross-machine access is meant to go through the "
            "relay transport, not a LAN-exposed plain websocket (see "
            "gateway/docs/relay-transport.md); set websocket.allow_lan_bind: true if you really "
            "need a bare LAN bind.",
            config_path, host);
        host = kDefaultHost;
    } else if (!is_loopback_host(host) && allow_lan_bind) {
        spdlog::warn(
            "SECURITY: gateway is binding to {}:{}, reachable from other machines on this "
            "network with NO AUTHENTICATION. Anyone who can reach this address can command "
            "every connected vehicle. Only do this on a trusted, isolated network; prefer the "
            "relay transport for cross-machine access.",
            host, port);
    }

    return std::make_unique<WebsocketTransport>(std::move(host), port);
}

WebsocketTransport::~WebsocketTransport() { stop(); }

void WebsocketTransport::start() {
    if (running_) return;

    ix::initNetSystem();
    server_ = std::make_unique<ix::WebSocketServer>(port_, host_);
    server_->setOnClientMessageCallback(
        [this](const std::shared_ptr<ix::ConnectionState>& connection_state,
               ix::WebSocket& web_socket, const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open: {
                    // The server owns each connection as a shared_ptr in its own
                    // client set; look up the matching one so our map shares
                    // ownership instead of holding a raw pointer that the server
                    // can free out from under a concurrent send().
                    std::shared_ptr<ix::WebSocket> shared_socket;
                    for (const auto& candidate : server_->getClients()) {
                        if (candidate.get() == &web_socket) {
                            shared_socket = candidate;
                            break;
                        }
                    }
                    if (!shared_socket) {
                        spdlog::warn(
                            "client connected but not found in server client set, dropping");
                        break;
                    }
                    const ClientId id = next_client_id_.fetch_add(1);
                    const ClientRole role = parse_role_from_uri(msg->openInfo.uri);
                    {
                        std::lock_guard lock(clients_mutex_);
                        clients_[id] = shared_socket;
                        client_ids_[&web_socket] = id;
                        client_roles_[id] = role;
                    }
                    spdlog::info("client {} connected from {} as {}", id,
                                 connection_state->getRemoteIp(),
                                 role == ClientRole::kViewer ? "viewer" : "operator");
                    if (on_connect_) on_connect_(id);
                    break;
                }
                case ix::WebSocketMessageType::Close: {
                    ClientId id = 0;
                    bool found = false;
                    {
                        std::lock_guard lock(clients_mutex_);
                        if (const auto it = client_ids_.find(&web_socket);
                            it != client_ids_.end()) {
                            id = it->second;
                            found = true;
                            clients_.erase(id);
                            client_ids_.erase(it);
                            client_roles_.erase(id);
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
                        if (const auto it = client_ids_.find(&web_socket);
                            it != client_ids_.end()) {
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
        client_roles_.clear();
    }
    ix::uninitNetSystem();
    running_ = false;
    spdlog::info("websocket server stopped");
}

bool WebsocketTransport::is_running() const { return running_; }

void WebsocketTransport::send(const ClientId client, const std::vector<uint8_t>& bytes) {
    std::shared_ptr<ix::WebSocket> web_socket;
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
    std::vector<std::shared_ptr<ix::WebSocket>> targets;
    {
        std::lock_guard lock(clients_mutex_);
        targets.reserve(clients_.size());
        for (const auto& [id, web_socket] : clients_) {
            targets.push_back(web_socket);
        }
    }
    const std::string payload(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    for (const auto& web_socket : targets) {
        web_socket->sendBinary(payload);
    }
}

Transport::ClientRole WebsocketTransport::role(const ClientId client) const {
    std::lock_guard lock(clients_mutex_);
    const auto it = client_roles_.find(client);
    return it == client_roles_.end() ? ClientRole::kViewer : it->second;
}

void WebsocketTransport::on_receive(ReceiveCallback callback) {
    assert(!running_ && "on_receive must be set before start()");
    on_receive_ = std::move(callback);
}

void WebsocketTransport::on_connect(ConnectCallback callback) {
    assert(!running_ && "on_connect must be set before start()");
    on_connect_ = std::move(callback);
}

void WebsocketTransport::on_disconnect(DisconnectCallback callback) {
    assert(!running_ && "on_disconnect must be set before start()");
    on_disconnect_ = std::move(callback);
}
