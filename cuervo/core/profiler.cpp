#include "cuervo/core/profiler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>

namespace cuervo::profiler {

namespace {

// Observation only (see profiler.hpp): never read by the simulation.
std::uint64_t steady_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct Frame {
    float frame_ms = 0.0f;
    std::array<float, max_zones> ms{};
    std::array<std::uint16_t, max_zones> calls{};
};

struct Open {
    ZoneId id;
    std::uint64_t start;
};

struct State {
    std::atomic<bool> enabled{false};
    std::atomic<std::thread::id> main_thread{};
    std::atomic<ClockFn> clock{&steady_ns};

    // Registry: written under `registry_mutex` (any thread), names published
    // by the release store of `zone_count`.
    std::mutex registry_mutex;
    std::array<const char*, max_zones> names{};
    std::atomic<std::uint32_t> zone_count{0};

    // Main thread only from here on.
    std::array<ZoneId, max_zones> parent{};
    std::array<bool, max_zones> placed{}; // parent decided (first opening)
    std::array<Open, 128> stack{};
    std::size_t depth = 0;
    std::size_t overflow = 0; // zones opened past the stack's capacity
    Frame current;
    std::vector<Frame> ring = std::vector<Frame>(history);
    std::size_t head = 0;   // next slot to write
    std::size_t filled = 0; // recorded frames, <= history
    std::uint64_t last_mark = 0;

    State() { parent.fill(no_zone); }
};

State& state() {
    static State s;
    return s;
}

bool on_main_thread(const State& s) {
    return s.main_thread.load(std::memory_order_relaxed) == std::this_thread::get_id();
}

std::uint64_t now(const State& s) { return s.clock.load(std::memory_order_relaxed)(); }

// Recorded frames, oldest first.
template <typename Fn>
void each_frame(const State& s, Fn&& fn) {
    const std::size_t first = (s.head + history - s.filled) % history;
    for (std::size_t i = 0; i < s.filled; ++i) fn(s.ring[(first + i) % history]);
}

} // namespace

void set_clock(ClockFn clock) { state().clock.store(clock != nullptr ? clock : &steady_ns); }

void set_enabled(bool on) {
    State& s = state();
    if (on) s.main_thread.store(std::this_thread::get_id());
    // The first frame after enabling starts at the next mark, not at a stale one.
    if (on && !s.enabled.load()) s.last_mark = 0;
    s.enabled.store(on);
}

bool enabled() { return state().enabled.load(std::memory_order_relaxed); }

void reset() {
    State& s = state();
    s.current = Frame{};
    s.head = 0;
    s.filled = 0;
    s.last_mark = 0;
}

ZoneId register_zone(const char* name) {
    State& s = state();
    std::lock_guard lock(s.registry_mutex);
    const std::uint32_t n = s.zone_count.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (std::strcmp(s.names[i], name) == 0) return static_cast<ZoneId>(i);
    }
    if (n >= max_zones) return no_zone;
    s.names[n] = name;
    s.zone_count.store(n + 1, std::memory_order_release);
    return static_cast<ZoneId>(n);
}

void begin_zone(ZoneId id) {
    State& s = state();
    if (id == no_zone || !s.enabled.load(std::memory_order_relaxed) || !on_main_thread(s)) return;
    if (s.depth >= s.stack.size()) {
        ++s.overflow;
        return;
    }
    if (!s.placed[id]) {
        s.placed[id] = true;
        s.parent[id] = s.depth > 0 ? s.stack[s.depth - 1].id : no_zone;
    }
    s.stack[s.depth++] = {id, now(s)};
}

void end_zone(ZoneId id) {
    State& s = state();
    if (id == no_zone || !on_main_thread(s)) return;
    if (s.overflow > 0) {
        --s.overflow;
        return;
    }
    // A zone opened while recording was off has no entry: only close our own.
    if (s.depth == 0 || s.stack[s.depth - 1].id != id) return;
    const Open open = s.stack[--s.depth];
    const std::uint64_t t = now(s);
    s.current.ms[id] += static_cast<float>(static_cast<double>(t - open.start) / 1e6);
    ++s.current.calls[id];
}

#if CUERVO_PROFILING
void frame_mark() {
#if CUERVO_TRACY
    FrameMark;
#endif
    State& s = state();
    if (!s.enabled.load(std::memory_order_relaxed) || !on_main_thread(s)) return;
    const std::uint64_t t = now(s);
    if (s.last_mark != 0) {
        s.current.frame_ms = static_cast<float>(static_cast<double>(t - s.last_mark) / 1e6);
        s.ring[s.head] = s.current;
        s.head = (s.head + 1) % history;
        s.filled = std::min(s.filled + 1, history);
    }
    s.current = Frame{};
    s.last_mark = t;
}
#endif

FrameStats frame_stats() {
    const State& s = state();
    FrameStats out;
    out.frames = s.filled;
    double sum = 0.0;
    each_frame(s, [&](const Frame& f) {
        sum += f.frame_ms;
        out.max_ms = std::max(out.max_ms, static_cast<double>(f.frame_ms));
        out.last_ms = f.frame_ms;
    });
    if (s.filled > 0) out.avg_ms = sum / static_cast<double>(s.filled);
    return out;
}

std::vector<ZoneStats> zone_stats() {
    const State& s = state();
    const std::uint32_t n = s.zone_count.load(std::memory_order_acquire);
    std::vector<ZoneStats> flat(n);
    std::vector<bool> seen(n, false);
    for (std::uint32_t i = 0; i < n; ++i) {
        flat[i].name = s.names[i];
        flat[i].id = static_cast<ZoneId>(i);
        flat[i].parent = s.parent[i];
    }
    double calls[max_zones] = {};
    each_frame(s, [&](const Frame& f) {
        for (std::uint32_t i = 0; i < n; ++i) {
            if (f.calls[i] == 0) continue;
            seen[i] = true;
            flat[i].avg_ms += f.ms[i];
            flat[i].max_ms = std::max(flat[i].max_ms, static_cast<double>(f.ms[i]));
            calls[i] += f.calls[i];
        }
        for (std::uint32_t i = 0; i < n; ++i) flat[i].last_ms = f.ms[i];
    });
    if (s.filled > 0) {
        for (std::uint32_t i = 0; i < n; ++i) {
            flat[i].avg_ms /= static_cast<double>(s.filled);
            flat[i].calls_per_frame = calls[i] / static_cast<double>(s.filled);
        }
    }
    // Tree order: each zone after its parent, siblings in registration order.
    std::vector<ZoneStats> out;
    out.reserve(n);
    const auto visit = [&](auto&& self, ZoneId parent, int depth) -> void {
        for (std::uint32_t i = 0; i < n; ++i) {
            if (!seen[i] || flat[i].parent != parent) continue;
            flat[i].depth = depth;
            out.push_back(flat[i]);
            self(self, static_cast<ZoneId>(i), depth + 1);
        }
    };
    visit(visit, no_zone, 0);
    // Zones whose parent was never seen in the window still show, at the top.
    for (std::uint32_t i = 0; i < n; ++i) {
        if (seen[i] && flat[i].parent != no_zone && !seen[flat[i].parent]) {
            flat[i].depth = 0;
            out.push_back(flat[i]);
            visit(visit, static_cast<ZoneId>(i), 1);
        }
    }
    return out;
}

std::vector<float> frame_history() {
    const State& s = state();
    std::vector<float> out;
    out.reserve(s.filled);
    each_frame(s, [&](const Frame& f) { out.push_back(f.frame_ms); });
    return out;
}

std::vector<float> zone_history(ZoneId id) {
    const State& s = state();
    std::vector<float> out;
    if (id >= max_zones) return out;
    out.reserve(s.filled);
    each_frame(s, [&](const Frame& f) { out.push_back(f.ms[id]); });
    return out;
}

std::string report() {
    const FrameStats frames = frame_stats();
    std::string out = std::format("frame: {:.3f} ms avg, {:.3f} ms max over {} frames\n", frames.avg_ms, frames.max_ms,
                                  frames.frames);
    out += std::format("  {:<32} {:>9} {:>9} {:>8}\n", "zone", "avg ms", "max ms", "calls/f");
    for (const ZoneStats& z : zone_stats()) {
        const std::string label = std::string(static_cast<std::size_t>(z.depth) * 2, ' ') + z.name;
        out += std::format("  {:<32} {:>9.3f} {:>9.3f} {:>8.2f}\n", label, z.avg_ms, z.max_ms, z.calls_per_frame);
    }
    return out;
}

} // namespace cuervo::profiler
