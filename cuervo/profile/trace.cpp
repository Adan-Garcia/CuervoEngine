#include "cuervo/profile/trace.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include "cuervo/core/profiler.hpp"

namespace cuervo::profile {

namespace {

struct Recorder {
    std::atomic<bool> capturing{false};
    std::atomic<std::uint32_t> draw_calls{0};
    std::mutex mutex;
    std::vector<TraceEvent> events;
    std::size_t max_events = 0;
    std::size_t dropped = 0;
    std::map<std::thread::id, std::uint32_t> threads;
};

Recorder& rec() {
    static Recorder r;
    return r;
}

// Caller holds the mutex.
std::uint32_t thread_index(Recorder& r) {
    const auto [it, inserted] = r.threads.emplace(std::this_thread::get_id(), static_cast<std::uint32_t>(r.threads.size()));
    return it->second;
}

void push(TraceEvent e) {
    Recorder& r = rec();
    std::lock_guard lock(r.mutex);
    if (!r.capturing.load(std::memory_order_relaxed)) return;
    if (r.events.size() >= r.max_events) {
        ++r.dropped;
        return;
    }
    e.thread = thread_index(r);
    r.events.push_back(std::move(e));
}

void zone_sink(const char* name, std::uint64_t start_ns, std::uint64_t end_ns) {
    add_span("zone", name, start_ns, end_ns);
}

void json_escape(std::string& out, std::string_view s) {
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }
}

} // namespace

void start_capture(std::size_t max_events) {
    Recorder& r = rec();
    {
        std::lock_guard lock(r.mutex);
        r.events.clear();
        r.events.reserve(std::min<std::size_t>(max_events, 1u << 16));
        r.threads.clear();
        r.max_events = max_events;
        r.dropped = 0;
        // The starting thread is thread 0 ("main").
        thread_index(r);
        r.capturing.store(true);
    }
    profiler::set_span_sink(&zone_sink);
}

void stop_capture() {
    profiler::set_span_sink(nullptr);
    rec().capturing.store(false);
}

bool capturing() { return rec().capturing.load(std::memory_order_relaxed); }
bool job_spans_enabled() { return capturing(); }

void add_span(const char* category, std::string name, std::uint64_t start_ns, std::uint64_t end_ns) {
    if (!capturing()) return;
    TraceEvent e;
    e.name = std::move(name);
    e.category = category;
    e.phase = 'X';
    e.start_ns = start_ns;
    e.duration_ns = end_ns >= start_ns ? end_ns - start_ns : 0;
    push(std::move(e));
}

void add_counter(const char* name, std::uint64_t at_ns, double value) {
    if (!capturing()) return;
    TraceEvent e;
    e.name = name;
    e.category = "counter";
    e.phase = 'C';
    e.start_ns = at_ns;
    e.value = value;
    push(std::move(e));
}

void add_draw_calls(std::uint32_t n) { rec().draw_calls.fetch_add(n, std::memory_order_relaxed); }
std::uint32_t take_draw_calls() { return rec().draw_calls.exchange(0, std::memory_order_relaxed); }

void end_frame() {
    const std::uint32_t draws = take_draw_calls();
    if (capturing()) add_counter("draw_calls", profiler::clock_ns(), static_cast<double>(draws));
}

std::vector<TraceEvent> events() {
    Recorder& r = rec();
    std::lock_guard lock(r.mutex);
    return r.events;
}

std::size_t dropped_events() {
    Recorder& r = rec();
    std::lock_guard lock(r.mutex);
    return r.dropped;
}

std::string chrome_trace_json(const std::vector<TraceEvent>& events) {
    std::uint64_t origin = ~std::uint64_t{0};
    std::uint32_t threads = 1;
    for (const TraceEvent& e : events) {
        origin = std::min(origin, e.start_ns);
        threads = std::max(threads, e.thread + 1);
    }
    if (events.empty()) origin = 0;
    std::string out = "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n";
    out += "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"args\":{\"name\":\"cuervo\"}}";
    for (std::uint32_t t = 0; t < threads; ++t) {
        out += std::format(",\n{{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":{},\"args\":{{\"name\":\"{}\"}}}}",
                           t, t == 0 ? std::string("main") : std::format("worker {}", t));
    }
    for (const TraceEvent& e : events) {
        std::string name;
        json_escape(name, e.name);
        const double ts_us = static_cast<double>(e.start_ns - origin) / 1000.0;
        if (e.phase == 'C') {
            out += std::format(",\n{{\"name\":\"{}\",\"cat\":\"{}\",\"ph\":\"C\",\"ts\":{:.3f},\"pid\":1,\"tid\":{},"
                               "\"args\":{{\"value\":{:.3f}}}}}",
                               name, e.category, ts_us, e.thread, e.value);
        } else {
            out += std::format(",\n{{\"name\":\"{}\",\"cat\":\"{}\",\"ph\":\"X\",\"ts\":{:.3f},\"dur\":{:.3f},"
                               "\"pid\":1,\"tid\":{}}}",
                               name, e.category, ts_us, static_cast<double>(e.duration_ns) / 1000.0, e.thread);
        }
    }
    out += "\n]}\n";
    return out;
}

bool write_chrome_trace(const std::filesystem::path& path, std::string* error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (error != nullptr) *error = "cannot open " + path.string();
        return false;
    }
    file << chrome_trace_json(events());
    file.flush();
    if (!file) {
        if (error != nullptr) *error = "write failed: " + path.string();
        return false;
    }
    return true;
}

std::vector<ZoneTotal> zone_totals(const std::vector<TraceEvent>& events) {
    std::map<std::string, ZoneTotal> by_name;
    for (const TraceEvent& e : events) {
        if (e.phase != 'X' || std::string_view(e.category) != "zone") continue;
        ZoneTotal& z = by_name[e.name];
        z.name = e.name;
        z.total_ms += static_cast<double>(e.duration_ns) / 1e6;
        ++z.spans;
    }
    std::vector<ZoneTotal> out;
    out.reserve(by_name.size());
    for (auto& [name, z] : by_name) out.push_back(std::move(z));
    std::ranges::sort(out, [](const ZoneTotal& a, const ZoneTotal& b) { return a.total_ms > b.total_ms; });
    return out;
}

} // namespace cuervo::profile
