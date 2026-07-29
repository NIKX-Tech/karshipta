#ifndef KARSHIPTA_GATEWAY_RELAY_TRANSPORT_H
#define KARSHIPTA_GATEWAY_RELAY_TRANSPORT_H

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <relayly/client.hpp>

#include "transport.h"

// Credentials this gateway uses to authenticate itself to a relayly server,
// mirroring relayly::Options (sdk/cpp/include/relayly/client.hpp): device_id
// and device_token both come from relayly's device registration (`POST
// /api/v1/devices` or the `relayly pair <name>` CLI, docs/PROTOCOL.md §2) and
// are opaque to us; private_key_path is where this device's X25519 static
// identity is loaded from (or generated into, on first run) via
// relayly::PrivateKey::LoadOrGenerate. A default-constructed, all-empty
// RelayCredentials is valid to build a RelayTransport with, but start()
// refuses to connect without both device_id and device_token set (see its
// doc comment).
struct RelayCredentials {
    std::string device_id;
    std::string private_key_path;
    std::string device_token;
};

// Second Transport implementation (BRIEF.md M4): connects outward to a relay
// service instead of listening, wrapping relayly's own C++ SDK
// (github.com/NIKX-Tech/relayly, sdk/cpp) rather than a raw WebSocket. relayly
// handles the Noise XX handshake, pairing, peer key pinning, and reconnection
// internally; this class only adapts its single-peer model onto Transport's
// multi-client-shaped interface and forwards Envelope bytes as opaque
// payloads.
//
// v1 links exactly one peer per device (relayly's own constraint, see
// sdk/cpp/README.md's Pairing section) - a paired peer is treated as "the"
// connected Transport::ClientId, assigned once and re-used across
// reconnects for as long as that same peer stays paired. Pairing itself
// (request_pair_code/accept_pair) is a deliberate, one-time setup action, not
// something start() does automatically: once paired, the relay server
// remembers the link server-side (docs/PROTOCOL.md §5.3) and replays it via
// the `welcome`/`peer_status` control messages on every future connect, so a
// restarted gateway reconnects to its already-paired console without
// re-pairing.
class RelayTransport final : public Transport {
   public:
    // relay_url: full ws(s):// URL of the relay endpoint's /ws path.
    // credentials: this gateway's relay identity. An empty RelayCredentials
    // is valid to construct with; start() will refuse to connect until
    // device_id and device_token are both set.
    RelayTransport(std::string relay_url, RelayCredentials credentials);

    // Builds a RelayTransport from relay_url plus RelayCredentials loaded
    // from a YAML config file at config_path (fields: device_id,
    // private_key_path, device_token). A missing or unparsable file is
    // logged and treated as an empty RelayCredentials, exactly like calling
    // the constructor directly with RelayCredentials{}.
    static std::unique_ptr<RelayTransport> from_config(std::string relay_url,
                                                        const std::string& config_path);

    // Stops the connection if still running, so no callback can fire against
    // a destroyed RelayTransport.
    ~RelayTransport() override;

    RelayTransport(const RelayTransport&) = delete;
    RelayTransport& operator=(const RelayTransport&) = delete;
    RelayTransport(RelayTransport&&) = delete;
    RelayTransport& operator=(RelayTransport&&) = delete;

    // Connects to the relay server and blocks until relayly::Client::Connect
    // returns, i.e. until the control-channel handshake (welcome +
    // announce_key, docs/PROTOCOL.md §3/§5) completes or fails - this differs
    // from WebsocketTransport::start(), which only begins listening and
    // returns immediately, but matches relayly's own Client::Connect
    // contract (see sdk/cpp/README.md's quick start, which calls it the same
    // way). Does NOT block waiting for a peer to be paired or become ready:
    // that is reported asynchronously via on_connect once relayly signals
    // the paired peer's session is usable (see class comment). A no-op if
    // credentials_.device_id or credentials_.device_token is empty (logged,
    // not thrown) or if already running.
    void start() override;
    void stop() override;
    [[nodiscard]] bool is_running() const override;

    void send(ClientId client, const std::vector<uint8_t>& bytes) override;
    void broadcast(const std::vector<uint8_t>& bytes) override;

    // Closes the single outbound relay link if `client` matches the
    // currently assigned peer (current_peer_id_/assigned_id_ - there is only
    // ever one peer, v1's one-peer-per-device constraint, see the class
    // comment). This drops the whole connection, not just one logical peer;
    // relayly's own on_peer_status(false) then does the usual assignment
    // reset and fires on_disconnect. No-op if `client` doesn't match
    // (including 0, the not-connected sentinel).
    void disconnect(ClientId client) override;

    // Always kOperator: relayly pairing has no per-peer role concept, so
    // there is nothing to mark a peer viewer with.
    [[nodiscard]] ClientRole role(ClientId client) const override;

    // The credentials this instance was built with, whether via the
    // constructor or from_config(). Read-only: there is no setter, since
    // credentials are meant to be fixed for the lifetime of one instance.
    [[nodiscard]] const RelayCredentials& credentials() const { return credentials_; }

    // Asks the relay server for a fresh 6-digit pairing code (blocks for one
    // server round trip). Share code.short_code() (or code.QrCodeUrl())
    // out of band with the console; code.wait() resolves once the console
    // accepts it and the resulting Noise handshake completes. Only valid
    // after start() has connected; throws relayly::Error(kClosed) otherwise.
    [[nodiscard]] relayly::PairCode request_pair_code();

    // Uses a 6-digit code obtained from the other side's request_pair_code()
    // to complete pairing from this end. The returned future resolves once
    // the resulting Noise handshake completes. Only valid after start() has
    // connected; throws relayly::Error(kClosed) otherwise.
    [[nodiscard]] std::future<relayly::Peer> accept_pair(const std::string& code);

    // Not synchronized against relayly's own callback thread: must be
    // called before start(), never while the connection is running.
    void on_receive(ReceiveCallback callback) override;
    void on_connect(ConnectCallback callback) override;
    void on_disconnect(DisconnectCallback callback) override;

   private:
    void handle_peer_ready(const std::string& peer_id);
    void handle_peer_status(const std::string& peer_id, bool online);
    void handle_message(const relayly::Message& message);

    std::string relay_url_;
    RelayCredentials credentials_;
    std::atomic<bool> running_{false};

    std::unique_ptr<relayly::Client> client_;

    // Guards current_peer_/assigned_id_ against relayly's own callback
    // thread racing send()/broadcast() called from our caller's thread.
    mutable std::mutex peer_mutex_;
    // The relayly peer id of the currently paired-and-ready peer, or nullopt
    // if none. v1 links exactly one peer per device (see class comment), so
    // this is a single optional slot, not a map.
    std::optional<std::string> current_peer_id_;
    // The Transport::ClientId assigned to current_peer_id_, stable across
    // reconnects for as long as the same relayly peer id stays current.
    ClientId assigned_id_{0};
    std::atomic<ClientId> next_client_id_{1};

    ReceiveCallback on_receive_;
    ConnectCallback on_connect_;
    DisconnectCallback on_disconnect_;
};

#endif  // KARSHIPTA_GATEWAY_RELAY_TRANSPORT_H
