#ifndef KARSHIPTA_GATEWAY_GT06_TCP_SERVER_H
#define KARSHIPTA_GATEWAY_GT06_TCP_SERVER_H

#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXSocket.h>
#include <ixwebsocket/IXSocketServer.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "herald_ward_manager.h"

// Defined in websocket_transport.cpp (declared again here too, mirroring
// HeraldHttpServer::from_config's own forward declaration) so this header
// doesn't need to include websocket_transport.h just for one free function.
bool is_running_in_container();

// Raw TCP listener for the GT06 tracker protocol (github issue #123): a
// binary wire format (0x78 0x78 frame header, conventionally port 5023)
// hundreds of low-cost GPS tracker models speak natively (Concox/Jimi/iTrack
// family and OEM rebrands). Subclasses ix::SocketServer - already vendored
// for the websocket transport - rather than hand-rolling a second
// cross-platform socket accept loop; gt06_parser.h owns the pure framing/
// decode logic this class wraps around a real connection, the same
// HeraldHttpServer/HeraldWardManager split (network concern vs. mapping
// concern) already established for the HTTP path.
//
// One worker thread per connection (ix::SocketServer's own model, see
// IXSocketServer.h). GT06 devices send a login packet (IMEI) once per
// connection and imply it for every subsequent packet, so each connection's
// handler remembers its own IMEI locally and combines it with each decoded
// location to build and ingest() a herald::v0::Herald - the same
// normalization entry point the HTTP path already uses.
class Gt06TcpServer : public ix::SocketServer {
   public:
    // host/port to listen on. Prefer from_config() at call sites; mirrors
    // HeraldHttpServer's constructor/from_config split.
    Gt06TcpServer(HeraldWardManager& ward_manager, std::string host, uint16_t port);

    // Builds host/port from a YAML config file at config_path (keys:
    // herald.gt06_host, herald.gt06_port, herald.allow_lan_bind,
    // herald.container_bind - the last two shared with HeraldHttpServer's
    // own keys, since they express the same safe-bind policy for whichever
    // Herald listener is asking). Falls back to (127.0.0.1, 5023) - GT06's
    // conventional port - on a missing or malformed config.
    static std::unique_ptr<Gt06TcpServer> from_config(
        const std::string& config_path, HeraldWardManager& ward_manager,
        const std::function<bool()>& is_in_container = is_running_in_container);

    ~Gt06TcpServer() override;

    Gt06TcpServer(const Gt06TcpServer&) = delete;
    Gt06TcpServer& operator=(const Gt06TcpServer&) = delete;
    Gt06TcpServer(Gt06TcpServer&&) = delete;
    Gt06TcpServer& operator=(Gt06TcpServer&&) = delete;

    // ix::SocketServer::start() (inherited, not shadowed by name here)
    // only launches the accept-loop/GC threads - it does not bind the
    // listening socket, that is the separate listen() call. This wraps
    // both into one call with the same "log and don't throw on bind
    // failure" behavior HeraldHttpServer::start() and
    // WebsocketTransport::start() already have, so main.cpp can just call
    // start() like it does for every other listener.
    void start();

    // Stops accepting new connections and signals every in-flight
    // connection handler to exit at its next poll timeout (up to ~1s).
    // Idempotent.
    void stop() override;

   private:
    void handleConnection(std::unique_ptr<ix::Socket> socket,
                           std::shared_ptr<ix::ConnectionState> connection_state) override;
    size_t getConnectedClientsCount() override;

    HeraldWardManager& ward_manager_;
    std::atomic<bool> stopping_{false};
    std::atomic<size_t> connected_clients_{0};
};

#endif  // KARSHIPTA_GATEWAY_GT06_TCP_SERVER_H
