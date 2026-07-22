#ifndef KARSHIPTA_GATEWAY_HERALD_HTTP_SERVER_H
#define KARSHIPTA_GATEWAY_HERALD_HTTP_SERVER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "herald_ward_manager.h"

namespace httplib {
class Server;
}  // namespace httplib

// Defined in websocket_transport.cpp (declared there too); forward-declared
// again here so from_config()'s default argument doesn't require this
// header to include websocket_transport.h just for one free function.
bool is_running_in_container();

// Thin HTTP listener for Herald ingestion (github.com/NIKX-Tech/herald): one
// route, POST /herald, translating the request body into a herald::v0::Herald
// and handing it to HeraldWardManager::ingest(). Kept separate from
// HeraldWardManager itself so the ingestion/mapping logic stays testable
// with a FakeTransport and no real socket (see
// gateway/tests/herald/herald_ward_manager_test.cpp) - this class owns only
// the network transport concern, the same separation WardManager and
// WebsocketTransport already have from each other.
class HeraldHttpServer {
   public:
    // host/port to listen on. Prefer from_config() at call sites; mirrors
    // WebsocketTransport's constructor/from_config split.
    HeraldHttpServer(HeraldWardManager& ward_manager, std::string host, uint16_t port);

    // Builds host/port from a YAML config file at config_path (keys:
    // herald.host, herald.http_port, herald.allow_lan_bind,
    // herald.container_bind), the exact same safe-by-default bind policy as
    // WebsocketTransport::from_config: a non-loopback host is only honored
    // if herald.allow_lan_bind is true, or if herald.container_bind is true
    // and is_in_container reports true (the docker-compose demo's case,
    // see deploy/gateway-config.yaml). Falls back to (127.0.0.1, 8766) on a
    // missing or malformed config. is_in_container exists as a parameter
    // only so tests can inject a fixed answer instead of depending on
    // /.dockerenv, same reasoning as WebsocketTransport::from_config's own
    // parameter.
    static std::unique_ptr<HeraldHttpServer> from_config(
        const std::string& config_path, HeraldWardManager& ward_manager,
        const std::function<bool()>& is_in_container = is_running_in_container);

    // Stops the server if still running, so no request callback can fire
    // against a destroyed HeraldHttpServer.
    ~HeraldHttpServer();

    HeraldHttpServer(const HeraldHttpServer&) = delete;
    HeraldHttpServer& operator=(const HeraldHttpServer&) = delete;
    HeraldHttpServer(HeraldHttpServer&&) = delete;
    HeraldHttpServer& operator=(HeraldHttpServer&&) = delete;

    // Starts listening on its own thread (httplib::Server::listen() blocks
    // the calling thread). Idempotent: a no-op if already running.
    void start();
    // Stops the server and joins its thread. Idempotent.
    void stop();

   private:
    HeraldWardManager& ward_manager_;
    std::string host_;
    uint16_t port_;
    std::unique_ptr<httplib::Server> server_;
    std::jthread listen_thread_;
};

#endif  // KARSHIPTA_GATEWAY_HERALD_HTTP_SERVER_H
