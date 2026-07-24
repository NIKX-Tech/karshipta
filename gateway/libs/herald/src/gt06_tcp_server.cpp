#include "gt06_tcp_server.h"

#include <herald/v0/herald.pb.h>
#include <ixwebsocket/IXNetSystem.h>
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <array>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "gt06_parser.h"

namespace {

// Gt06-prefixed rather than the bare kDefaultHost/kDefaultPort names
// herald_http_server.cpp uses for the same concept: with the identical
// bare names, this file's local Clang/Xcode toolchain reported them as
// unused (-Werror=unused-const-variable) despite clear uses below - a
// same-static-library, same-anonymous-namespace-name toolchain quirk, not
// a real problem with either file. Distinct names sidestep it and are
// clearer to grep for anyway.
constexpr auto kGt06DefaultHost = "127.0.0.1";
// GT06's conventional port for tracker firmware that doesn't have a
// server address configured by hand.
constexpr uint16_t kGt06DefaultPort = 5023;
// How long a connection's read loop waits for data before re-checking
// stopping_/isTerminated() - bounds how long stop() can take to actually
// quiesce every in-flight connection thread.
constexpr int kPollTimeoutMs = 1000;
// One recv() call's buffer size; frames are reassembled across calls in
// the connection's own accumulation buffer, this just bounds one syscall.
constexpr size_t kRecvChunkSize = 1024;

// Deliberately exact-match only, same as websocket_transport.cpp's and
// herald_http_server.cpp's own is_loopback_host; duplicated rather than
// shared across libraries for one tiny predicate, matching this repo's
// existing serialize_envelope precedent.
bool is_loopback_host(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

herald::v0::Herald build_herald_message(const std::string& imei, const Gt06Parser::Location& location) {
    herald::v0::Herald msg;
    msg.set_entity_id(imei);
    msg.set_timestamp_ms(location.timestamp_ms);
    // GT06 has no device-purpose field (only a device-type/model variant),
    // so there is no way to tell a livestock collar from a generic asset
    // tracker at this protocol layer - GENERIC_TRACKER is the honest
    // default, not a guess at a more specific class.
    msg.set_entity_class(herald::v0::ENTITY_CLASS_GENERIC_TRACKER);
    auto* position = msg.mutable_position();
    position->set_latitude_deg(location.latitude_deg);
    position->set_longitude_deg(location.longitude_deg);
    auto* gps = msg.mutable_gps();
    gps->set_num_satellites(location.satellites);
    // health_ok as a proxy for fix quality: GT06's base location packet
    // carries no other device-health signal (no battery/error field), and
    // an invalid fix is exactly the kind of "weak signal" issue
    // herald.proto's own health_ok doc comment describes.
    msg.set_health_ok(location.gps_valid);
    msg.set_source("gt06");
    return msg;
}

}  // namespace

Gt06TcpServer::Gt06TcpServer(HeraldWardManager& ward_manager, std::string host, const uint16_t port)
    : ix::SocketServer(port, host), ward_manager_(ward_manager) {
    ix::initNetSystem();
}

std::unique_ptr<Gt06TcpServer> Gt06TcpServer::from_config(
    const std::string& config_path, HeraldWardManager& ward_manager,
    const std::function<bool()>& is_in_container) {
    std::string host = kGt06DefaultHost;
    uint16_t port = kGt06DefaultPort;
    bool allow_lan_bind = false;
    bool container_bind = false;

    std::error_code exists_error;
    if (!std::filesystem::exists(config_path, exists_error) || exists_error) {
        spdlog::info(
            "gateway config '{}' not found; GT06 TCP listener binding to the safe default {}:{}",
            config_path, kGt06DefaultHost, kGt06DefaultPort);
    } else {
        try {
            const YAML::Node root = YAML::LoadFile(config_path);
            if (const YAML::Node herald = root["herald"]; herald) {
                if (herald["gt06_host"]) host = herald["gt06_host"].as<std::string>();
                if (herald["gt06_port"]) port = herald["gt06_port"].as<uint16_t>();
                if (herald["allow_lan_bind"]) allow_lan_bind = herald["allow_lan_bind"].as<bool>();
                if (herald["container_bind"]) container_bind = herald["container_bind"].as<bool>();
            }
        } catch (const YAML::Exception& parse_error) {
            spdlog::error(
                "failed to parse gateway config '{}': {}; GT06 TCP listener binding to the safe "
                "default {}:{}",
                config_path, parse_error.what(), kGt06DefaultHost, kGt06DefaultPort);
            host = kGt06DefaultHost;
            port = kGt06DefaultPort;
            allow_lan_bind = false;
            container_bind = false;
        }
    }

    if (is_loopback_host(host)) {
        // Nothing to gate: fall through and bind as requested.
    } else if (allow_lan_bind) {
        spdlog::warn(
            "SECURITY: GT06 TCP listener is binding to {}:{}, reachable from other machines on "
            "this network with NO AUTHENTICATION. Anyone who can reach this address can inject "
            "ward telemetry.",
            host, port);
    } else if (container_bind && is_in_container()) {
        spdlog::warn(
            "GT06 TCP listener is binding to {}:{} because herald.container_bind is set and this "
            "process detected it is running inside a container. This is reachable only through "
            "whatever ports the container runtime publishes to the host, not directly from the "
            "LAN; see gateway/docs/websocket-transport.md for the same reasoning applied to the "
            "websocket transport.",
            host, port);
    } else if (container_bind) {
        spdlog::warn(
            "gateway config '{}' sets herald.container_bind: true, but this process is not "
            "running inside a container; ignoring and forcing 127.0.0.1 instead.",
            config_path);
        host = kGt06DefaultHost;
    } else {
        spdlog::warn(
            "gateway config '{}' requests herald.gt06_host='{}', but herald.allow_lan_bind is "
            "off; forcing 127.0.0.1 instead. Set herald.allow_lan_bind: true if trackers really "
            "need to reach this gateway over the LAN.",
            config_path, host);
        host = kGt06DefaultHost;
    }

    return std::make_unique<Gt06TcpServer>(ward_manager, std::move(host), port);
}

Gt06TcpServer::~Gt06TcpServer() {
    stop();
    ix::uninitNetSystem();
}

void Gt06TcpServer::start() {
    const auto [bound, error_message] = listen();
    if (!bound) {
        spdlog::error("GT06 TCP listener failed to bind {}:{} ({})", getHost(), getPort(),
                      error_message);
        return;
    }
    spdlog::info("GT06 TCP listener listening on {}:{}", getHost(), getPort());
    ix::SocketServer::start();
}

void Gt06TcpServer::stop() {
    stopping_ = true;
    ix::SocketServer::stop();
}

size_t Gt06TcpServer::getConnectedClientsCount() { return connected_clients_.load(); }

void Gt06TcpServer::handleConnection(std::unique_ptr<ix::Socket> socket,
                                      std::shared_ptr<ix::ConnectionState> connection_state) {
    connected_clients_.fetch_add(1);
    const std::string remote_ip = connection_state->getRemoteIp();
    spdlog::info("GT06 device connected from {}", remote_ip);

    // GT06 sends login (IMEI) once per connection, then implies it for
    // every subsequent packet - remembered locally for this connection's
    // lifetime, not shared with any other connection.
    std::optional<std::string> imei;
    std::vector<uint8_t> buffer;
    std::array<uint8_t, kRecvChunkSize> recv_chunk{};

    while (!stopping_ && !connection_state->isTerminated()) {
        const ix::PollResultType poll_result = socket->isReadyToRead(kPollTimeoutMs);
        if (poll_result == ix::PollResultType::Timeout) continue;
        if (poll_result == ix::PollResultType::Error ||
            poll_result == ix::PollResultType::CloseRequest) {
            break;
        }

        const ssize_t received = socket->recv(recv_chunk.data(), recv_chunk.size());
        if (received <= 0) break;  // 0 = orderly close, <0 = error
        buffer.insert(buffer.end(), recv_chunk.begin(), recv_chunk.begin() + received);

        bool desync = false;
        while (true) {
            const auto result = Gt06Parser::parse_frame(buffer);
            if (result.result == Gt06Parser::FrameResult::kIncomplete) break;
            if (result.result == Gt06Parser::FrameResult::kInvalid) {
                spdlog::warn("GT06 device {} sent an unparseable frame, disconnecting", remote_ip);
                desync = true;
                break;
            }

            const auto& packet = result.packet;
            switch (packet.type) {
                case Gt06Parser::PacketType::kLogin:
                    if (packet.imei.has_value()) {
                        imei = packet.imei;
                        spdlog::info("GT06 device {} identified as IMEI {}", remote_ip, *imei);
                    }
                    break;
                case Gt06Parser::PacketType::kLocation:
                    if (packet.location.has_value()) {
                        if (imei.has_value()) {
                            const auto herald_msg = build_herald_message(*imei, *packet.location);
                            if (ward_manager_.ingest(herald_msg) ==
                                HeraldIngestResult::kWardIdCollision) {
                                spdlog::error(
                                    "GT06 device {} (IMEI {}): entity_id collides with an "
                                    "existing MAVLink ward_id, dropping this fix",
                                    remote_ip, *imei);
                            }
                        } else {
                            spdlog::warn(
                                "GT06 device {} sent a location before logging in, dropping "
                                "(no IMEI known yet for this connection)",
                                remote_ip);
                        }
                    }
                    break;
                case Gt06Parser::PacketType::kHeartbeat:
                case Gt06Parser::PacketType::kUnsupported:
                    break;
            }

            auto ack = Gt06Parser::build_ack(packet.type, packet.serial_number);
            if (!ack.empty()) {
                socket->send(reinterpret_cast<char*>(ack.data()), ack.size());
            }

            buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(result.consumed_bytes));
        }
        if (desync) break;
    }

    spdlog::info("GT06 device {} disconnected", remote_ip);
    connected_clients_.fetch_sub(1);
    connection_state->setTerminated();
}
