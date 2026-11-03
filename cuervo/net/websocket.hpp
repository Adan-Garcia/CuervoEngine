#pragma once

#include <memory>
#include <string>
#include <utility>

#include "cuervo/net/transport.hpp"

// WebSocket Transport (DESIGN.md 4.19): browsers have no UDP, so packets travel as binary
// WebSocket messages to a relay that forwards them between peers by PeerId. Relay protocol
// (reference implementation: tools/net/relay.py): connect to ws://host:port/<room>; the relay
// sends one message with this client's id (u32 LE; ids are 0,1,2... in join order, so the id
// is the player number); afterwards send [to:u32 LE][payload] and receive [from:u32 LE][payload];
// to = broadcast_peer reaches every other peer in the room.
//
// Desktop: a minimal RFC 6455 client over plain TCP (handshake verified, client frames masked,
// ping answered, fragments joined), over TLS for wss:// (Mbed TLS, cuervo/net/tcp.hpp; a build
// without CUERVO_TLS refuses wss:// with a clear error). Web (Emscripten): the browser WebSocket.
// Delivery is best-effort datagrams like UDP, though TCP-backed in practice.
namespace cuervo::net {

struct WebSocketOptions {
    WebSocketOptions(std::string url_ = {}, std::string room_ = {}) : url(std::move(url_)), room(std::move(room_)) {}
    std::string url;  // ws://host:port[/path] relay endpoint
    std::string room; // relay room to join (appended to the path)
    // wss:// (desktop, CUERVO_TLS): the certificate is verified against the system trust store, or
    // `ca_file` (a PEM bundle) when set. `insecure` skips verification and exists for tests against
    // a local self-signed relay only. The web build ignores both (the browser verifies).
    std::string ca_file;
    bool insecure = false;
};

class WebSocketTransport : public Transport {
public:
    static constexpr PeerId broadcast_peer = 0xFFFFFFFFu;
    // False until the relay assigned an id (always true on desktop, where open() blocks for it;
    // on web open() cannot block, so poll this) and false again after the connection closed.
    virtual bool ready() const = 0;
    virtual bool closed() const = 0;
};

// Null with `error` set on failure (bad URL, connect, TLS or certificate failure, handshake or id timeout).
std::unique_ptr<WebSocketTransport> open_websocket_transport(const WebSocketOptions& options, std::string& error);

} // namespace cuervo::net
