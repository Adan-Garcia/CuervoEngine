#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "cuervo/input/input_map.hpp"
#include "cuervo/net/lockstep.hpp"
#include "cuervo/net/loopback.hpp"
#include "cuervo/net/rollback.hpp"
#include "cuervo/script/script_host.hpp"

using namespace cuervo;
using namespace cuervo::net;

namespace {

const char* game = R"(
var total = 0
var log = ""
class Main extends Node {
    var n = 0
    fn update(dt: float) {
        if Input.held(#a0) { n += 1 }
        if Input.held(#a1) { n += 2 }
        if Input.held(#a2) { n += 3 }
        if Input.held(#a3) { n += 4 }
        if Input.held_by(1, #a0) { n -= 1 }
        if Input.held_by(1, #a1) { n -= 2 }
        if Input.held_by(1, #a2) { n -= 5 }
        if Input.held_by(1, #a3) { n += 7 }
        if Input.pressed_by(1, #a0) { n += 100 }
        total += n + Random.range_int(0, 7)
        log = "{log}{total % 10}"
        if log.len() > 30 { log = "" }
    }
}
)";

// Both players' frames go to the host as they are: player 0 is Input.*, player 1 Input.*_by(1, ...).
// A player's input is a pure function of (player, tick): it changes every 5th
// tick, like a held key, so repeat-last prediction is often right but not
// always.
InputFrame input_for(std::uint32_t player, std::uint32_t tick) {
    std::uint64_t x = (static_cast<std::uint64_t>(player) << 32) | ((tick - 1) / 5);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    InputFrame f;
    f.held = (x & 3) == 0 ? 0 : (x >> 8) & 15;
    return f;
}

struct Sim {
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;
    std::vector<script::SimSnapshot> slots;
    std::uint32_t diverge_at = 0; // test hook: hash differs from this tick on
    explicit Sim(std::uint32_t nslots) : slots(nslots) {
        const char* names[4] = {"a0", "a1", "a2", "a3"};
        for (int i = 0; i < 4; ++i)
            map.set_action(Name(names[i]), {Binding::key(static_cast<Key>(static_cast<int>(Key::A) + i))});
        script::HostOptions o;
        o.project_dir = std::filesystem::temp_directory_path() / "cuervo_net_rollback_tests";
        std::filesystem::create_directories(o.project_dir);
        o.seed = 77;
        host = std::make_unique<script::ScriptHost>(o, map);
        REQUIRE_MESSAGE(host->load({{"main.plm", game}}, "Main"), host->error());
        // Two hosts in one process share the cycle collector's suspect buffer, so the first tick
        // of one collects the other's start-up suspects and their images differ until both have
        // ticked once. A warm-up tick on each makes the peers' starting images alike (a real
        // process holds one host; saving at tick 0 itself works, see the tests at the end).
        host->tick(InputFrame{});
    }
    std::uint64_t hash() const {
        std::uint64_t h = host->snapshot_hash();
        return diverge_at && host->tick_count() > diverge_at ? h ^ 1 : h;
    }
};

struct RbHost final : RollbackHost {
    Sim sim;
    explicit RbHost(std::uint32_t window) : sim(window + 1) {}
    void step(std::span<const InputFrame> f) override { sim.host->tick_players(f); }
    void save(std::uint32_t slot, std::uint32_t tick) override {
        std::string err;
        REQUIRE_MESSAGE(sim.host->save(sim.slots[slot], err), "tick " << tick << ": " << err);
    }
    void restore(std::uint32_t slot, std::uint32_t) override {
        std::string err;
        REQUIRE_MESSAGE(sim.host->restore(sim.slots[slot], err), err);
    }
    std::uint64_t hash() override { return sim.hash(); }
};

struct Peer {
    std::unique_ptr<RbHost> host;
    std::unique_ptr<RollbackSession> session;
    std::vector<std::uint64_t> confirmed; // confirmed[i] = hash after tick i + 1
};

struct Match {
    LoopbackNetwork net;
    std::vector<Peer> peers;
    Match(std::uint64_t seed, LinkConfig cfg, std::uint32_t window) : net(seed, cfg) {
        peers.resize(2);
        for (std::uint32_t i = 0; i < 2; ++i) {
            auto& ep = net.add_endpoint();
            peers[i].host = std::make_unique<RbHost>(window);
            peers[i].session = std::make_unique<RollbackSession>(ep, *peers[i].host, 2, window);
        }
    }
    void run(int steps) {
        for (int s = 0; s < steps; ++s) {
            net.step();
            for (std::uint32_t i = 0; i < 2; ++i) {
                Peer& p = peers[i];
                p.session->update(input_for(i, p.session->predicted_tick() + 1));
                while (p.confirmed.size() < p.session->confirmed_tick()) {
                    auto h = p.session->hash_at(static_cast<std::uint32_t>(p.confirmed.size()) + 1);
                    REQUIRE(h.has_value());
                    p.confirmed.push_back(*h);
                }
            }
        }
    }
};

void check_identical(const Match& m, std::size_t min_ticks) {
    std::size_t n = std::min(m.peers[0].confirmed.size(), m.peers[1].confirmed.size());
    CHECK(n >= min_ticks);
    for (std::size_t i = 0; i < n; ++i)
        if (m.peers[0].confirmed[i] != m.peers[1].confirmed[i]) {
            FAIL("confirmed hash mismatch at tick " << i + 1);
            return;
        }
}

} // namespace

TEST_CASE("rollback: real ScriptHost, clean link, peers converge") {
    Match m(1, {}, 6);
    m.run(1500);
    check_identical(m, 1400);
    for (auto& p : m.peers) CHECK_FALSE(p.session->desync());
}

TEST_CASE("rollback: lossy jittery link converges over thousands of ticks with bounded rollback") {
    LinkConfig cfg{.latency_min = 2, .jitter = 3, .loss_permille = 150, .dup_permille = 50};
    Match m(42, cfg, 8);
    m.run(4000);
    check_identical(m, 2500);
    for (auto& p : m.peers) {
        CHECK_FALSE(p.session->desync());
        CHECK(p.session->rollbacks() > 0);
        CHECK(p.session->deepest_rollback() <= 8);
        CHECK(p.session->resimulated_ticks() <= p.session->rollbacks() * 8);
        CHECK(p.session->rollbacks() < p.session->predicted_tick());
        CHECK(p.session->predicted_tick() - p.session->confirmed_tick() <= 8);
    }
}

TEST_CASE("rollback: confirmed hashes equal lockstep for the same inputs") {
    LinkConfig cfg{.latency_min = 1, .jitter = 2, .loss_permille = 100};
    Match m(7, cfg, 6);
    m.run(1500);
    std::size_t n = std::min(m.peers[0].confirmed.size(), m.peers[1].confirmed.size());
    REQUIRE(n >= 500);

    LoopbackNetwork net(7, cfg);
    Sim sim(1);
    std::vector<std::uint64_t> locked;
    LockstepSession a(net.add_endpoint(), 2, 0), b(net.add_endpoint(), 2, 0); // only a drives the sim
    std::uint32_t sub_a = 0, sub_b = 0;
    std::vector<InputFrame> fa, fb;
    for (int s = 0; s < 20000 && locked.size() < n; ++s) {
        net.step();
        a.pump();
        b.pump();
        if (a.can_submit()) a.submit_local(input_for(0, ++sub_a));
        if (b.can_submit()) b.submit_local(input_for(1, ++sub_b));
        b.advance(fb);
        if (a.advance(fa)) {
            sim.host->tick_players(fa);
            locked.push_back(sim.hash());
            a.report_hash(locked.back());
        }
    }
    REQUIRE(locked.size() >= n);
    for (std::size_t i = 0; i < n; ++i) {
        if (locked[i] != m.peers[0].confirmed[i]) {
            FAIL("rollback differs from lockstep at tick " << i + 1);
            return;
        }
    }
}

TEST_CASE("rollback: an injected divergence is detected at its first tick") {
    LinkConfig cfg{.latency_min = 1, .jitter = 2, .loss_permille = 100};
    Match m(11, cfg, 6);
    m.peers[1].host->sim.diverge_at = 300;
    m.run(1200);
    for (auto& p : m.peers) {
        REQUIRE(p.session->desync().has_value());
        CHECK(p.session->desync()->tick == 300);
    }
}

TEST_CASE("rollback: a run is a pure function of the seed") {
    LinkConfig cfg{.latency_min = 1, .jitter = 4, .loss_permille = 250};
    Match a(5, cfg, 8), b(5, cfg, 8);
    a.run(1200);
    b.run(1200);
    CHECK(a.peers[0].confirmed == b.peers[0].confirmed);
    CHECK(a.peers[0].session->rollbacks() == b.peers[0].session->rollbacks());
}

TEST_CASE("ScriptHost::save works before the first tick and restores to the same run") {
    InputMap map;
    script::HostOptions o;
    o.project_dir = std::filesystem::temp_directory_path() / "cuervo_net_rollback_tests";
    std::filesystem::create_directories(o.project_dir);
    o.seed = 5;
    script::ScriptHost host(o, map);
    REQUIRE_MESSAGE(host.load({{"main.plm", game}}, "Main"), host.error());
    script::SimSnapshot snap;
    std::string err;
    REQUIRE_MESSAGE(host.save(snap, err), err);
    for (int i = 0; i < 20; ++i) host.tick(InputFrame{});
    const auto h = host.state_hash();
    REQUIRE_MESSAGE(host.restore(snap, err), err);
    CHECK(host.tick_count() == 0);
    for (int i = 0; i < 20; ++i) host.tick(InputFrame{});
    CHECK(host.state_hash() == h);
}

TEST_CASE("ScriptHost::save works at tick 0 while another host holds suspects in the shared heap") {
    Sim a(1), b(1);
    std::string err;
    CHECK_MESSAGE(a.host->save(a.slots[0], err), err);
    CHECK_MESSAGE(b.host->save(b.slots[0], err), err);
}
