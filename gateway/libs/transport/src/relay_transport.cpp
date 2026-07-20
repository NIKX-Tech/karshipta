#include "relay_transport.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <cassert>
#include <filesystem>

RelayTransport::RelayTransport(std::string relay_url, RelayCredentials credentials)
    : relay_url_(std::move(relay_url)), credentials_(std::move(credentials)) {}

std::unique_ptr<RelayTransport> RelayTransport::from_config(std::string relay_url,
                                                             const std::string& config_path) {
    RelayCredentials credentials;

    std::error_code exists_error;
    if (!std::filesystem::exists(config_path, exists_error) || exists_error) {
        spdlog::warn(
            "relay credentials file '{}' not found; starting with empty credentials "
            "(scaffold only, see gateway/docs/relay-transport.md)",
            config_path);
        return std::make_unique<RelayTransport>(std::move(relay_url), std::move(credentials));
    }

    try {
        const YAML::Node root = YAML::LoadFile(config_path);
        if (root["device_id"]) credentials.device_id = root["device_id"].as<std::string>();
        if (root["private_key_path"])
            credentials.private_key_path = root["private_key_path"].as<std::string>();
    } catch (const YAML::Exception& parse_error) {
        spdlog::error("failed to parse relay credentials '{}': {}", config_path,
                      parse_error.what());
        credentials = RelayCredentials{};
    }

    return std::make_unique<RelayTransport>(std::move(relay_url), std::move(credentials));
}

RelayTransport::~RelayTransport() { stop(); }

void RelayTransport::start() {
    if (running_) return;

    spdlog::warn(
        "relay transport has no pairing handshake or encryption yet (Noise XX "
        "not implemented, see gateway/docs/relay-transport.md): frames are "
        "neither authenticated nor encrypted");

    ix::initNetSystem();
    auto socket = std::make_shared<ix::WebSocket>();
    socket->setUrl(relay_url_);
    socket->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open: {
                const ClientId id = next_client_id_.fetch_add(1);
                {
                    std::lock_guard lock(peer_mutex_);
                    peer_id_ = id;
                }
                spdlog::info("relay transport connected to {} as peer {}", relay_url_, id);
                if (on_connect_) on_connect_(id);
                break;
            }
            case ix::WebSocketMessageType::Close: {
                ClientId id = 0;
                {
                    std::lock_guard lock(peer_mutex_);
                    id = peer_id_;
                    peer_id_ = 0;
                }
                if (id != 0) {
                    spdlog::info("relay transport disconnected from {}", relay_url_);
                    if (on_disconnect_) on_disconnect_(id);
                }
                break;
            }
            case ix::WebSocketMessageType::Message: {
                if (!msg->binary) {
                    spdlog::warn("ignoring non-binary frame from relay");
                    break;
                }
                ClientId id = 0;
                {
                    std::lock_guard lock(peer_mutex_);
                    id = peer_id_;
                }
                if (id != 0 && on_receive_) {
                    on_receive_(id, std::vector<uint8_t>(msg->str.begin(), msg->str.end()));
                }
                break;
            }
            default:
                break;
        }
    });

    {
        std::lock_guard lock(peer_mutex_);
        socket_ = socket;
    }
    socket->start();

    running_ = true;
    spdlog::info("relay transport connecting to {} (device_id={})", relay_url_,
                  credentials_.device_id);
}

void RelayTransport::stop() {
    if (!running_) return;

    std::shared_ptr<ix::WebSocket> socket;
    {
        std::lock_guard lock(peer_mutex_);
        socket = std::move(socket_);
        peer_id_ = 0;
    }
    if (socket) socket->stop();
    ix::uninitNetSystem();
    running_ = false;
    spdlog::info("relay transport stopped");
}

bool RelayTransport::is_running() const { return running_; }

void RelayTransport::send(const ClientId client, const std::vector<uint8_t>& bytes) {
    std::shared_ptr<ix::WebSocket> socket;
    {
        std::lock_guard lock(peer_mutex_);
        if (peer_id_ == 0 || peer_id_ != client) return;
        socket = socket_;
    }
    socket->sendBinary(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void RelayTransport::broadcast(const std::vector<uint8_t>& bytes) {
    ClientId id;
    {
        std::lock_guard lock(peer_mutex_);
        id = peer_id_;
    }
    if (id != 0) send(id, bytes);
}

Transport::ClientRole RelayTransport::role(ClientId /*client*/) const {
    return ClientRole::kOperator;
}

void RelayTransport::on_receive(ReceiveCallback callback) {
    assert(!running_ && "on_receive must be set before start()");
    on_receive_ = std::move(callback);
}

void RelayTransport::on_connect(ConnectCallback callback) {
    assert(!running_ && "on_connect must be set before start()");
    on_connect_ = std::move(callback);
}

void RelayTransport::on_disconnect(DisconnectCallback callback) {
    assert(!running_ && "on_disconnect must be set before start()");
    on_disconnect_ = std::move(callback);
}
