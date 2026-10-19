// The script profiler (pluma/profiler.hpp, DESIGN.md 5.14): call counts,
// self and inclusive time from the VM's call hooks, balanced across
// coroutines, panics and VMs.

#include <doctest.h>

#include <algorithm>
#include <string>

#include "pluma/bytecode.hpp"
#include "pluma/profiler.hpp"
#include "pluma/session.hpp"
#include "pluma/vm.hpp"

namespace {

// Every clock read is one tick later: times become counts of hook events,
// so they are exact.
std::uint64_t ticks = 0;
std::uint64_t tick_clock() { return ++ticks; }

const pluma::ScriptProfiler::FunctionStats* find(const std::vector<pluma::ScriptProfiler::FunctionStats>& all,
                                                 std::string_view name) {
    auto it = std::find_if(all.begin(), all.end(), [&](const auto& f) { return f.name == name; });
    return it == all.end() ? nullptr : &*it;
}

pluma::VmOptions quiet() {
    pluma::VmOptions o;
    o.out = [](std::string_view) {};
    return o;
}

} // namespace

TEST_CASE("script profiler: exact call counts, self and inclusive time") {
    pluma::ScriptProfiler prof(&tick_clock);
    pluma::Session session(quiet());
    session.vm().set_debug_hooks(&prof);
    const char* src = "fn leaf(x: int) -> int { return x + 1 }\n"
                      "fn mid(x: int) -> int { return leaf(x) + leaf(x) }\n"
                      "fn fib(n: int) -> int {\n"
                      "    if n < 2 { return n }\n"
                      "    return fib(n - 1) + fib(n - 2)\n"
                      "}\n"
                      "var t = 0\n"
                      "for i in 0..10 { t += mid(i) }\n"
                      "print(fib(10))\n";
    const pluma::SessionResult r = session.run_script("test.plm", src);
    REQUIRE_MESSAGE(r.ok, r.rendered);
    CHECK(prof.depth() == 0);
    const auto all = prof.snapshot();
    const auto* leaf = find(all, "leaf");
    const auto* mid = find(all, "mid");
    const auto* fib = find(all, "fib");
    REQUIRE(leaf != nullptr);
    REQUIRE(mid != nullptr);
    REQUIRE(fib != nullptr);
    CHECK(leaf->calls == 20);
    CHECK(mid->calls == 10);
    CHECK(fib->calls == 177); // fib(10) makes 177 calls
    CHECK(leaf->location == "test.plm:1");
    // One tick per hook event: a leaf frame lasts 1 tick; a mid frame 5
    // (enter, 2 x leaf enter/leave, leave), 2 of them in its leaves.
    CHECK(leaf->inclusive_ns == 20);
    CHECK(leaf->self_ns == 20);
    CHECK(mid->inclusive_ns == 50);
    CHECK(mid->self_ns == 30);
    // Recursion: fib's inclusive time is its outermost frame's (2 * 177 - 1
    // ticks), not the sum over every level.
    CHECK(fib->inclusive_ns == 2 * 177 - 1);
    CHECK(fib->self_ns == fib->inclusive_ns);
    std::uint64_t self_sum = 0;
    for (const auto& f : all) {
        CHECK(f.self_ns <= f.inclusive_ns);
        self_sum += f.self_ns;
    }
    CHECK(self_sum == prof.total_ns());
    // Sorted by self time.
    CHECK(std::is_sorted(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.self_ns > b.self_ns; }));
    const std::string report = prof.report(2);
    INFO(report);
    CHECK(report.find("fib (test.plm:4)") != std::string::npos);
    CHECK(report.find("more") != std::string::npos);

    prof.reset();
    CHECK(prof.snapshot().empty());
    CHECK(prof.total_ns() == 0);
    REQUIRE(session.run_script("again.plm", "print(mid(1))").ok);
    const auto again = prof.snapshot();
    REQUIRE(find(again, "mid") != nullptr);
    CHECK(find(again, "mid")->calls == 1);
    CHECK(find(again, "leaf")->calls == 2);
}

TEST_CASE("script profiler: coroutines and panics keep the stack balanced") {
    pluma::ScriptProfiler prof;
    pluma::Session session(quiet());
    session.vm().set_debug_hooks(&prof);
    const char* src = "fn inner() {\n yield\n yield\n}\n"
                      "fn run() {\n await inner()\n print(\"done\")\n}\n"
                      "fn boom(n: int) -> int {\n if n == 0 { return 1 / n }\n return boom(n - 1)\n}\n"
                      "start(run())\n";
    REQUIRE(session.run_script("test.plm", src).ok);
    CHECK(prof.depth() == 0); // suspended frames have left
    for (int i = 0; i < 4; ++i) {
        REQUIRE(session.vm().tick_coroutines().ok);
        CHECK(prof.depth() == 0);
    }
    const auto all = prof.snapshot();
    // A resume enters the suspended frames again: inner and run are entered
    // once at the start and once per resume (two yields).
    REQUIRE(find(all, "inner") != nullptr);
    REQUIRE(find(all, "run") != nullptr);
    CHECK(find(all, "inner")->calls == 3);
    CHECK(find(all, "run")->calls == 3);

    // A panic four frames deep unwinds them all.
    const pluma::SessionResult r = session.run_script("panic.plm", "boom(3)");
    CHECK(r.panic.has_value());
    CHECK(prof.depth() == 0);
    REQUIRE(find(prof.snapshot(), "boom") != nullptr);
    CHECK(find(prof.snapshot(), "boom")->calls == 4);
    for (const auto& f : prof.snapshot()) CHECK(f.self_ns <= f.inclusive_ns);
}

TEST_CASE("script profiler: one profiler over several VMs (hot reload) aggregates by function") {
    pluma::ScriptProfiler prof;
    for (int i = 0; i < 2; ++i) {
        pluma::Session session(quiet());
        session.vm().set_debug_hooks(&prof);
        REQUIRE(session.run_script("test.plm", "fn f() -> int { return 1 }\nprint(f() + f())\n").ok);
    }
    REQUIRE(find(prof.snapshot(), "f") != nullptr);
    CHECK(find(prof.snapshot(), "f")->calls == 4);

    // Hooks installed while a frame is open: its return has no entry and is ignored.
    pluma::Session session(quiet());
    pluma::ScriptProfiler late;
    pluma::Proto fn;
    fn.name = "entered_before";
    late.on_return(session.vm(), fn);
    CHECK(late.depth() == 0);
    CHECK(late.snapshot().empty());
}
