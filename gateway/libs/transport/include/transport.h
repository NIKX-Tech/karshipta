#ifndef KARSHIPTA_GATEWAY_TRANSPORT_H
#define KARSHIPTA_GATEWAY_TRANSPORT_H

#include <cstdint>
#include <functional>
#include <vector>

// Abstraction over how Envelope bytes reach a console client. Two
// implementations are expected: a plain WebSocket server (this one) and an
// outbound relay transport (BRIEF.md M4). Code above this interface (main,
// a future Gateway/VehicleManager) must talk to Transport only and must never
// see WebSocket or relay types.
class Transport {
public:
    // Identifies one connected client, scoped to a single Transport instance.
    // Passed to send() to target a specific client and to the connect/
    // disconnect callbacks to say which client changed state.
    using ClientId = uint64_t;

    // Fired once per binary frame received from a client. `bytes` is a
    // serialized Envelope; Transport never parses it, only carries it.
    using ReceiveCallback = std::function<void(ClientId client, const std::vector<uint8_t>& bytes)>;
    // Fired when a new client connects, before any of its frames are delivered.
    using ConnectCallback = std::function<void(ClientId client)>;
    // Fired when a client disconnects, whether closed cleanly or link drop.
    using DisconnectCallback = std::function<void(ClientId client)>;

    virtual ~Transport() = default;

    // Starts listening (or, for a future relay transport, connecting out).
    // Idempotent: a no-op if already running.
    virtual void start() = 0;
    // Stops and drops every client. Idempotent: a no-op if not running.
    virtual void stop() = 0;
    [[nodiscard]] virtual bool is_running() const = 0;

    // Sends bytes (a serialized Envelope) to one specific client. No-op if
    // `client` is not currently connected.
    virtual void send(ClientId client, const std::vector<uint8_t>& bytes) = 0;
    // Sends bytes to every currently connected client.
    virtual void broadcast(const std::vector<uint8_t>& bytes) = 0;

    // Registers the callback invoked for every received frame. Only one
    // callback at a time, and registration must happen before start():
    // implementations deliver events from their own threads once running.
    virtual void on_receive(ReceiveCallback callback) = 0;
    // Registers the callback invoked when a client connects.
    virtual void on_connect(ConnectCallback callback) = 0;
    // Registers the callback invoked when a client disconnects.
    virtual void on_disconnect(DisconnectCallback callback) = 0;
};

#endif  // KARSHIPTA_GATEWAY_TRANSPORT_H
