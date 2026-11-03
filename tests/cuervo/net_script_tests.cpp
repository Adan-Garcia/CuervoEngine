#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cuervo/input/input_map.hpp"
#include "cuervo/net/loopback.hpp"
#include "cuervo/net/websocket.hpp"
#include "cuervo/script/script_host.hpp"
#include "net_http_fixture.hpp"
#include "net_relay_fixture.hpp"

using namespace cuervo;
using namespace cuervo::net;

namespace {

// The sim reads only per-player inputs: Net.player()/connected() differ between machines, so (as the
// Net docs say) they must not feed simulation state.
const char* game = R"(
var total = 0
class Main extends Node {
    var n = 0
    fn update(dt: float) {
        if Input.held(#a0) { n += 1 }
        if Input.held_by(1, #a0) { n += 10 }
        if Input.held_by(1, #a1) { n -= 3 }
        total += n + Random.range_int(0, 7)
    }
}
)";

InputFrame input_for(std::uint32_t player, std::uint32_t tick) {
    InputFrame f;
    f.held = ((tick / 3 + player) % 3 == 0) ? 0 : (tick + player) % 4;
    return f;
}

struct Peer {
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;
    std::vector<std::uint64_t> hashes;
    std::uint32_t submitted = 0;
    explicit Peer(const char* tag) {
        map.set_action(Name("a0"), {Binding::key(Key::A)});
        map.set_action(Name("a1"), {Binding::key(Key::B)});
        script::HostOptions o;
        o.project_dir = std::filesystem::temp_directory_path() / (std::string("cuervo_net_script_") + tag);
        std::filesystem::create_directories(o.project_dir);
        o.seed = 5;
        host = std::make_unique<script::ScriptHost>(o, map);
        REQUIRE_MESSAGE(host->load({{"main.plm", game}}, "Main"), host->error());
        host->tick(InputFrame{}); // warm-up: two hosts share one process (see net_rollback_tests)
    }
    // One host step. The local input is a pure function of (player, tick).
    void step(std::uint32_t player) {
        if (host->net_tick(input_for(player, host->tick_count() + 1))) hashes.push_back(host->snapshot_hash());
    }
};

void run_pair(LoopbackNetwork& net, Peer& a, Peer& b, int steps) {
    for (int s = 0; s < steps; ++s) {
        net.step();
        a.step(0);
        b.step(1);
    }
}

void check_same(const Peer& a, const Peer& b, std::size_t min_ticks) {
    const std::size_t n = std::min(a.hashes.size(), b.hashes.size());
    CHECK(n >= min_ticks);
    for (std::size_t i = 0; i < n; ++i)
        if (a.hashes[i] != b.hashes[i]) {
            FAIL("hash mismatch at tick " << i + 1);
            return;
        }
}

} // namespace

TEST_CASE("net script: without a session net_tick does nothing && Net reports offline") {
    Peer a("offline");
    CHECK_FALSE(a.host->net_tick(InputFrame{}));
    CHECK_FALSE(a.host->net_context().active());
    CHECK(a.host->net_context().player() == 0);
    CHECK_FALSE(a.host->net_context().connected());
}

TEST_CASE("net script: Net natives are callable from Pluma") {
    InputMap map;
    script::HostOptions o;
    o.project_dir = std::filesystem::temp_directory_path() / "cuervo_net_script_natives";
    std::filesystem::create_directories(o.project_dir);
    script::ScriptHost host(o, map);
    const char* src = R"(
var ok = false
class Main extends Node {
    fn ready() {
        Net.set_mode(#rollback)
        Net.set_input_delay(4)
        if Net.player() == 0 && Net.players() == 1 && !Net.connected() && !Net.desynced() && Net.is_rollback() {
            ok = Net.join("not an address") == false
        }
        Net.stop()
    }
}
)";
    REQUIRE_MESSAGE(host.load({{"main.plm", src}}, "Main"), host.error());
    CHECK(host.net_context().mode() == script::NetContext::Mode::Rollback);
    CHECK_FALSE(host.net_context().active());
}

TEST_CASE("net script: a UDP mesh refuses bad address lists and indices") {
    script::NetContext ctx;
    std::string error;
    CHECK_FALSE(ctx.mesh("127.0.0.1:7001", 0, error));                 // one player is not a match
    CHECK_FALSE(ctx.mesh("127.0.0.1:7001,127.0.0.1:7002", 2, error));  // we are not in the list
    CHECK_FALSE(ctx.mesh("127.0.0.1:7001,nonsense", 0, error));        // cannot resolve
    CHECK_FALSE(ctx.active());
}

TEST_CASE("net script: lockstep over a bad loopback link keeps both hosts identical") {
    LoopbackNetwork net(3, {.latency_min = 1, .jitter = 3, .loss_permille = 150});
    Peer a("ls_a"), b("ls_b");
    a.host->net_context().attach(net.add_endpoint(), 2);
    b.host->net_context().attach(net.add_endpoint(), 2);
    run_pair(net, a, b, 3000);
    check_same(a, b, 800);
    CHECK_FALSE(a.host->net_context().desynced());
    CHECK(a.host->net_context().connected());
    CHECK(a.host->net_context().peers() == 1);
    CHECK(b.host->net_context().player() == 1);
    // Scripts see the same facts through Net.*: the peer-joined event fired once.
    CHECK(a.host->net_context().consume_peer_joined());
    CHECK_FALSE(a.host->net_context().consume_peer_joined());
}

TEST_CASE("net script: rollback mode through ScriptHost snapshots converges") {
    LoopbackNetwork net(9, {.latency_min = 2, .jitter = 3, .loss_permille = 100});
    Peer a("rb_a"), b("rb_b");
    for (Peer* p : {&a, &b}) {
        p->host->net_context().set_mode(script::NetContext::Mode::Rollback);
        p->host->net_context().set_max_rollback(8);
    }
    a.host->net_context().attach(net.add_endpoint(), 2);
    b.host->net_context().attach(net.add_endpoint(), 2);
    run_pair(net, a, b, 2000);
    REQUIRE(a.host->net_context().rollback() != nullptr);
    CHECK(a.host->net_context().rollback()->rollbacks() > 0);
    CHECK_FALSE(a.host->net_context().desynced());
    CHECK_FALSE(b.host->net_context().desynced());
    // Rollback hosts tick ahead of confirmation: compare the confirmed prefix via the sessions.
    const auto* ra = a.host->net_context().rollback();
    const auto* rb = b.host->net_context().rollback();
    const std::uint32_t n = std::min(ra->confirmed_tick(), rb->confirmed_tick());
    CHECK(n > 500);
    for (std::uint32_t t = 1; t + 80 < n; ++t) {
        const auto ha = ra->hash_at(t), hb = rb->hash_at(t);
        if (ha && hb && *ha != *hb) FAIL("confirmed hash mismatch at tick " << t);
    }
}

TEST_CASE("net script: relay session over WebSocket stays in sync") {
#if defined(_WIN32)
    return;
#else
    Relay relay;
    if (!relay.ok()) {
        MESSAGE("python3 relay unavailable; skipped");
        return;
    }
    const std::string url = "ws://127.0.0.1:" + std::to_string(relay.port);
    Peer a("ws_a"), b("ws_b");
    std::string err;
    REQUIRE_MESSAGE(a.host->net_context().relay(url, "duel", 2, err), err);
    REQUIRE_MESSAGE(b.host->net_context().relay(url, "duel", 2, err), err);
    CHECK(a.host->net_context().player() == 0);
    CHECK(b.host->net_context().player() == 1);
    for (int s = 0; s < 6000 && (a.hashes.size() < 200 || b.hashes.size() < 200); ++s) {
        a.step(0);
        b.step(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // real sockets: let the relay run
    }
    check_same(a, b, 200);
    CHECK_FALSE(a.host->net_context().desynced());
#endif
}

TEST_CASE("net script: Http natives refuse bad input and read unknown handles as empty") {
    InputMap map;
    script::HostOptions o;
    o.project_dir = std::filesystem::temp_directory_path() / "cuervo_net_script_http_natives";
    std::filesystem::create_directories(o.project_dir);
    script::ScriptHost host(o, map);
    const char* src = R"(
var ok = false
class Main extends Node {
    fn ready() {
        Http.set_timeout(500)
        var bad_url = Http.get("ftp://example.com/")
        var bad_header = Http.get("http://127.0.0.1:1/", "no colon here")
        ok = bad_url == 0 && bad_header == 0 && !Http.done(99) && Http.status(99) == 0 && Http.body(99) == ""
        Http.close(99)
    }
}
)";
    REQUIRE_MESSAGE(host.load({{"main.plm", src}}, "Main"), host.error());
    CHECK(host.net_context().http_timeout_ms == 500);
    CHECK_FALSE(host.net_context().http_find(1));
}

#if !defined(_WIN32)
TEST_CASE("net script: Http.get is polled from update and never enters the sim by itself") {
    HttpServer server;
    if (!server.ok()) {
        MESSAGE("python3 unavailable; skipped");
        return;
    }
    InputMap map;
    script::HostOptions o;
    o.project_dir = std::filesystem::temp_directory_path() / "cuervo_net_script_http_poll";
    std::filesystem::create_directories(o.project_dir);
    script::ScriptHost host(o, map);
    const std::string src = std::string(R"(
var h = 0
var got = 0
class Main extends Node {
    fn ready() { h = Http.post(")") + server.url("/echo") + R"(", "ping", "X-Token: t1\nContent-Type: text/plain") }
    fn update(dt: float) {
        if h != 0 && Http.done(h) {
            if Http.status(h) == 200 && Http.body(h) == "ping" && Http.header(h, "X-Seen-X-Token") == "t1" { got = 1 }
            Http.close(h)
            h = 0
        }
    }
}
)";
    REQUIRE_MESSAGE(host.load({{"main.plm", src}}, "Main"), host.error());
    REQUIRE(host.net_context().http_find(1)); // started in ready(); closed by update() when done
    std::uint64_t first_hash = 0;
    for (int i = 0; i < 2000 && host.net_context().http_find(1); ++i) {
        host.tick(InputFrame{});
        if (i == 0) first_hash = host.state_hash();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK_FALSE(host.net_context().http_find(1)); // update() saw done() and closed the handle
    CHECK(first_hash != 0);
}
#endif

// Regression: a flaky desync that only the snapshot hash saw. Garbage made between ticks (a draw frame;
// how many run per tick depends on the machine's speed) changes how much work the cycle collector does in
// the next tick, and so the credit it leaves behind. That is not game state, and must not reach the snapshot.
TEST_CASE("net script: garbage made between ticks does not change the snapshot hash") {
    const char* churn_game = R"(
class Cell { var other: Cell? = null }
fn churn(n: int) {
    var i = 0
    while i < n {
        var a = Cell()
        var b = Cell()
        a.other = b
        b.other = a
        i += 1
    }
}
var total = 0
class Main extends Node {
    fn update(dt: float) {
        churn(3)
        total += 1
    }
    fn draw_like(n: int) { churn(n) }
}
)";
    InputMap map;
    map.set_action(Name("a0"), {Binding::key(Key::A)});
    std::vector<std::unique_ptr<script::ScriptHost>> hosts;
    for (const char* tag : {"churn_a", "churn_b"}) {
        script::HostOptions o;
        o.project_dir = std::filesystem::temp_directory_path() / (std::string("cuervo_net_script_") + tag);
        std::filesystem::create_directories(o.project_dir);
        o.seed = 5;
        hosts.push_back(std::make_unique<script::ScriptHost>(o, map));
        REQUIRE_MESSAGE(hosts.back()->load({{"main.plm", churn_game}}, "Main"), hosts.back()->error());
    }
    for (int t = 0; t < 40; ++t) {
        // Host 1 "draws" between ticks: extra garbage, as a frame that ran no tick.
        if (t > 0 && t % 2 == 0) {
            const pluma::Value n = pluma::Value::integer(pluma::NumKind::I32, 5 + t);
            REQUIRE(hosts[1]->session()->vm().call_method(hosts[1]->root(), "draw_like", std::span(&n, 1)).ok);
        }
        for (auto& h : hosts) h->tick(InputFrame{});
        CHECK(hosts[0]->snapshot_hash() == hosts[1]->snapshot_hash());
    }
}

// Regression: the same, for a debt. With a tiny budget the cycle collector overruns it whenever a lot of
// garbage is waiting (a slow machine's draw frames), and the debt it carries must not reach the snapshot.
TEST_CASE("net script: a collector in debt from between-tick garbage does not change the snapshot hash") {
    const char* churn_game = R"(
class Cell { var other: Cell? = null }
fn churn(n: int) {
    var i = 0
    while i < n {
        var a = Cell()
        var b = Cell()
        a.other = b
        b.other = a
        i += 1
    }
}
var total = 0
class Main extends Node {
    fn update(dt: float) {
        churn(2)
        total += 1
    }
    fn draw_like(n: int) { churn(n) }
}
)";
    InputMap map;
    map.set_action(Name("a0"), {Binding::key(Key::A)});
    std::vector<std::unique_ptr<script::ScriptHost>> hosts;
    for (const char* tag : {"debt_a", "debt_b"}) {
        script::HostOptions o;
        o.project_dir = std::filesystem::temp_directory_path() / (std::string("cuervo_net_script_") + tag);
        std::filesystem::create_directories(o.project_dir);
        o.seed = 5;
        o.gc_budget = 40;
        hosts.push_back(std::make_unique<script::ScriptHost>(o, map));
        REQUIRE_MESSAGE(hosts.back()->load({{"main.plm", churn_game}}, "Main"), hosts.back()->error());
    }
    for (int t = 0; t < 40; ++t) {
        if (t > 0 && t % 3 == 0) {
            const pluma::Value n = pluma::Value::integer(pluma::NumKind::I32, 400 + 10 * t);
            REQUIRE(hosts[1]->session()->vm().call_method(hosts[1]->root(), "draw_like", std::span(&n, 1)).ok);
        }
        for (auto& h : hosts) h->tick(InputFrame{});
        CHECK(hosts[0]->snapshot_hash() == hosts[1]->snapshot_hash());
    }
}
