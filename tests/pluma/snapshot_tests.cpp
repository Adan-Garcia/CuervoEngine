// VM snapshots (pluma/snapshot.hpp, DESIGN.md 4.8, milestone 11.7a): restoring an image makes
// the program continue exactly as the uninterrupted run did.

#include <doctest.h>

#include <string>
#include <vector>

#include "pluma/heap.hpp"
#include "pluma/session.hpp"
#include "pluma/snapshot.hpp"
#include "pluma/vm.hpp"

namespace {

const char* program = R"(
class N {
    var n = 0
    var other: N
    weak var w: N
    var tag = "t"
    var links = Map<str, N>()
}
var nodes = List<N>()
var owners = Map<N, str>()
var seen = Set<str>()
var dq = Deque<int>()
var pq = PriorityQueue<int>((a, b) => a < b)
var total = 0
var label = "x"
var pos = vec2(1.0, 2.0)
var big = vec3(1.0, 2.0, 3.0)
var keep: N

fn work(id: int) {
    var cnt = 0
    while true {
        var k = N()
        k.n = cnt * id
        k.other = k
        k.links["self"] = k
        if nodes.len() > 0 {
            k.w = nodes[nodes.len() - 1]
        }
        nodes.push(k)
        owners[k] = "n{cnt}"
        seen.add("s{cnt % 5}")
        dq.push_back(cnt)
        pq.push(100 - cnt)
        if dq.len() > 4 {
            total += dq.pop_front()
        }
        if nodes.len() > 6 {
            nodes = nodes.slice(1)
        }
        if cnt % 7 == 0 {
            owners.clear()
        }
        pos = pos + vec2(0.5, 0.25)
        label = label + "y"
        var captured = cnt
        var f = fn() -> int { return captured + id }
        total += f()
        keep = k
        await wait_ticks(1 + cnt % 3)
        print("{cnt}:{total}:{nodes.len()}:{owners.len()}:{k.w == null}:{pos.x}:{label.len()}:{seen.len()}:{pq.len()}")
        cnt += 1
    }
}
start(work(1))
start(work(2))
)";

struct Rig {
    std::string out;
    pluma::Session session;
    Rig() : session(make_options(out)) {
        pluma::SessionResult r = session.run_script("t.plm", program);
        REQUIRE_MESSAGE(r.ok, r.rendered);
    }
    static pluma::VmOptions make_options(std::string& out) {
        pluma::VmOptions o;
        o.out = [&out](std::string_view s) { out += s; };
        return o;
    }
    void run(int ticks) {
        for (int i = 0; i < ticks; ++i) {
            pluma::RunResult r = session.vm().tick_coroutines();
            REQUIRE(r.ok);
            session.vm().collect_cycles(200);
        }
    }
    std::vector<std::uint8_t> save() {
        pluma::SnapshotWriter w(session.vm());
        w.vm_state();
        std::vector<std::uint8_t> image;
        const bool ok = w.finish(image);
        REQUIRE_MESSAGE(ok, w.error());
        return image;
    }
    void restore(const std::vector<std::uint8_t>& image) {
        pluma::snapshot_reset(session.vm());
        pluma::SnapshotReader r(session.vm(), image);
        REQUIRE_MESSAGE(r.ok(), r.error());
        REQUIRE_MESSAGE(r.vm_state(), r.error());
        REQUIRE_MESSAGE(r.finish(), r.error());
    }
};

} // namespace

TEST_CASE("snapshot: restoring continues exactly like the uninterrupted run") {
    Rig a;
    a.run(23);
    const std::vector<std::uint8_t> image = a.save();
    CHECK(a.save() == image); // taking a snapshot changes nothing
    a.out.clear();
    a.run(40);
    const std::string expected = a.out;
    const std::vector<std::uint8_t> end_image = a.save();
    REQUIRE(!expected.empty());

    // Back in the same VM.
    a.restore(image);
    CHECK(a.save() == image);
    a.out.clear();
    a.run(40);
    CHECK(a.out == expected);
    CHECK(a.save() == end_image);

    // And into a fresh VM running the same code.
    Rig b;
    b.run(5);
    b.restore(image);
    CHECK(b.save() == image);
    b.out.clear();
    b.run(40);
    CHECK(b.out == expected);
    CHECK(b.save() == end_image);
}

TEST_CASE("snapshot: restoring does not leak objects") {
    Rig a;
    a.run(30);
    const auto image = a.save();
    a.run(10);
    for (int i = 0; i < 3; ++i) a.restore(image);
    const std::size_t live = pluma::live_objects();
    for (int i = 0; i < 3; ++i) a.restore(image);
    CHECK(pluma::live_objects() == live);
}

TEST_CASE("snapshot: code that changed is refused") {
    Rig a;
    a.run(3);
    const auto image = a.save();
    std::string out;
    pluma::Session other(Rig::make_options(out));
    REQUIRE(other.run_script("t.plm", "var x = 1\nfn f() {}\n").ok);
    pluma::snapshot_reset(other.vm());
    pluma::SnapshotReader r(other.vm(), image);
    CHECK((!r.ok() || !r.vm_state()));
}

TEST_CASE("snapshot: arena objects and stale references survive a restore") {
    std::string out;
    pluma::Session s(Rig::make_options(out));
    REQUIRE(s.run_script("a.plm", R"(
class Tmp { var v = 1 }
var kept: Tmp = null
class Holder { var t: Tmp }
var h = Holder()
h.t = new(scene) Tmp()
h.t.v = 5
fn leak() -> Tmp { return new(frame) Tmp() }
kept = leak()
)").ok);
    s.vm().tick_coroutines();
    s.vm().reset_frame_arena(); // `kept` is stale now
    pluma::SnapshotWriter w(s.vm());
    w.vm_state();
    std::vector<std::uint8_t> image;
    REQUIRE_MESSAGE(w.finish(image), w.error());
    s.vm().reset_scene_arena();
    CHECK(s.vm().arena_objects(pluma::ArenaKind::Scene) == 0);
    pluma::snapshot_reset(s.vm());
    pluma::SnapshotReader r(s.vm(), image);
    REQUIRE_MESSAGE(r.ok(), r.error());
    REQUIRE_MESSAGE(r.vm_state(), r.error());
    REQUIRE_MESSAGE(r.finish(), r.error());
    CHECK(s.vm().arena_objects(pluma::ArenaKind::Scene) == 1);
    CHECK(s.run_script("c.plm", "print(h.t.v)").ok);
    CHECK(out == "5\n");
    CHECK_FALSE(s.run_script("d.plm", "print(kept.v)").ok);
}
