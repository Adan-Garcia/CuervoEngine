#include <doctest.h>

#include <cstring>
#include <string>

#include "cuervo/net/loopback.hpp"

using namespace cuervo::net;

namespace {
std::vector<std::byte> msg(const std::string& s) {
    std::vector<std::byte> v(s.size());
    std::memcpy(v.data(), s.data(), s.size());
    return v;
}
// Runs a fixed send pattern and returns the delivery log as a string.
std::string run_log(std::uint64_t seed, LinkConfig cfg) {
    LoopbackNetwork net(seed, cfg);
    auto& a = net.add_endpoint();
    auto& b = net.add_endpoint();
    std::string log;
    for (int i = 0; i < 200; ++i) {
        a.send(b.local_id(), msg(std::to_string(i) + ","));
        net.step();
        Packet p;
        while (b.recv(p)) log.append(reinterpret_cast<const char*>(p.data.data()), p.data.size());
    }
    return log;
}
} // namespace

TEST_CASE("loopback: zero latency delivers in order, ids and from are set") {
    LoopbackNetwork net(1);
    auto& a = net.add_endpoint();
    auto& b = net.add_endpoint();
    CHECK(a.local_id() == 0);
    CHECK(b.local_id() == 1);
    a.send(1, msg("x"));
    a.send(1, msg("y"));
    Packet p;
    REQUIRE(b.recv(p));
    CHECK(p.from == 0);
    CHECK(p.data.size() == 1);
    REQUIRE(b.recv(p));
    CHECK_FALSE(b.recv(p));
}

TEST_CASE("loopback: latency holds packets until their step") {
    LoopbackNetwork net(1, {.latency_min = 3});
    auto& a = net.add_endpoint();
    auto& b = net.add_endpoint();
    a.send(1, msg("x"));
    Packet p;
    for (int i = 0; i < 3; ++i) {
        CHECK_FALSE(b.recv(p));
        net.step();
    }
    CHECK(b.recv(p));
}

TEST_CASE("loopback: refuses oversize packets and unknown peers") {
    LoopbackNetwork net(1);
    auto& a = net.add_endpoint();
    CHECK_FALSE(a.send(9, msg("x")));
    CHECK_FALSE(a.send(0, std::vector<std::byte>(Transport::max_packet_size + 1)));
    CHECK(a.send(0, std::vector<std::byte>(Transport::max_packet_size)));
}

TEST_CASE("loopback: loss, reorder and duplicates are seeded and deterministic") {
    LinkConfig cfg{.latency_min = 1, .jitter = 5, .loss_permille = 200, .dup_permille = 100};
    auto one = run_log(7, cfg);
    CHECK(one == run_log(7, cfg));
    CHECK(one != run_log(8, cfg));
    LoopbackNetwork net(7, cfg);
    auto& a = net.add_endpoint();
    net.add_endpoint();
    for (int i = 0; i < 1000; ++i) a.send(1, msg("p"));
    CHECK(net.dropped() > 100);
    CHECK(net.dropped() < 300);
}
