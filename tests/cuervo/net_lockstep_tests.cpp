#include <doctest.h>

#include <memory>
#include <vector>

#include "cuervo/core/rng.hpp"
#include "cuervo/net/lockstep.hpp"
#include "cuervo/net/loopback.hpp"

using namespace cuervo;
using namespace cuervo::net;

namespace {

std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    return x ^ (x >> 33);
}

// Stand-in for a ScriptHost: state is a pure function of the input frames, and
// the hash plays the role of snapshot_hash().
struct ToySim {
    std::uint64_t state = 1;
    void step(const std::vector<InputFrame>& frames) {
        for (auto& f : frames) state = mix(state ^ f.held ^ (static_cast<std::uint64_t>(f.axes[0]) << 40));
    }
};

struct Peer {
    std::unique_ptr<LockstepSession> session;
    ToySim sim;
    Pcg32 input_rng;
    std::vector<std::uint64_t> hashes; // hashes[i] = after tick i + 1
    std::vector<std::vector<InputFrame>> delivered;
    std::uint32_t corrupt_at = 0; // test hook: perturb the sim once at this tick
};

struct Match {
    LoopbackNetwork net;
    std::vector<Peer> peers;
    Match(std::uint64_t seed, LinkConfig cfg, std::uint32_t players, std::uint32_t delay) : net(seed, cfg) {
        peers.resize(players);
        for (std::uint32_t i = 0; i < players; ++i) {
            auto& ep = net.add_endpoint();
            peers[i].session = std::make_unique<LockstepSession>(ep, players, delay);
            peers[i].input_rng = Pcg32(100 + i);
        }
    }
    void run(int steps) {
        for (int s = 0; s < steps; ++s) {
            net.step();
            for (auto& p : peers) {
                p.session->pump();
                if (p.session->can_submit()) {
                    InputFrame f;
                    f.held = p.input_rng.next_u32();
                    f.axes[0] = static_cast<std::int16_t>(p.input_rng.next_below(2000)) - 1000;
                    p.session->submit_local(f);
                }
                std::vector<InputFrame> frames;
                if (p.session->advance(frames)) {
                    p.sim.step(frames);
                    if (p.corrupt_at == p.session->sim_tick()) p.sim.state ^= 1;
                    p.hashes.push_back(p.sim.state);
                    p.delivered.push_back(frames);
                    p.session->report_hash(p.sim.state);
                }
            }
        }
    }
};

void check_identical(const Match& m, std::size_t min_ticks) {
    std::size_t n = m.peers[0].hashes.size();
    CHECK(n >= min_ticks);
    for (auto& p : m.peers) n = std::min(n, p.hashes.size());
    for (std::size_t i = 0; i < n; ++i)
        for (auto& p : m.peers)
            if (p.hashes[i] != m.peers[0].hashes[i]) {
                FAIL("hash mismatch at tick " << i + 1);
                return;
            }
}

} // namespace

TEST_CASE("lockstep: clean link keeps two peers identical and never stalls past the delay") {
    Match m(1, {}, 2, 2);
    m.run(3000);
    check_identical(m, 2900);
    CHECK_FALSE(m.peers[0].session->desync());
    CHECK_FALSE(m.peers[1].session->desync());
}

TEST_CASE("lockstep: lossy, jittery, duplicating link stays hash-identical for thousands of ticks") {
    LinkConfig cfg{.latency_min = 2, .jitter = 4, .loss_permille = 250, .dup_permille = 100};
    Match m(42, cfg, 2, 4);
    m.run(8000);
    check_identical(m, 4000);
    CHECK(m.net.dropped() > 1000);
    CHECK_FALSE(m.peers[0].session->desync());
    CHECK_FALSE(m.peers[1].session->desync());
}

TEST_CASE("lockstep: three players over a bad link") {
    LinkConfig cfg{.latency_min = 1, .jitter = 3, .loss_permille = 150, .dup_permille = 50};
    Match m(9, cfg, 3, 4);
    m.run(4000);
    check_identical(m, 2000);
    CHECK_FALSE(m.peers[2].session->desync());
}

TEST_CASE("lockstep: a run is a pure function of the seed") {
    LinkConfig cfg{.latency_min = 1, .jitter = 5, .loss_permille = 300};
    Match a(5, cfg, 2, 3), b(5, cfg, 2, 3);
    a.run(1500);
    b.run(1500);
    CHECK(a.peers[0].hashes == b.peers[0].hashes);
}

TEST_CASE("lockstep: delivered frames replay offline to the same hashes") {
    LinkConfig cfg{.latency_min = 2, .jitter = 2, .loss_permille = 200};
    Match m(3, cfg, 2, 3);
    m.run(1500);
    ToySim offline;
    auto& p = m.peers[1];
    REQUIRE(p.delivered.size() > 500);
    std::size_t n = std::min(p.delivered.size(), m.peers[0].hashes.size());
    for (std::size_t i = 0; i < n; ++i) {
        offline.step(p.delivered[i]);
        REQUIRE(offline.state == m.peers[0].hashes[i]);
    }
}

TEST_CASE("lockstep: injected divergence is detected at its first tick on both peers") {
    LinkConfig cfg{.latency_min = 1, .jitter = 3, .loss_permille = 200};
    Match m(11, cfg, 2, 3);
    m.peers[1].corrupt_at = 700;
    m.run(2000);
    for (auto& p : m.peers) {
        REQUIRE(p.session->desync());
        CHECK(p.session->desync()->tick == 700);
        CHECK(p.session->desync()->local_hash != p.session->desync()->remote_hash);
    }
}

TEST_CASE("lockstep: a silent peer stalls the other instead of letting it run ahead") {
    Match m(1, {}, 2, 2);
    for (int s = 0; s < 100; ++s) {
        m.net.step();
        auto& p = m.peers[0];
        p.session->pump();
        if (p.session->can_submit()) p.session->submit_local(InputFrame{});
        std::vector<InputFrame> frames;
        while (p.session->advance(frames)) p.session->report_hash(0);
    }
    // Only the pre-filled delay ticks can run without the peer's input.
    CHECK(m.peers[0].session->sim_tick() == 2);
}

TEST_CASE("lockstep: malformed packets are ignored") {
    LoopbackNetwork net(1);
    auto& a = net.add_endpoint();
    auto& b = net.add_endpoint();
    LockstepSession s(b, 2, 2);
    Pcg32 rng(77);
    for (int i = 0; i < 2000; ++i) {
        std::vector<std::byte> junk(rng.next_below(300));
        for (auto& x : junk) x = static_cast<std::byte>(rng.next_u32());
        if (junk.size() > 2 && (i & 1)) {
            junk[0] = std::byte{0x43};
            junk[1] = std::byte{0x4c};
        }
        a.send(1, junk);
        s.pump();
    }
    CHECK(s.sim_tick() == 0);
    CHECK_FALSE(s.desync());
}
