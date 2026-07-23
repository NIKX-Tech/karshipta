// Small operator CLI for triggering relay pairing (gateway/docs/relay-
// transport.md's "Still open" list): request_pair_code/accept_pair have no
// caller anywhere else in this repo. Connects with the same relay
// credentials the real gateway process uses, requests a 6-digit code,
// prints it for the operator to type into the console's pairing UI, then
// blocks until the console accepts it or the code expires. Pairing is
// server-side durable (relay-transport.md: "the relay server remembers the
// pairing... and replays it... on every future connect"), so this tool does
// not need to stay running afterward - the real gateway process, started
// separately with the same device_id, picks up the pairing on its own next
// connect.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <string>

#include <relayly/errors.hpp>
#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include "relay_transport.h"

namespace {

constexpr auto kDefaultConfigPath = "gateway/config/gateway.yaml";
constexpr auto kDefaultRelayCredentialsPath = "gateway/config/relay_credentials.yaml";

// Mirrors main.cpp's build_transport() config reading (relay.url,
// relay.credentials_path), scoped to just the two fields this tool needs.
// Not shared code with main.cpp: that function returns a Transport&, which
// hides request_pair_code()/accept_pair() (relay-specific, not part of the
// Transport interface, see relay_transport.h) - this tool needs the
// concrete RelayTransport, so it reads the same two config keys itself
// rather than reaching into build_transport()'s result.
struct RelayConfig {
    std::string url;
    std::string credentials_path = kDefaultRelayCredentialsPath;
};

RelayConfig load_relay_config(const std::string& config_path) {
    RelayConfig config;
    std::error_code exists_error;
    if (!std::filesystem::exists(config_path, exists_error) || exists_error) {
        spdlog::warn("gateway config '{}' not found; relay.url will be empty", config_path);
        return config;
    }
    try {
        const YAML::Node root = YAML::LoadFile(config_path);
        if (const YAML::Node relay = root["relay"]; relay) {
            if (relay["url"]) config.url = relay["url"].as<std::string>();
            if (relay["credentials_path"])
                config.credentials_path = relay["credentials_path"].as<std::string>();
        }
    } catch (const YAML::Exception& parse_error) {
        spdlog::error("failed to parse '{}': {}", config_path, parse_error.what());
    }
    return config;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string config_path = argc > 1 ? argv[1] : kDefaultConfigPath;

    const RelayConfig relay_config = load_relay_config(config_path);
    if (relay_config.url.empty()) {
        spdlog::error(
            "no relay.url in '{}' - set it before pairing (gateway/docs/relay-transport.md)",
            config_path);
        return EXIT_FAILURE;
    }

    auto transport = RelayTransport::from_config(relay_config.url, relay_config.credentials_path);
    const RelayCredentials& credentials = transport->credentials();
    if (credentials.device_id.empty() || credentials.device_token.empty()) {
        spdlog::error(
            "'{}' has no device_id/device_token - provision one against the relay server first "
            "(see gateway/docs/relay-transport.md's Credentials section)",
            relay_config.credentials_path);
        return EXIT_FAILURE;
    }

    spdlog::info("connecting to relay at {} as device '{}'...", relay_config.url,
                 credentials.device_id);
    try {
        transport->start();
    } catch (const relayly::Error& error) {
        spdlog::error("failed to connect: {}", error.what());
        return EXIT_FAILURE;
    }

    relayly::PairCode code;
    try {
        code = transport->request_pair_code();
    } catch (const relayly::Error& error) {
        spdlog::error("failed to request a pairing code: {}", error.what());
        transport->stop();
        return EXIT_FAILURE;
    }

    std::cout << "\nPairing code: " << code.short_code() << "\n"
              << "Expires in " << code.expires_in() << "s. Enter this in the console's "
              << "connection panel (Relay mode) to pair.\n\n"
              << "Waiting for the console to accept it..." << std::endl;

    std::future<relayly::Peer> pairing = code.wait();
    const auto status = pairing.wait_for(std::chrono::seconds(code.expires_in()));
    if (status != std::future_status::ready) {
        spdlog::error("pairing code expired with no console accepting it");
        transport->stop();
        return EXIT_FAILURE;
    }

    try {
        const relayly::Peer peer = pairing.get();
        std::cout << "Paired with " << peer.id << ". You can now start the gateway normally "
                  << "with transport: relay in " << config_path << "." << std::endl;
    } catch (const relayly::Error& error) {
        spdlog::error("pairing failed: {}", error.what());
        transport->stop();
        return EXIT_FAILURE;
    }

    transport->stop();
    return EXIT_SUCCESS;
}
