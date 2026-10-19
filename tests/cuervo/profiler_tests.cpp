// Engine zones (cuervo/core/profiler.hpp) and the script host's script
// profiling (DESIGN.md 5.14 "Clarified (Phase 8 -- Profiler)").

#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <thread>

#include "cuervo/core/profiler.hpp"
#include "cuervo/script/script_host.hpp"

using namespace cuervo;

namespace {

#if CUERVO_PROFILING
std::atomic<std::uint64_t> fake_ns{1};
std::uint64_t fake_clock() { return fake_ns.load(); }
void advance_ms(double ms) { fake_ns += static_cast<std::uint64_t>(ms * 1e6); }

// The profiler is process-wide: each test starts clean and leaves it off
// with the real clock.
struct FakeClock {
    FakeClock() {
        profiler::set_clock(&fake_clock);
        profiler::reset();
        profiler::set_enabled(true);
    }
    ~FakeClock() {
        profiler::set_enabled(false);
        profiler::reset();
        profiler::set_clock(nullptr);
    }
};

const profiler::ZoneStats* find_zone(const std::vector<profiler::ZoneStats>& zones, std::string_view name) {
    auto it = std::find_if(zones.begin(), zones.end(), [&](const profiler::ZoneStats& z) { return z.name == name; });
    return it == zones.end() ? nullptr : &*it;
}

void outer_frame(double inner_a, double inner_b, double outer_own) {
    CUERVO_PROFILE_ZONE("test.outer");
    {
        CUERVO_PROFILE_ZONE("test.inner");
        advance_ms(inner_a);
    }
    if (inner_b > 0.0) {
        CUERVO_PROFILE_ZONE("test.inner"); // another site, same zone
        advance_ms(inner_b);
    }
    advance_ms(outer_own);
}
#endif

} // namespace

#if CUERVO_PROFILING
TEST_CASE("profiler: nested zones aggregate per frame between frame marks") {
    FakeClock clock;
    profiler::frame_mark(); // the first mark only starts frame 1
    outer_frame(2.0, 1.0, 2.0); // outer 5 ms, inner 3 ms in 2 entries
    advance_ms(1.0);
    profiler::frame_mark(); // frame 1: 6 ms
    outer_frame(1.0, 0.0, 2.0); // outer 3 ms, inner 1 ms
    profiler::frame_mark(); // frame 2: 3 ms

    const profiler::FrameStats frames = profiler::frame_stats();
    CHECK(frames.frames == 2);
    CHECK(frames.avg_ms == doctest::Approx(4.5));
    CHECK(frames.max_ms == doctest::Approx(6.0));
    CHECK(frames.last_ms == doctest::Approx(3.0));
    CHECK(profiler::frame_history() == std::vector<float>{6.0f, 3.0f});

    const auto zones = profiler::zone_stats();
    const profiler::ZoneStats* outer = find_zone(zones, "test.outer");
    const profiler::ZoneStats* inner = find_zone(zones, "test.inner");
    REQUIRE(outer != nullptr);
    REQUIRE(inner != nullptr);
    CHECK(outer->avg_ms == doctest::Approx(4.0));
    CHECK(outer->max_ms == doctest::Approx(5.0));
    CHECK(outer->last_ms == doctest::Approx(3.0));
    CHECK(outer->calls_per_frame == doctest::Approx(1.0));
    CHECK(inner->parent == outer->id);
    CHECK(inner->depth == outer->depth + 1);
    CHECK(inner->avg_ms == doctest::Approx(2.0));
    CHECK(inner->max_ms == doctest::Approx(3.0));
    CHECK(inner->calls_per_frame == doctest::Approx(1.5));
    // Tree order: the child right after its parent.
    CHECK(inner == outer + 1);
    CHECK(profiler::zone_history(inner->id) == std::vector<float>{3.0f, 1.0f});
    CHECK(profiler::report().find("test.inner") != std::string::npos);
}

TEST_CASE("profiler: disabled zones, the ring buffer window and job-worker zones") {
    FakeClock clock;
    profiler::frame_mark();
    // Off: nothing is recorded, frames included.
    profiler::set_enabled(false);
    outer_frame(1.0, 1.0, 1.0);
    profiler::frame_mark();
    CHECK(profiler::frame_stats().frames == 0);
    profiler::set_enabled(true);

    // Zones opened on another thread are ignored (only the main thread records).
    profiler::frame_mark();
    std::thread worker([] {
        CUERVO_PROFILE_ZONE("test.worker");
        advance_ms(1.0);
    });
    worker.join();
    profiler::frame_mark();
    CHECK(profiler::frame_stats().frames == 1);
    CHECK(find_zone(profiler::zone_stats(), "test.worker") == nullptr);

    // The window keeps the last `history` frames.
    for (std::size_t i = 0; i < profiler::history + 10; ++i) {
        outer_frame(1.0, 0.0, static_cast<double>(i % 2));
        profiler::frame_mark();
    }
    const profiler::FrameStats frames = profiler::frame_stats();
    CHECK(frames.frames == profiler::history);
    const auto zones = profiler::zone_stats();
    const profiler::ZoneStats* outer = find_zone(zones, "test.outer");
    REQUIRE(outer != nullptr);
    CHECK(outer->max_ms == doctest::Approx(2.0));
    CHECK(outer->avg_ms == doctest::Approx(1.5));
    profiler::reset();
    CHECK(profiler::frame_stats().frames == 0);
    CHECK(profiler::zone_stats().empty());
}
#endif

TEST_CASE("script host: script profiling survives hot reload and leaves the simulation alone") {
    const char* v1 = "class Main extends Node {\n"
                     "    var n = 0\n"
                     "    fn bump() { n += 1 }\n"
                     "    fn update(dt: float) { bump() }\n"
                     "}\n";
    const char* v2 = "class Main extends Node {\n"
                     "    var n = 0\n"
                     "    fn bump() { n += 2 }\n"
                     "    fn update(dt: float) { bump() }\n"
                     "}\n";
    InputMap map;
    script::ScriptHost plain({}, map);
    script::ScriptHost profiled({}, map);
    profiled.set_script_profiling(true);
    REQUIRE_MESSAGE(plain.load({{"main.plm", v1}}, "Main"), plain.error());
    REQUIRE_MESSAGE(profiled.load({{"main.plm", v1}}, "Main"), profiled.error());
    REQUIRE(profiled.script_profiler() != nullptr);
    CHECK(profiled.session()->vm().debug_hooks() == profiled.script_profiler());
    for (int i = 0; i < 5; ++i) {
        plain.tick({});
        profiled.tick({});
    }
    // A hot reload is a fresh VM: the profiler follows it.
    REQUIRE(plain.load({{"main.plm", v2}}, "Main"));
    REQUIRE(profiled.load({{"main.plm", v2}}, "Main"));
    CHECK(profiled.session()->vm().debug_hooks() == profiled.script_profiler());
    for (int i = 0; i < 5; ++i) {
        plain.tick({});
        profiled.tick({});
    }
    CHECK(plain.state_hash() == profiled.state_hash());
    const auto stats = profiled.script_profiler()->snapshot();
    auto bump = std::find_if(stats.begin(), stats.end(), [](const auto& f) { return f.name == "Main.bump"; });
    REQUIRE(bump != stats.end());
    CHECK(bump->calls == 10); // 5 before and 5 after the reload, one entry
    CHECK(profiled.script_profiler()->depth() == 0);

    // Off: uninstalled, numbers kept; another DebugHooks is never replaced.
    profiled.set_script_profiling(false);
    CHECK(profiled.session()->vm().debug_hooks() == nullptr);
    CHECK_FALSE(profiled.script_profiler()->snapshot().empty());
    struct Other : pluma::DebugHooks {
        std::uint32_t events() const override { return calls; }
    } other;
    profiled.session()->vm().set_debug_hooks(&other);
    profiled.set_script_profiling(true);
    CHECK(profiled.session()->vm().debug_hooks() == &other);
    profiled.session()->vm().set_debug_hooks(nullptr);
}
