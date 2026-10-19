#pragma once

// Engine zones (DESIGN.md 5.14 "a profiler view", 7.2 "Profiling"): a tiny
// scoped-zone profiler for the main phases of a frame.
//
//   CUERVO_PROFILE_ZONE("tick");   // until the end of the enclosing scope
//   profiler::frame_mark();        // once per frame, outside every zone
//
// Each frame records the inclusive time and the entry count of every named
// zone into a ring buffer of the last `history` frames; zone_stats() gives
// the average and maximum per frame over that window, in tree order (a zone
// is placed under the zone it was first opened in). Zone names are static
// strings; two sites with the same name share a zone. A zone nested in
// itself (recursion) counts its time once per level.
//
// Threads: only the main thread records (the thread that called
// set_enabled(true)); zones opened on job workers are ignored here, which
// keeps recording lock-free and the per-frame numbers meaningful (a worker's
// time overlaps the main thread's). Tracy, when compiled in, sees every
// thread. Stats are read from the main thread too (the runtime's summary,
// the editor's Profiler panel).
//
// Recording is off until set_enabled(true) (`cuervo --profile`, the editor's
// Profiler panel); a disabled zone costs one relaxed atomic load. With the
// CMake option CUERVO_PROFILING=OFF the macros compile to nothing and
// frame_mark() is empty. CUERVO_TRACY=ON additionally maps every zone and
// frame mark to Tracy (always on, the viewer connects on demand).
//
// Profiling reads a monotonic clock. That is allowed because it only
// observes: nothing recorded here ever feeds the simulation 

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#if CUERVO_TRACY
#include <tracy/Tracy.hpp>
#endif

namespace cuervo::profiler {

inline constexpr bool compiled_in = CUERVO_PROFILING != 0;
inline constexpr std::size_t history = 300; // frames kept
inline constexpr std::size_t max_zones = 64;

using ZoneId = std::uint16_t;
inline constexpr ZoneId no_zone = 0xffff;

// Nanoseconds from an arbitrary origin; tests inject a fake clock
// (nullptr = std::chrono::steady_clock).
using ClockFn = std::uint64_t (*)();
void set_clock(ClockFn clock);

void set_enabled(bool on);
bool enabled();
// Forgets every recorded frame (the zones stay registered).
void reset();

// The zone named `name` (a string with static storage), registered on first
// use; no_zone once max_zones are taken.
ZoneId register_zone(const char* name);
void begin_zone(ZoneId id);
void end_zone(ZoneId id);

class ScopedZone {
public:
    explicit ScopedZone(ZoneId id) : id_(id) { begin_zone(id_); }
    ~ScopedZone() { end_zone(id_); }
    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;

private:
    ZoneId id_;
};

#if CUERVO_PROFILING
// Closes the current frame: its zone times go into the ring buffer.
void frame_mark();
#else
inline void frame_mark() {}
#endif

struct ZoneStats {
    const char* name = "";
    ZoneId id = no_zone;
    ZoneId parent = no_zone; // the zone it was first opened in
    int depth = 0;           // nesting level for display (0 = top)
    double avg_ms = 0.0;     // per frame, over the recorded frames
    double max_ms = 0.0;
    double last_ms = 0.0;    // the latest frame
    double calls_per_frame = 0.0;
};

struct FrameStats {
    std::size_t frames = 0; // recorded frames in the window
    double avg_ms = 0.0;
    double max_ms = 0.0;
    double last_ms = 0.0;
};

// Zones seen in the window, parents before their children.
std::vector<ZoneStats> zone_stats();
FrameStats frame_stats();
// Frame times (ms), oldest first; `zone_history` the same for one zone.
std::vector<float> frame_history();
std::vector<float> zone_history(ZoneId id);
// A text table of frame_stats() and zone_stats() (`cuervo --profile`).
std::string report();

} // namespace cuervo::profiler

#define CUERVO_PROFILE_CONCAT_(a, b) a##b
#define CUERVO_PROFILE_CONCAT(a, b) CUERVO_PROFILE_CONCAT_(a, b)

#if CUERVO_PROFILING
#if CUERVO_TRACY
// A variable per line, so several zones can share a scope.
#define CUERVO_PROFILE_TRACY_(name) ZoneNamedN(CUERVO_PROFILE_CONCAT(cuervo_tracy_zone_, __LINE__), name, true)
#else
#define CUERVO_PROFILE_TRACY_(name) static_cast<void>(0)
#endif
// `name` must be a string literal (Tracy keeps it as a static source location).
#define CUERVO_PROFILE_ZONE(name)                                                                                    \
    static const ::cuervo::profiler::ZoneId CUERVO_PROFILE_CONCAT(cuervo_zone_id_, __LINE__) =                   \
        ::cuervo::profiler::register_zone(name);                                                                    \
    const ::cuervo::profiler::ScopedZone CUERVO_PROFILE_CONCAT(cuervo_zone_, __LINE__)(                          \
        CUERVO_PROFILE_CONCAT(cuervo_zone_id_, __LINE__));                                                          \
    CUERVO_PROFILE_TRACY_(name)
#else
#define CUERVO_PROFILE_ZONE(name) static_cast<void>(0)
#endif
