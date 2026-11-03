#include <doctest.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cuervo/net/udp.hpp"
#include "cuervo/net/websocket.hpp"

using namespace cuervo::net;

namespace {
std::vector<std::byte> bytes(const std::string& s) {
    std::vector<std::byte> v(s.size());
    if (!s.empty()) std::memcpy(v.data(), s.data(), s.size());
    return v;
}
// Loopback delivery is not instant: poll briefly (test-only wall clock, never the sim).
bool wait_recv(Transport& t, Packet& p) {
    for (int i = 0; i < 500; ++i) {
        if (t.recv(p)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
} // namespace

TEST_CASE("udp: two sockets on 127.0.0.1 exchange packets and learn each other") {
    std::string err;
    auto a = UdpTransport::open("127.0.0.1:0", err);
    REQUIRE_MESSAGE(a, err);
    auto b = UdpTransport::open("127.0.0.1:0", err);
    REQUIRE_MESSAGE(b, err);
    CHECK(a->local_port() != 0);
    CHECK(a->local_port() != b->local_port());

    const PeerId ab = a->add_peer(b->local_address());
    REQUIRE(ab == 1);
    CHECK(a->add_peer(b->local_address()) == ab);
    CHECK(a->peer_address(ab) == b->local_address());

    Packet p;
    CHECK_FALSE(a->recv(p)); // non-blocking: nothing yet
    CHECK(a->send(ab, bytes("hello")));
    REQUIRE(wait_recv(*b, p));
    CHECK(p.data == bytes("hello"));
    CHECK(p.from == 1); // b registered a as its first peer

    CHECK(b->send(p.from, bytes("reply")));
    REQUIRE(wait_recv(*a, p));
    CHECK(p.from == ab);
    CHECK(p.data == bytes("reply"));
}

TEST_CASE("udp: size limit, unknown peers and bad addresses are refused") {
    std::string err;
    auto a = UdpTransport::open("127.0.0.1:0", err);
    REQUIRE_MESSAGE(a, err);
    auto b = UdpTransport::open("127.0.0.1:0", err);
    REQUIRE_MESSAGE(b, err);
    const PeerId ab = a->add_peer(b->local_address());
    CHECK_FALSE(a->send(ab, std::vector<std::byte>(Transport::max_packet_size + 1)));
    CHECK(a->send(ab, std::vector<std::byte>(Transport::max_packet_size)));
    CHECK_FALSE(a->send(99, bytes("x")));
    CHECK_FALSE(a->send(0, bytes("x")));
    CHECK(a->add_peer("nonsense") == 0);
    CHECK(a->add_peer("127.0.0.1:70000") == 0);
    CHECK_FALSE(UdpTransport::open("127.0.0.1", err));
    CHECK_FALSE(err.empty());
    Packet p;
    REQUIRE(wait_recv(*b, p));
    CHECK(p.data.size() == Transport::max_packet_size);
}

TEST_CASE("udp: the peer table is bounded against unknown senders") {
    std::string err;
    auto host = UdpTransport::open("127.0.0.1:0", err);
    REQUIRE_MESSAGE(host, err);
    std::vector<std::unique_ptr<UdpTransport>> clients;
    for (std::size_t i = 0; i < UdpTransport::max_peers + 4; ++i) {
        clients.push_back(UdpTransport::open("127.0.0.1:0", err));
        REQUIRE_MESSAGE(clients.back(), err);
        const PeerId h = clients.back()->add_peer(host->local_address());
        clients.back()->send(h, bytes("hi"));
    }
    std::size_t got = 0;
    Packet p;
    while (got < UdpTransport::max_peers && wait_recv(*host, p)) ++got;
    CHECK(got == UdpTransport::max_peers);
    // The surplus senders are dropped, not queued or registered.
    for (int i = 0; i < 50; ++i) {
        if (host->recv(p)) FAIL("a sender beyond max_peers was accepted");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
