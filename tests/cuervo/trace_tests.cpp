// Trace capture and Chrome trace JSON (milestone 11.4, cuervo/profile/trace.hpp).

#include <doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cuervo/core/profiler.hpp"
#include "cuervo/jobs/job_system.hpp"
#include "cuervo/profile/trace.hpp"

using namespace cuervo;

namespace {
std::uint64_t g_now = 0;
std::uint64_t fake_clock() { return g_now; }
bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
} // namespace

TEST_CASE("trace: zones become spans with durations, counters and thread metadata") {
    if constexpr (!profiler::compiled_in) return;
    profiler::set_clock(&fake_clock);
    profiler::set_enabled(true);
    profiler::reset();
    g_now = 1'000'000;
    profile::start_capture();
    profiler::frame_mark();
    {
        CUERVO_PROFILE_ZONE("trace_outer");
        g_now += 2'000'000;
        {
            CUERVO_PROFILE_ZONE("trace_inner");
            g_now += 500'000;
        }
        g_now += 1'000'000;
    }
    profile::add_draw_calls(3);
    profile::add_draw_calls(4);
    profile::end_frame();
    profile::stop_capture();
    profiler::set_enabled(false);
    profiler::set_clock(nullptr);

    const auto events = profile::events();
    const auto totals = profile::zone_totals(events);
    REQUIRE(totals.size() == 2);
    CHECK(totals[0].name == "trace_outer");
    CHECK(totals[0].total_ms == doctest::Approx(3.5));
    CHECK(totals[1].name == "trace_inner");
    CHECK(totals[1].total_ms == doctest::Approx(0.5));
    CHECK(profile::take_draw_calls() == 0); // end_frame consumed them

    const std::string json = profile::chrome_trace_json(events);
    CHECK(contains(json, "\"traceEvents\""));
    CHECK(contains(json, "\"name\":\"trace_outer\",\"cat\":\"zone\",\"ph\":\"X\""));
    CHECK(contains(json, "\"dur\":3500.000"));
    CHECK(contains(json, "\"name\":\"draw_calls\""));
    CHECK(contains(json, "\"value\":7.000"));
    CHECK(contains(json, "\"thread_name\""));
}

TEST_CASE("trace: nothing is recorded outside a capture, and the buffer is bounded") {
    profile::add_span("zone", "ignored", 0, 10);
    profile::start_capture(2);
    profile::stop_capture();
    profile::add_span("zone", "late", 0, 10);
    CHECK(profile::events().empty());

    profile::start_capture(2);
    for (int i = 0; i < 5; ++i) profile::add_span("zone", "s", 0, 10);
    profile::stop_capture();
    CHECK(profile::events().size() == 2);
    CHECK(profile::dropped_events() == 3);
}

TEST_CASE("trace: names are JSON-escaped and the file is written") {
    profile::start_capture();
    profile::add_span("zone", "a \"quoted\"\\name\n", 5, 9);
    profile::stop_capture();
    const std::string json = profile::chrome_trace_json(profile::events());
    CHECK(contains(json, "a \\\"quoted\\\"\\\\name\\n"));

    const auto path = std::filesystem::temp_directory_path() / "cuervo_trace_test.json";
    std::string error;
    REQUIRE_MESSAGE(profile::write_chrome_trace(path, &error), error);
    std::stringstream text;
    {
        // Closed before the remove: Windows refuses to delete an open file.
        std::ifstream in(path);
        text << in.rdbuf();
    }
    CHECK(text.str() == json);
    std::filesystem::remove(path);
    CHECK_FALSE(profile::write_chrome_trace(std::filesystem::temp_directory_path() / "no_such_dir_xyz" / "t.json"));
}

TEST_CASE("trace: job tasks land on the thread timeline") {
    jobs::JobSystem pool(3);
    profile::start_capture();
    std::atomic<int> ran{0};
    pool.run(64, [&](std::size_t) {
        ran.fetch_add(1);
        std::this_thread::yield();
    });
    profile::stop_capture();
    CHECK(ran == 64);
    int job_spans = 0;
    for (const profile::TraceEvent& e : profile::events()) {
        if (std::string_view(e.category) == "job") ++job_spans;
    }
    CHECK(job_spans == 64);
}
