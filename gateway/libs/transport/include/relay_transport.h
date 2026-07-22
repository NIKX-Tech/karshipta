#ifndef KARSHIPTA_GATEWAY_RELAY_TRANSPORT_H
#define KARSHIPTA_GATEWAY_RELAY_TRANSPORT_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "transport.h"

namespace ix {
class WebSocket;
}  // namespace ix

// Credentials this gateway uses to authenticate itself to a relayly server:
// device_id and private_key_path mirror relayly's own Go SDK Options struct
// (DeviceID, PrivateKey loaded via LoadOrGenerateKey(path)). Neither field is
// used yet (see RelayTransport's class comment); a default-constructed,
// all-empty RelayCredentials is valid and keeps RelayTransport in the same
// unauthenticated scaffold mode it was in before this struct existed.
//
// relayly pairs a device with a peer per relationship, not once for the
// whole device: one device_id/private_key stays constant, but each paired
// peer needs its own pairing token (from RequestPairCode/AcceptPair). That
// per-peer token is deliberately not modeled here yet, since it is obtained
// through the still-unimplemented Noise XX handshake, not loaded from static
// config.
struct RelayCredentials {
    std::string device_id;
    std::string private_key_path;
};

// Second Transport implementation (BRIEF.md M4): connects outward to a relay
// service instead of listening, built on relayly (github.com/NIKX-Tech/relayly).
//
// SCAFFOLD ONLY. relayly pairs devices through a Noise Protocol XX handshake
// authenticated by a pairing token, with device_id and the device's private
// key carried as connect-time credentials; none of that handshake is
// implemented here yet, pending the pairing spec and credentials from Erfan
// (BRIEF.md M4). What this class does today is the plain outbound-connect
// plumbing: it opens an unauthenticated, unencrypted WebSocket connection to
// `relay_url` and satisfies the Transport interface against it. Do not point
// it at a real relayly server expecting a paired peer.
class RelayTransport final : public Transport {
   public:
    // relay_url: full ws(s):// URL of the relay endpoint. credentials: this
    // gateway's relay identity; carried for future use, not yet sent over
    // the wire (see class comment). An empty RelayCredentials is valid.
    RelayTransport(std::string relay_url, RelayCredentials credentials);

    // Builds a RelayTransport from relay_url plus RelayCredentials loaded
    // from a YAML config file at config_path (fields: device_id,
    // private_key_path), so callers do not need real credentials to exist
    // yet to wire this in. A missing or unparsable file is logged and
    // treated as an empty RelayCredentials, exactly like calling the
    // constructor directly with RelayCredentials{}.
    static std::unique_ptr<RelayTransport> from_config(std::string relay_url,
                                                        const std::string& config_path);

    // Stops the connection if still running, so no callback can fire against
    // a destroyed RelayTransport.
    ~RelayTransport() override;

    RelayTransport(const RelayTransport&) = delete;
    RelayTransport& operator=(const RelayTransport&) = delete;
    RelayTransport(RelayTransport&&) = delete;
    RelayTransport& operator=(RelayTransport&&) = delete;

    void start() override;
    void stop() override;
    [[nodiscard]] bool is_running() const override;

    void send(ClientId client, const std::vector<uint8_t>& bytes) override;
    void broadcast(const std::vector<uint8_t>& bytes) override;

    // Closes the single outbound relay link if `client` matches the current
    // peer_id_ (there is only ever one peer in this scaffold, see the class
    // comment). This drops the whole connection, not just one logical peer;
    // the socket's own Close callback then does the usual peer_id_ reset and
    // fires on_disconnect. No-op if `client` doesn't match (including 0, the
    // not-connected sentinel).
    void disconnect(ClientId client) override;

    // Always kOperator: relayly pairing has no per-peer role concept yet
    // (see the class comment), so there is nothing to mark a peer viewer
    // with. Revisit once peer identity exists past the Noise XX handshake.
    [[nodiscard]] ClientRole role(ClientId client) const override;

    // The credentials this instance was built with, whether via the
    // constructor or from_config(). Read-only: there is no setter, since
    // credentials are meant to be fixed for the lifetime of one instance.
    [[nodiscard]] const RelayCredentials& credentials() const { return credentials_; }

    // Not synchronized against the ix client's own callback thread: must be
    // called before start(), never while the connection is running.
    void on_receive(ReceiveCallback callback) override;
    void on_connect(ConnectCallback callback) override;
    void on_disconnect(DisconnectCallback callback) override;

   private:
    std::string relay_url_;
    RelayCredentials credentials_;
    std::atomic<bool> running_{false};

    // Guards socket_/peer_id_ against the ix client's own callback thread
    // racing send()/broadcast() called from our caller's thread.
    mutable std::mutex peer_mutex_;
    std::shared_ptr<ix::WebSocket> socket_;
    // Id representing the single underlying WebSocket link to the relay
    // server, or 0 if not connected. This is NOT a relayly peer id: relayly
    // itself lets one device pair with several peers at once, each with its
    // own id (see class comment and gateway/docs/relay-transport.md), but
    // peer identity only exists once the Noise XX handshake and pairing are
    // implemented. Until then, this id stands in for "the relay link is up",
    // not for any specific paired peer.
    ClientId peer_id_{0};
    std::atomic<ClientId> next_client_id_{1};

    ReceiveCallback on_receive_;
    ConnectCallback on_connect_;
    DisconnectCallback on_disconnect_;
};

#endif  // KARSHIPTA_GATEWAY_RELAY_TRANSPORT_H
