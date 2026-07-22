#include "herald_http_server.h"

#include <google/protobuf/util/json_util.h>
#include <herald/v0/herald.pb.h>
#include <httplib.h>
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <system_error>
#include <utility>

namespace {

constexpr auto kDefaultHost = "127.0.0.1";
constexpr uint16_t kDefaultPort = 8766;

// Deliberately exact-match only, same as websocket_transport.cpp's own
// is_loopback_host; duplicated rather than shared across libraries for one
// tiny predicate, matching this repo's existing serialize_envelope
// precedent.
bool is_loopback_host(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

}  // namespace

HeraldHttpServer::HeraldHttpServer(HeraldWardManager& ward_manager, std::string host,
                                   const uint16_t port)
    : ward_manager_(ward_manager),
      host_(std::move(host)),
      port_(port),
      server_(std::make_unique<httplib::Server>()) {
    server_->Post("/herald", [this](const httplib::Request& req, httplib::Response& res) {
        herald::v0::Herald msg;
        bool parsed;
        if (req.get_header_value("Content-Type") == "application/json") {
            parsed = google::protobuf::util::JsonStringToMessage(req.body, &msg).ok();
        } else {
            parsed = msg.ParseFromString(req.body);
        }
        if (!parsed) {
            spdlog::warn("undecodable {} byte Herald HTTP body", req.body.size());
            res.status = 400;
            return;
        }
        switch (ward_manager_.ingest(msg)) {
            case HeraldIngestResult::kOk:
                res.status = 200;
                break;
            case HeraldIngestResult::kWardIdCollision:
                res.status = 409;
                break;
        }
    });
}

std::unique_ptr<HeraldHttpServer> HeraldHttpServer::from_config(
    const std::string& config_path, HeraldWardManager& ward_manager,
    const std::function<bool()>& is_in_container) {
    std::string host = kDefaultHost;
    uint16_t port = kDefaultPort;
    bool allow_lan_bind = false;
    bool container_bind = false;

    std::error_code exists_error;
    if (!std::filesystem::exists(config_path, exists_error) || exists_error) {
        spdlog::info(
            "gateway config '{}' not found; Herald HTTP listener binding to the safe default "
            "{}:{}",
            config_path, kDefaultHost, kDefaultPort);
    } else {
        try {
            const YAML::Node root = YAML::LoadFile(config_path);
            if (const YAML::Node herald = root["herald"]; herald) {
                if (herald["host"]) host = herald["host"].as<std::string>();
                if (herald["http_port"]) port = herald["http_port"].as<uint16_t>();
                if (herald["allow_lan_bind"]) allow_lan_bind = herald["allow_lan_bind"].as<bool>();
                if (herald["container_bind"]) container_bind = herald["container_bind"].as<bool>();
            }
        } catch (const YAML::Exception& parse_error) {
            spdlog::error(
                "failed to parse gateway config '{}': {}; Herald HTTP listener binding to the "
                "safe default {}:{}",
                config_path, parse_error.what(), kDefaultHost, kDefaultPort);
            host = kDefaultHost;
            port = kDefaultPort;
            allow_lan_bind = false;
            container_bind = false;
        }
    }

    if (is_loopback_host(host)) {
        // Nothing to gate: fall through and bind as requested.
    } else if (allow_lan_bind) {
        spdlog::warn(
            "SECURITY: Herald HTTP listener is binding to {}:{}, reachable from other machines "
            "on this network with NO AUTHENTICATION. Anyone who can reach this address can "
            "inject ward telemetry.",
            host, port);
    } else if (container_bind && is_in_container()) {
        spdlog::warn(
            "Herald HTTP listener is binding to {}:{} because herald.container_bind is set and "
            "this process detected it is running inside a container. This is reachable only "
            "through whatever ports the container runtime publishes to the host, not directly "
            "from the LAN; see gateway/docs/websocket-transport.md for the same reasoning "
            "applied to the websocket transport.",
            host, port);
    } else if (container_bind) {
        spdlog::warn(
            "gateway config '{}' sets herald.container_bind: true, but this process is not "
            "running inside a container; ignoring and forcing 127.0.0.1 instead.",
            config_path);
        host = kDefaultHost;
    } else {
        spdlog::warn(
            "gateway config '{}' requests herald.host='{}', but herald.allow_lan_bind is off; "
            "forcing 127.0.0.1 instead. Set herald.allow_lan_bind: true if a Herald source "
            "really needs to reach this gateway over the LAN.",
            config_path, host);
        host = kDefaultHost;
    }

    return std::make_unique<HeraldHttpServer>(ward_manager, std::move(host), port);
}

HeraldHttpServer::~HeraldHttpServer() { stop(); }

void HeraldHttpServer::start() {
    if (listen_thread_.joinable()) return;
    listen_thread_ = std::jthread([this] {
        if (!server_->listen(host_, port_)) {
            spdlog::error("Herald HTTP listener failed to bind {}:{}", host_, port_);
        }
    });
}

void HeraldHttpServer::stop() {
    if (!listen_thread_.joinable()) return;
    server_->stop();
    listen_thread_.join();
}
