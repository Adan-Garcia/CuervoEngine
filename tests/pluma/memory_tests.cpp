// Memory (DESIGN.md 5.10, LANGUAGE.md 2.11 / 9.14): the cycle collector,
// weak references, acyclic types, and manual memory (alloc / free,
// new(frame), new(scene), manual classes, checked references, unsafe,
// escape analysis).

#include <doctest.h>

#include <algorithm>
#include <memory>

#include "pluma/heap.hpp"
#include "pluma/session.hpp"
#include "pluma/value.hpp"
#include "run_helpers.hpp"

using test::panics;
using test::prints;
using test::rejects;

namespace {

// A REPL-like session: run() adds inputs; the VM, heap and arenas persist.
struct Env {
    std::string out;
    std::unique_ptr<pluma::Session> session;
    std::string last_panic;
    Env() {
        pluma::VmOptions o;
        o.out = [this](std::string_view s) { out += s; };
        session = std::make_unique<pluma::Session>(o);
    }
    bool run(const std::string& src) {
        pluma::SessionResult r = session->run_script("test.plm", src);
        INFO("source:\n" << src << "\n" << r.rendered);
        REQUIRE(r.compiled);
        last_panic = r.panic ? std::format("{}:{}: {}", r.panic->line, r.panic->column, r.panic->message) : "";
        return r.ok;
    }
    pluma::VM& vm() { return session->vm(); }
    // Collects everything that is garbage now.
    std::size_t collect() {
        pluma::CollectResult r = pluma::collect_all_cycles();
        return r.freed;
    }
};

} // namespace

// ---------------------------------------------------------------- collector

TEST_CASE("collector: reference cycles are collected, live data is kept") {
    Env e;
    e.run(R"(
class Pair { var other: Pair; var payload = List<int>() }
var keep: Pair = null
fn garbage(n: int) {
    for i in 0..n {
        var a = Pair()
        var b = Pair()
        a.other = b
        b.other = a
        a.payload.push(i)
    }
}
fn live() {
    var a = Pair()
    a.other = a
    keep = a
}
)");
    e.collect();
    const std::size_t base = pluma::live_objects();
    e.run("garbage(100)\nlive()");
    CHECK(pluma::live_objects() > base + 400); // 100 * (2 pairs + 2 lists) + the live one
    std::size_t freed = e.collect();
    CHECK(freed == 200); // the pairs; their lists (acyclic) die by refcount
    CHECK(pluma::live_objects() == base + 2); // `keep` and its list
    e.run("keep.other = null\nkeep = null"); // breaking the cycle frees it by refcount
    e.collect();
    CHECK(pluma::live_objects() == base);
}

TEST_CASE("collector: cycles through lists, closures, coroutines and signals") {
    Env e;
    e.run(R"(
class Node {
    var children = List<Node>()
    var parent: Node
    var on_tick: fn()
    var task: Task
    signal hit(n: int)
    var hits = 0
    fn run() -> Node {
        yield
        return self
    }
}
fn lists() {
    var root = Node()
    for i in 0..3 {
        var c = Node()
        c.parent = root
        root.children.push(c)
    }
}
fn closures() {
    var n = Node()
    n.on_tick = fn() { n.hits += 1 }          // n -> closure -> cell -> n
}
fn signals() {
    var n = Node()
    n.hit.connect(fn(k: int) { n.hits += k }) // n -> signal -> closure -> n
}
fn coroutines() {
    var n = Node()
    n.task = start(n.run())                    // finished: task.result = n
}
)");
    e.collect();
    const std::size_t base = pluma::live_objects();
    e.run("lists()\nclosures()\nsignals()");
    CHECK(pluma::live_objects() > base);
    e.collect();
    CHECK(pluma::live_objects() == base);

    e.run("coroutines()");
    // Suspended: the scheduler holds the coroutine, which holds the node.
    e.collect();
    CHECK(pluma::live_objects() > base);
    e.vm().tick_coroutines(); // finishes; the node <-> task cycle is garbage
    e.collect();
    CHECK(pluma::live_objects() == base);
}

TEST_CASE("collector: incremental steps with a work budget, mutation in between") {
    Env e;
    e.run(R"(
class Ring { var next: Ring; var n = 0 }
var saved: Ring = null
fn ring(len: int) -> Ring {
    var first = Ring()
    var cur = first
    for i in 1..len {
        var r = Ring()
        cur.next = r
        cur = r
    }
    cur.next = first
    return first
}
fn drop_rings(count: int, len: int) {
    for i in 0..count { ring(len) }
}
)");
    e.collect();
    const std::size_t base = pluma::live_objects();

    // A zero budget does nothing; the suspects wait.
    e.run("drop_rings(10, 10)");
    pluma::CollectResult r = e.vm().collect_cycles(0).ok ? pluma::CollectResult{} : pluma::CollectResult{};
    CHECK(pluma::live_objects() == base + 100);
    CHECK(pluma::heap_stats().suspects > 0);

    // One step processes the whole buffer as one candidate set, even past
    // its budget; the overshoot is paid back by the next steps.
    pluma::CollectResult step;
    REQUIRE(e.vm().collect_cycles(10, &step).ok);
    CHECK(step.sets == 1);
    CHECK(step.freed == 100);
    CHECK(step.work > 10);
    CHECK(pluma::live_objects() == base);
    e.run("drop_rings(1, 4)");
    pluma::CollectResult owed;
    e.vm().collect_cycles(10, &owed); // still in debt: nothing runs
    CHECK(owed.sets == 0);
    CHECK(pluma::live_objects() == base + 4);
    std::uint64_t debt = step.work - 10;
    for (std::uint64_t i = 0; i < debt / 10 + 1; ++i) e.vm().collect_cycles(10, &owed);
    CHECK(pluma::live_objects() == base);

    // A suspect revived between steps survives; dropped again, it dies.
    e.run("var r = ring(5)\nsaved = r\nr = null");
    e.collect();
    CHECK(pluma::live_objects() == base + 5);
    e.run("saved = null");
    e.collect();
    CHECK(pluma::live_objects() == base);

    // An object freed by refcount while it waits in the buffer is a shell
    // the next set deletes.
    e.run("var x = Ring()\nvar y = x\ny = null\nx = null");
    CHECK(pluma::live_objects() == base);
    e.collect();
    CHECK(pluma::live_objects() == base);
    (void)r;
}

TEST_CASE("collector: deterministic: the same program collects the same way") {
    auto trace = [] {
        Env e;
        e.run(R"(
class A { var b: B; var xs = List<A>() }
class B { var a: A }
fn churn(n: int) {
    for i in 0..n {
        var a = A()
        a.b = B()
        a.b.a = a
        if i % 3 == 0 { a.xs.push(a) }
    }
}
)");
        std::string t;
        for (int tick = 0; tick < 20; ++tick) {
            e.run("churn(7)");
            pluma::CollectResult r;
            e.vm().collect_cycles(50, &r);
            t += std::format("{}:{}:{} ", r.sets, r.freed, r.work);
        }
        return t;
    };
    std::string first = trace();
    CHECK(first == trace());
}

TEST_CASE("collector: a cycle-heavy script stays bounded over 10k ticks") {
    Env e;
    e.run(R"(
class Body {
    var parts = List<Body>()
    var owner: Body
    var on_hit: fn()
    var hp = 3
}
var world = List<Body>()
var ticks = 0
fn advance() {
    ticks += 1
    var b = Body()
    for i in 0..4 {
        var p = Body()
        p.owner = b
        b.parts.push(p)
    }
    b.on_hit = fn() { b.hp -= 1 }
    world.push(b)
    if world.len() > 16 { world.remove(0) }   // the removed body is cyclic garbage
}
)");
    e.collect();
    const std::size_t base = pluma::live_objects();
    std::size_t peak = 0;
    for (int t = 0; t < 10000; ++t) {
        e.run("advance()");
        REQUIRE(e.vm().collect_cycles(2000).ok);
        peak = std::max(peak, pluma::live_objects() - base);
    }
    // 16 live bodies of 5 nodes + lists + closures, plus at most a few
    // ticks of garbage waiting for its candidate set.
    CHECK(peak < 600);
    MESSAGE("peak live objects above base: " << peak << ", total collected " << pluma::heap_stats().collected);
}

// ---------------------------------------------------------------- acyclic types

TEST_CASE("acyclic: proven acyclic types are never suspects") {
    Env e;
    e.run(R"(
struct P { var x = 1 }
class Leaf { var name = "leaf"; var nums = List<int>(); var p = P {} }
class Holder { var leaves = List<Leaf>() }
var keep = Holder()
fn touch() {
    var l = Leaf()
    keep.leaves.push(l)
    var h = keep
    h = null
}
)");
    e.collect();
    pluma::HeapStats before = pluma::heap_stats();
    e.run("touch()\ntouch()");
    // Holder can't reach itself either: nothing at all became a suspect.
    CHECK(pluma::heap_stats().suspects == before.suspects);

    // A class that can reach itself is traced.
    e.run("class Cyc { var next: Cyc }\nvar c = Cyc()\nvar d = c\nd = null");
    CHECK(pluma::heap_stats().suspects > before.suspects);
}

TEST_CASE("acyclic: a broken @acyclic promise is reported by the collector") {
    Env e;
    e.run(R"(
@acyclic
class Promise { var other: Promise }
fn break_it() {
    var a = Promise()
    a.other = a
}
)");
    e.run("break_it()");
    pluma::RunResult r = e.vm().collect_cycles(1000000);
    CHECK_FALSE(r.ok);
    REQUIRE(r.panic.has_value());
    CHECK(r.panic->message.find("'@acyclic' class 'Promise'") != std::string::npos);
    // A kept promise is fine (and the class is still traced in development).
    e.run("@acyclic\nclass Fine { var n = 0; var next: Fine }\nvar f = Fine()\nf.next = Fine()\nf = null");
    CHECK(e.vm().collect_cycles(1000000).ok);
    rejects("@acyclic\nstruct S { }", "1:1: '@acyclic' applies only to classes\n");
}

// ---------------------------------------------------------------- weak

TEST_CASE("weak: weak fields don't keep objects alive and read null after") {
    prints(R"(
class Boss { var hp = 3 }
class Minion { weak var boss: Boss; var name = "m" }
var b = Boss()
var m = Minion()
m.boss = b
print(m.boss?.hp ?? -1)
b = null
print(m.boss == null)
)",
           "3\ntrue\n");
    // Parent links: a weak back reference makes the tree acyclic-by-refcount.
    Env e;
    e.run(R"(
class Node { weak var parent: Node; var kids = List<Node>() }
var root = Node()
fn grow() {
    for i in 0..3 {
        var k = Node()
        k.parent = root
        root.kids.push(k)
    }
}
)");
    e.run("grow()");
    std::size_t with = pluma::live_objects();
    e.run("var k = root.kids[0]\nprint(k.parent == root)\nroot = null\nprint(k.parent == null)\nk = null");
    CHECK(e.out == "true\ntrue\n");
    CHECK(pluma::live_objects() < with);
    e.collect();
}

TEST_CASE("weak: references into collected cycles become null") {
    Env e;
    e.run(R"(
class Pair { var other: Pair; var n = 7 }
class Watcher { weak var seen: Pair }
var w = Watcher()
fn make() {
    var a = Pair()
    a.other = Pair()
    a.other.other = a
    w.seen = a
}
)");
    e.run("make()\nprint(w.seen?.n ?? 0)");
    e.collect();
    e.run("print(w.seen == null)");
    CHECK(e.out == "7\ntrue\n");
}

TEST_CASE("weak: where it is allowed") {
    rejects("fn f() {\n    weak var x: int = 1\n}",
            "2:5: 'weak' applies only to class instance fields, not to local variables\n");
    rejects("weak var g: str = \"\"", "1:10: 'weak' applies only to class instance fields, not to global variables\n");
    rejects("class C { weak var n: int }", "1:20: a 'weak' field must have a class or list type, not 'int'\n");
    rejects("class C { }\nstruct S { weak var c: C }", "2:21: 'weak' fields are only allowed in classes (structs are values)\n");
    rejects("class C { static weak var c: C }", "1:27: 'weak' applies only to instance fields, not to static variables\n");
}

// ---------------------------------------------------------------- manual memory

TEST_CASE("manual: alloc and free; use after free panics with where") {
    prints(R"(
class Buf { var size = 0; var data = List<int>() }
var b = alloc Buf()
b.size = 4
b.data.push(1)
print(b.size + b.data.len())
free(b)
print("freed")
)",
           "5\nfreed\n");
    panics(R"(
class Buf { var size = 0 }
var b = alloc Buf()
var c = b
free(b)
print(c.size)
)",
           "6:7: use after free: 'c'; it was freed at test.plm:5:6");
    panics("class Buf { }\nvar b = alloc Buf()\nfree(b)\nfree(b)",
           "4:6: double free: 'b' was already freed; it was freed at test.plm:3:6");
    panics("class Buf { }\nvar b = Buf()\nvar c = b\nfree(c)",
           "4:6: free: 'c' is not a manual object (only objects from 'alloc', 'new(frame)', 'new(scene)' or a manual "
           "class can be freed)");
    prints("class Buf { }\nvar b: Buf = null\nfree(b)\nprint(\"ok\")", "ok\n"); // free(null) is a no-op
}

TEST_CASE("manual: lists, methods, defer free, manual classes") {
    prints(R"(
fn sum() -> int {
    var xs = alloc List<int>()
    defer free(xs)
    for i in 0..5 { xs.push(i) }
    var t = 0
    for x in xs { t += x }
    return t
}
print(sum())
manual class Pool {
    var items = List<str>()
    fn add(s: str) { items.push(s) }
}
var p = Pool()
p.add("a")
p.add("b")
print(p.items.len())
free(p)
)",
           "10\n2\n");
    panics("var xs = alloc List<int>()\nfree(xs)\nxs.push(1)", "3:1: use after free: the list; it was freed at test.plm:2:6");
    rejects("manual class M { }\nclass S extends M { }",
            "2:7: 'S' extends manual class 'M', so it must be a 'manual class' too\n");
    rejects("class C { }\nfree(C())", "2:6: 'free' needs a manual object; this one is automatic (reference-counted): "
                                       "allocate it with 'alloc' to free it yourself\n");
    rejects("free(3)", "1:6: 'free' takes a manual object (a class instance or a list), not 'int'\n");
    rejects("var x = alloc int()", "1:15: 'int' cannot be allocated with 'alloc': only classes and lists can\n");
    rejects("class C { }\nvar x = new(heap) C()",
            "2:13: unknown arena 'heap': use 'new(frame)' (freed at the end of the tick) or 'new(scene)' (freed when "
            "the scene unloads)\n");
}

TEST_CASE("manual: frame and scene arenas") {
    Env e;
    e.run(R"(
class Tmp { var v = 1 }
var kept: Tmp = null
fn frame_stuff() -> int {
    var t = new(frame) Tmp()
    var pts = new(frame) List<vec2>()
    pts.push(vec2(1.0, 2.0))
    t.v = pts.len()
    return t.v
}
fn leak_to_local() -> Tmp {
    return new(frame) Tmp()
}
class Holder { var t: Tmp }
var h = Holder()
h.t = new(scene) Tmp()
)");
    e.run("print(frame_stuff())\nkept = leak_to_local()\nprint(kept.v)");
    CHECK(e.vm().arena_objects(pluma::ArenaKind::Frame) == 3);
    CHECK(e.vm().arena_objects(pluma::ArenaKind::Scene) == 1);
    e.vm().tick_coroutines(); // tick 1
    e.vm().reset_frame_arena();
    CHECK(e.vm().arena_objects(pluma::ArenaKind::Frame) == 0);
    CHECK_FALSE(e.run("print(kept.v)"));
    CHECK(e.last_panic == "1:7: use after free: 'kept'; it was freed by the frame arena at the end of tick 1");
    CHECK(e.run("print(h.t.v)"));
    e.vm().reset_scene_arena();
    CHECK_FALSE(e.run("print(h.t.v)"));
    CHECK(e.last_panic == "1:7: use after free: 'h.t'; it was freed by the scene arena when the scene unloaded");
    CHECK(e.out == "1\n1\n1\n");
}

TEST_CASE("manual: unsafe skips the generation check") {
    prints(R"(
class Tmp { var v = 1 }
var t = alloc Tmp()
var u = t
free(t)
unsafe {
    u.v = 5      // no check: writes the freed slot
}
print("no panic")
)",
           "no panic\n");
}

TEST_CASE("manual: objects hold counts on automatic objects and release them") {
    Env e;
    e.run("class Auto { var n = 1 }\nclass M { var a: Auto; var xs = List<Auto>() }\n"
          "fn tmp() -> M { return new(frame) M() }");
    const std::size_t base = pluma::live_objects();
    e.run("var m = alloc M()\nm.a = Auto()\nm.xs.push(Auto())\nvar f = tmp()\nf.a = Auto()");
    CHECK(pluma::live_objects() == base + 7); // m, f, their (automatic) lists, three Autos
    e.run("free(m)");
    e.vm().reset_frame_arena();
    // Only the stale references (plain values in globals) remain: no objects.
    CHECK(pluma::live_objects() == base);
    // Manual objects are never suspects, even holding cyclic data.
    pluma::HeapStats s = pluma::heap_stats();
    e.run("class Cyc { var c: Cyc }\nvar k = alloc M()\nvar k2 = k\nk2 = null");
    CHECK(pluma::heap_stats().suspects == s.suspects);
    e.run("free(k)");
}

TEST_CASE("manual: frees cancel the object's coroutines") {
    Env e;
    e.run(R"(
manual class Spinner {
    var n = 0
    fn spin() { loop { n += 1; yield } }
    fn go() { start(spin()) }
}
var s = Spinner()
s.go()
)");
    e.vm().tick_coroutines();
    CHECK(e.vm().coroutines().size() == 1);
    e.run("free(s)");
    CHECK(e.vm().coroutines().empty());
}

TEST_CASE("manual: escape analysis rejects obvious escapes") {
    const char* decls = "class T { var other: T; var xs = List<T>() }\nvar g: T = null\n";
    auto with = [&](const char* body) { return std::string(decls) + body; };
    rejects(with("fn f(keep: T) {\n    var t = new(frame) T()\n    keep.other = t\n}"),
            "5:18: a 'new(frame)' object cannot be stored in field 'other' of a longer-lived object: the frame arena "
            "frees it at the end of the tick (allocate the holder with 'new(frame)' too, or use 'new(scene)' or "
            "'alloc')\n");
    rejects(with("fn f() {\n    g = new(frame) T()\n}"),
            "4:9: a 'new(frame)' object cannot be stored in 'g': the frame arena frees it at the end of the tick "
            "(allocate the holder with 'new(frame)' too, or use 'new(scene)' or 'alloc')\n");
    rejects(with("fn f() {\n    g = new(scene) T()\n}"),
            "4:9: a 'new(scene)' object cannot be stored in 'g': the scene arena frees it when the scene unloads "
            "(use 'alloc')\n");
    rejects(with("fn f(keep: T) {\n    var t = new(frame) T()\n    keep.xs.push(t)\n}"),
            "5:5: a 'new(frame)' object cannot be stored in a longer-lived list: the frame arena frees it at the end "
            "of the tick (allocate the holder with 'new(frame)' too, or use 'new(scene)' or 'alloc')\n");
    rejects(with("class H { var t = new(frame) T() }"),
            "3:19: a 'new(frame)' object cannot be stored in a field initializer: the frame arena frees it at the end "
            "of the tick (allocate the holder with 'new(frame)' too, or use 'new(scene)' or 'alloc')\n");
    rejects(with("fn later(t: T) {\n    yield\n}\nfn f() {\n    var t = new(frame) T()\n    start(later(t))\n}"),
            "8:17: a 'new(frame)' object cannot be stored in a coroutine (it can outlive the tick): the frame arena "
            "frees it at the end of the tick (allocate the holder with 'new(frame)' too, or use 'new(scene)' or "
            "'alloc')\n");
    rejects(with("var keep: fn() = null\nfn f() {\n    var t = new(frame) T()\n    keep = fn() { t.other = null }\n}"),
            "6:12: a 'new(frame)' object cannot be stored in 'keep': the frame arena frees it at the end of the tick "
            "(allocate the holder with 'new(frame)' too, or use 'new(scene)' or 'alloc')\n");
    // Frame objects may point at each other and live in locals; scene objects
    // may go into fields.
    prints(with("fn f(keep: T) -> int {\n    var a = new(frame) T()\n    var b = new(frame) T()\n    a.other = b\n"
                "    var l = new(frame) List<T>()\n    l.push(b)\n    keep.other = new(scene) T()\n"
                "    return l.len()\n}\nprint(f(T()))"),
           "1\n");
}
