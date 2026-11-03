#pragma once

// Trace capture (DESIGN.md 5.14, 7.2 "Profiling"): records timed spans and
// counters from the frame loop, the engine zones (cuervo/core/profiler.hpp)
// and the job workers, and writes them as Chrome trace-event JSON (open in
// chrome://tracing or ui.perfetto.dev).
//
//   profile::start_capture();          // hooks the zone sink; profiler must be enabled
//   ...frames run...
//   profile::stop_capture();
//   profile::write_chrome_trace(path); // or chrome_trace_json()
//
// What lands in a trace:
//   - category "zone": every engine zone on the main thread (frame,
//     simulation, scripts, update, physics, render, ... : the per-system tick
//     cost) as a complete ("X") event;
//   - category "job": one span per job-system task, on the thread that ran it
//     (the job thread timeline);
//   - counters ("C"): draw calls per frame and the Pluma heap
//     (pluma/profiler.hpp HeapProfiler).
//
// Like the zone profiler this only observes: timestamps come from a
// monotonic clock, never reach the simulation, and are not part of any hash.
// The recorder is bounded; events past `max_events` are counted as dropped.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cuervo::profile {

struct TraceEvent {
    std::string name;
    const char* category = "zone";
    char phase = 'X';            // 'X' span, 'C' counter
    std::uint64_t start_ns = 0;  // profiler clock
    std::uint64_t duration_ns = 0;
    std::uint32_t thread = 0;    // small index: 0 = the thread that started the capture
    double value = 0.0;          // counters
};

// Begins a capture (clears earlier events) and installs the zone sink.
void start_capture(std::size_t max_events = 1u << 20);
// Ends it (events stay readable until the next start).
void stop_capture();
bool capturing();

// Thread-safe; ignored when no capture is running.
void add_span(const char* category, std::string name, std::uint64_t start_ns, std::uint64_t end_ns);
void add_counter(const char* name, std::uint64_t at_ns, double value);

// Draw calls issued since the last take (batchers add; the frame loop takes
// one value per frame and records it as a counter). Always counts, so it is
// cheap enough to leave on.
void add_draw_calls(std::uint32_t n);
std::uint32_t take_draw_calls();

// Marks a frame boundary: records the draw-call counter and any extra counters.
void end_frame();

std::vector<TraceEvent> events();
std::size_t dropped_events();

// Chrome trace-event JSON ("traceEvents" array plus thread-name metadata).
std::string chrome_trace_json(const std::vector<TraceEvent>& events);
bool write_chrome_trace(const std::filesystem::path& path, std::string* error = nullptr);

// Total time in ms of zone spans by name, for summaries and tests.
struct ZoneTotal {
    std::string name;
    double total_ms = 0.0;
    std::size_t spans = 0;
};
std::vector<ZoneTotal> zone_totals(const std::vector<TraceEvent>& events);

// True while a capture runs: the job system's cheap gate for per-task spans.
bool job_spans_enabled();

} // namespace cuervo::profile
