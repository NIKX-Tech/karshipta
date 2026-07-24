#include "herald_http_server.h"

#include <google/protobuf/util/json_util.h>
#include <herald/v0/herald.pb.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <system_error>
#include <utility>

namespace {

constexpr auto kDefaultHost = "127.0.0.1";
constexpr uint16_t kDefaultPort = 8766;
constexpr auto kDefaultMappingConfigDir = "gateway/config/herald_mappings";

// Deliberately exact-match only, same as websocket_transport.cpp's own
// is_loopback_host; duplicated rather than shared across libraries for one
// tiny predicate, matching this repo's existing serialize_envelope
// precedent.
bool is_loopback_host(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

// Loads every *.yaml file in mapping_dir as a HeraldFieldMapping, keyed by
// source_name. A missing directory is not an error (the "Mapped" path is
// entirely optional - a deployment using only the native or GT06 paths has
// no reason to create one); a file that fails to parse is logged and
// skipped by HeraldFieldMapper::load_mapping itself, so one broken config
// doesn't take down every other mapping.
std::map<std::string, HeraldFieldMapping> load_mappings(const std::string& mapping_dir) {
    std::map<std::string, HeraldFieldMapping> mappings;
    std::error_code exists_error;
    if (!std::filesystem::exists(mapping_dir, exists_error) || exists_error) {
        spdlog::info("Herald mapping config directory '{}' not found; no mapped sources loaded",
                     mapping_dir);
        return mappings;
    }
    std::error_code iterate_error;
    for (const auto& entry :
         std::filesystem::directory_iterator(mapping_dir, iterate_error)) {
        if (iterate_error) break;
        if (entry.path().extension() != ".yaml") continue;
        if (auto mapping = HeraldFieldMapper::load_mapping(entry.path().string())) {
            const auto source_name = mapping->source_name;
            mappings.emplace(source_name, std::move(*mapping));
            spdlog::info("loaded Herald mapping '{}' from '{}'", source_name, entry.path().string());
        }
    }
    return mappings;
}

}  // namespace

HeraldHttpServer::HeraldHttpServer(HeraldWardManager& ward_manager, std::string host,
                                   const uint16_t port, std::map<std::string, HeraldFieldMapping> mappings)
    : ward_manager_(ward_manager),
      host_(std::move(host)),
      port_(port),
      mappings_(std::move(mappings)),
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
    server_->Post(R"(/herald/mapped/([a-zA-Z0-9_-]+))",
                  [this](const httplib::Request& req, httplib::Response& res) {
                      handle_mapped_request(req, res);
                  });
}

void HeraldHttpServer::handle_mapped_request(const httplib::Request& req, httplib::Response& res) {
    const std::string source_name = req.matches[1];
    const auto mapping_it = mappings_.find(source_name);
    if (mapping_it == mappings_.end()) {
        spdlog::warn("Herald mapped request for unknown source '{}'", source_name);
        res.status = 404;
        return;
    }

    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::parse_error& parse_error) {
        spdlog::warn("Herald mapped source '{}': undecodable {} byte JSON body ({})", source_name,
                     req.body.size(), parse_error.what());
        res.status = 400;
        return;
    }

    const auto herald_msg = HeraldFieldMapper::apply(mapping_it->second, payload);
    if (!herald_msg) {
        // HeraldFieldMapper::apply already logged exactly why.
        res.status = 400;
        return;
    }

    switch (ward_manager_.ingest(*herald_msg)) {
        case HeraldIngestResult::kOk:
            res.status = 200;
            break;
        case HeraldIngestResult::kWardIdCollision:
            res.status = 409;
            break;
    }
}

std::unique_ptr<HeraldHttpServer> HeraldHttpServer::from_config(
    const std::string& config_path, HeraldWardManager& ward_manager,
    const std::function<bool()>& is_in_container) {
    std::string host = kDefaultHost;
    uint16_t port = kDefaultPort;
    bool allow_lan_bind = false;
    bool container_bind = false;
    std::string mapping_config_dir = kDefaultMappingConfigDir;

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
                if (herald["mapping_config_dir"])
                    mapping_config_dir = herald["mapping_config_dir"].as<std::string>();
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
            mapping_config_dir = kDefaultMappingConfigDir;
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

    return std::make_unique<HeraldHttpServer>(ward_manager, std::move(host), port,
                                               load_mappings(mapping_config_dir));
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
