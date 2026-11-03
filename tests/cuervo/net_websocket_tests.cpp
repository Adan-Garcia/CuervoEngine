#include <doctest.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>


#include "cuervo/net/websocket.hpp"
#include "net_relay_fixture.hpp"

// Runs the desktop client against tools/net/relay.py on an ephemeral port (no fixed ports).
// Skipped when python3 or popen is unavailable.
using namespace cuervo::net;

#if !defined(_WIN32)
namespace {

std::vector<std::byte> bytes(const std::string& s) {
    std::vector<std::byte> v(s.size());
    if (!s.empty()) std::memcpy(v.data(), s.data(), s.size());
    return v;
}

bool wait_recv(Transport& t, Packet& p) {
    for (int i = 0; i < 1000; ++i) {
        if (t.recv(p)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

} // namespace

TEST_CASE("websocket: two clients exchange datagrams through the relay") {
    Relay relay;
    if (!relay.ok()) {
        MESSAGE("python3 relay unavailable; skipped");
        return;
    }
    const std::string url = "ws://127.0.0.1:" + std::to_string(relay.port);
    std::string err;
    auto a = open_websocket_transport({url, "room-a"}, err);
    REQUIRE_MESSAGE(a, err);
    auto b = open_websocket_transport({url, "room-a"}, err);
    REQUIRE_MESSAGE(b, err);
    auto other = open_websocket_transport({url, "room-b"}, err);
    REQUIRE_MESSAGE(other, err);
    CHECK(a->local_id() == 0);
    CHECK(b->local_id() == 1);
    CHECK(other->local_id() == 0); // ids are per room

    CHECK(a->send(1, bytes("ping")));
    Packet p;
    REQUIRE(wait_recv(*b, p));
    CHECK(p.from == 0);
    CHECK(p.data == bytes("ping"));

    CHECK(b->send(WebSocketTransport::broadcast_peer, bytes("all")));
    REQUIRE(wait_recv(*a, p));
    CHECK(p.from == 1);
    CHECK(p.data == bytes("all"));

    // A maximum-size datagram (extended 16-bit length) survives, and rooms are isolated.
    std::vector<std::byte> big(Transport::max_packet_size, std::byte{0x5A});
    CHECK(a->send(1, big));
    REQUIRE(wait_recv(*b, p));
    CHECK(p.data == big);
    CHECK_FALSE(a->send(1, std::vector<std::byte>(Transport::max_packet_size + 1)));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_FALSE(other->recv(p));
}

#if defined(CUERVO_HAS_TLS)
TEST_CASE("websocket: wss through a TLS relay verifies the certificate") {
    TestCert cert;
    if (!cert.ok()) {
        MESSAGE("openssl unavailable; skipped");
        return;
    }
    Relay relay("--tls-cert \"" + cert.cert() + "\" --tls-key \"" + cert.key() + "\"");
    if (!relay.ok()) {
        MESSAGE("python3 relay unavailable; skipped");
        return;
    }
    const std::string url = "wss://127.0.0.1:" + std::to_string(relay.port);
    std::string err;

    SUBCASE("trusting the certificate through ca_file") {
        WebSocketOptions o{url, "tls-room"};
        o.ca_file = cert.cert();
        auto a = open_websocket_transport(o, err);
        REQUIRE_MESSAGE(a, err);
        auto b = open_websocket_transport(o, err);
        REQUIRE_MESSAGE(b, err);
        CHECK(a->send(1, bytes("secret")));
        Packet p;
        REQUIRE(wait_recv(*b, p));
        CHECK(p.data == bytes("secret"));
        std::vector<std::byte> big(Transport::max_packet_size, std::byte{0x33});
        CHECK(b->send(0, big));
        REQUIRE(wait_recv(*a, p));
        CHECK(p.data == big);
    }
    SUBCASE("an untrusted self-signed certificate is refused by default") {
        auto a = open_websocket_transport({url, "tls-room"}, err); // system store only
        CHECK_FALSE(a);
        CHECK(err.find("verification failed") != std::string::npos);
    }
    SUBCASE("a wrong ca_file is refused") {
        TestCert other;
        REQUIRE(other.ok());
        WebSocketOptions o{url, "tls-room"};
        o.ca_file = other.cert();
        CHECK_FALSE(open_websocket_transport(o, err));
        CHECK(err.find("verification failed") != std::string::npos);
    }
    SUBCASE("a missing ca_file is an error, not a silent skip") {
        WebSocketOptions o{url, "tls-room"};
        o.ca_file = (cert.dir / "missing.pem").string();
        CHECK_FALSE(open_websocket_transport(o, err));
        CHECK(err.find("ca_file") != std::string::npos);
    }
    SUBCASE("insecure connects without verification (test-only switch)") {
        WebSocketOptions o{url, "tls-room"};
        o.insecure = true;
        auto a = open_websocket_transport(o, err);
        CHECK_MESSAGE(a, err);
    }
    SUBCASE("a plain ws:// client cannot talk to the TLS relay") {
        CHECK_FALSE(open_websocket_transport({"ws://127.0.0.1:" + std::to_string(relay.port), "r"}, err));
    }
}
#endif
#endif

TEST_CASE("websocket: malformed URLs and unreachable hosts fail with a clear error") {
    std::string err;
    CHECK_FALSE(open_websocket_transport({"wss://example.invalid/x", "r"}, err)); // does not resolve
    CHECK_FALSE(err.empty());
    CHECK_FALSE(open_websocket_transport({"http://x", "r"}, err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(open_websocket_transport({"ws://127.0.0.1:1", "r"}, err)); // nothing listens
    CHECK_FALSE(err.empty());
}
