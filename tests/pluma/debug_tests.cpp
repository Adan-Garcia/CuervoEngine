// VM debug hooks (pluma/debug.hpp, DESIGN.md 5.14): line and call events,
// stack and value inspection for the DAP debugger and the profiler.

#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <numeric>
#include <functional>
#include <string>
#include <vector>

#include "pluma/session.hpp"
#include "pluma/value.hpp"
#include "pluma/vm.hpp"

namespace {

struct Recorder : pluma::DebugHooks {
    std::uint32_t mask = lines | calls;
    std::vector<std::string> lines_seen; // "function:line"
    int depth = 0;
    int max_depth = 0;
    std::vector<std::string> calls_seen;
    std::function<void(pluma::VM&)> at_line;

    std::uint32_t events() const override { return mask; }
    void on_line(pluma::VM& vm) override {
        const auto frames = vm.stack_frames();
        REQUIRE_FALSE(frames.empty());
        lines_seen.push_back(std::format("{}:{}", frames[0].function, frames[0].line));
        if (at_line) at_line(vm);
    }
    void on_call(pluma::VM&, const pluma::Proto& fn) override {
        max_depth = std::max(max_depth, ++depth);
        calls_seen.push_back("+" + fn.name);
    }
    void on_return(pluma::VM&, const pluma::Proto& fn) override {
        --depth;
        calls_seen.push_back("-" + fn.name);
    }
    std::size_t count(std::string_view entry) const {
        return static_cast<std::size_t>(std::count(lines_seen.begin(), lines_seen.end(), entry));
    }
};

const char* const adder = "fn add(a: int, b: int) -> int {\n" // 1
                          "    var c = a + b\n"               // 2
                          "    return c\n"                    // 3
                          "}\n"                               // 4
                          "var total = 0\n"                   // 5
                          "for i in 0..3 { total += add(i, 1) }\n" // 6
                          "print(total)\n";                   // 7

} // namespace

TEST_CASE("debug hooks: line events per line, per call and per pass of a one-line loop") {
    Recorder rec;
    pluma::Session session;
    session.vm().set_debug_hooks(&rec);
    const pluma::SessionResult r = session.run_script("test.plm", adder);
    REQUIRE_MESSAGE(r.ok, r.rendered);
    INFO(std::format("{}", rec.lines_seen.size()));
    CHECK(rec.count("add:2") == 3);
    CHECK(rec.count("add:3") == 3);
    CHECK(rec.count("add:4") == 0); // the closing brace has no code
    // The loop line comes back after every call and on each backward jump.
    const auto loop = std::find_if(rec.lines_seen.begin(), rec.lines_seen.end(),
                                   [](const std::string& s) { return s.ends_with(":6"); });
    REQUIRE(loop != rec.lines_seen.end());
    CHECK(std::count_if(rec.lines_seen.begin(), rec.lines_seen.end(),
                        [](const std::string& s) { return s.ends_with(":6"); }) >= 3);
    INFO(std::accumulate(rec.lines_seen.begin(), rec.lines_seen.end(), std::string(), [](std::string a, const std::string& b) { return a + b + " "; }));
    CHECK(rec.lines_seen.back().ends_with(":7"));

    // Removing the hooks stops the events.
    session.vm().set_debug_hooks(nullptr);
    const std::size_t seen = rec.lines_seen.size();
    REQUIRE(session.run_script("again.plm", "print(add(1, 2))").ok);
    CHECK(rec.lines_seen.size() == seen);
}

TEST_CASE("debug hooks: stack frames and locals at a line") {
    Recorder rec;
    std::vector<pluma::StackFrameInfo> frames;
    std::vector<std::string> locals;
    rec.at_line = [&](pluma::VM& vm) {
        auto f = vm.stack_frames();
        if (!frames.empty() || f[0].function != "add" || f[0].line != 3) return;
        frames = f;
        for (const pluma::LocalInfo& l : vm.frame_locals(0)) {
            locals.push_back(std::format("{}: {} = {}", l.name, l.type, vm.describe(l.value)));
        }
    };
    pluma::Session session;
    session.vm().set_debug_hooks(&rec);
    REQUIRE(session.run_script("test.plm", adder).ok);
    REQUIRE(frames.size() >= 2);
    CHECK(frames[0].file == "test.plm");
    CHECK(frames[1].line == 6); // the caller, inside its call
    CHECK(locals == std::vector<std::string>{"a: int = 0", "b: int = 1", "c: int = 1"});
}

TEST_CASE("debug hooks: calls and returns balance across coroutines and panics") {
    Recorder rec;
    rec.mask = pluma::DebugHooks::calls;
    pluma::Session session;
    session.vm().set_debug_hooks(&rec);
    const char* src = "fn inner() {\n yield\n yield\n}\n"
                      "fn run() {\n await inner()\n print(\"done\")\n}\n"
                      "fn boom(n: int) -> int {\n if n == 0 { return 1 / n }\n return boom(n - 1)\n}\n"
                      "start(run())\n";
    REQUIRE(session.run_script("test.plm", src).ok);
    for (int i = 0; i < 4; ++i) REQUIRE(session.vm().tick_coroutines().ok);
    CHECK(rec.depth == 0);
    // A panic three frames deep unwinds them all.
    const pluma::SessionResult r = session.run_script("panic.plm", "boom(3)");
    CHECK(r.panic.has_value());
    CHECK(rec.depth == 0);
    CHECK(rec.max_depth >= 5);
    CHECK(rec.lines_seen.empty()); // lines were not asked for
}

TEST_CASE("debug hooks: children of values and lines with code") {
    pluma::Session session;
    const char* src = "class P {\n var name = \"p\"\n var pos = vec2(1.0, 2.0)\n var tags = [3, 4]\n}\n"
                      "var p = P()\n";
    REQUIRE(session.run_script("test.plm", src).ok);
    pluma::VM& vm = session.vm();
    const pluma::Value p = session.global("p");
    const auto fields = vm.children(p);
    REQUIRE(fields.size() == 3);
    CHECK(fields[0].name == "name");
    CHECK(vm.describe(fields[0].value) == "\"p\"");
    const auto pos = vm.children(fields[1].value);
    REQUIRE(pos.size() == 2);
    CHECK(pos[1].name == "y");
    CHECK(vm.describe(pos[1].value) == "2.0");
    const auto tags = vm.children(fields[2].value);
    REQUIRE(tags.size() == 2);
    CHECK(tags[1].name == "[1]");
    CHECK(vm.describe(tags[1].value) == "4");

    pluma::Session adder_session;
    REQUIRE(adder_session.run_script("adder.plm", adder).ok);
    const auto lines = adder_session.vm().code_lines("adder.plm");
    for (std::uint32_t l : {2u, 3u, 5u, 6u, 7u}) CHECK(std::find(lines.begin(), lines.end(), l) != lines.end());
    CHECK(std::find(lines.begin(), lines.end(), 4u) == lines.end());
    CHECK(adder_session.vm().code_lines("other.plm").empty());
}
