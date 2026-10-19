#include "pluma/profiler.hpp"

#include <algorithm>
#include <chrono>
#include <format>

#include "pluma/bytecode.hpp"

namespace pluma {

namespace {

std::uint64_t steady_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

ScriptProfiler::ScriptProfiler(Clock clock) : clock_(clock != nullptr ? clock : &steady_ns) {}

std::uint64_t ScriptProfiler::now() const { return clock_(); }

std::uint32_t ScriptProfiler::stat_of(const Proto& fn) {
    const std::string& key = fn.key.empty() ? fn.name : fn.key;
    // A cached entry must still name the same function: after a hot reload
    // the old VM's Protos are freed and a new Proto may reuse an address.
    if (auto it = by_proto_.find(&fn); it != by_proto_.end() && stats_[it->second].key == key) return it->second;
    auto [it, added] = by_key_.try_emplace(key, static_cast<std::uint32_t>(stats_.size()));
    if (added) {
        Stat st;
        st.key = key;
        st.s.name = fn.name;
        std::string_view file;
        std::uint32_t line = 0;
        std::uint32_t column = 0;
        if (fn.resolve_pc(0, file, line, column)) st.s.location = std::format("{}:{}", file, line);
        stats_.push_back(std::move(st));
    }
    by_proto_.insert_or_assign(&fn, it->second);
    return it->second;
}

void ScriptProfiler::on_call(VM& vm, const Proto& fn) {
    (void)vm;
    const std::uint32_t stat = stat_of(fn);
    Stat& st = stats_[stat];
    ++st.s.calls;
    ++st.active;
    stack_.push_back({stat, now(), 0});
}

void ScriptProfiler::close(const Entry& e, std::uint64_t t) {
    const std::uint64_t inclusive = t >= e.start ? t - e.start : 0;
    Stat& st = stats_[e.stat];
    st.s.self_ns += inclusive >= e.child_ns ? inclusive - e.child_ns : 0;
    if (st.active > 0) --st.active;
    // A recursive function's inclusive time is its outermost frame's.
    if (st.active == 0) st.s.inclusive_ns += inclusive;
    if (!stack_.empty()) stack_.back().child_ns += inclusive;
}

void ScriptProfiler::on_return(VM& vm, const Proto& fn) {
    (void)vm;
    // A frame entered before the hooks were installed returns with no entry
    // of ours (the stack is then empty or ends elsewhere): ignore it.
    if (stack_.empty()) return;
    const std::uint32_t stat = stat_of(fn);
    auto it = std::find_if(stack_.rbegin(), stack_.rend(), [&](const Entry& e) { return e.stat == stat; });
    if (it == stack_.rend()) return;
    const std::uint64_t t = now();
    // Normally the top; if events were lost in between, close those too.
    const std::size_t target = static_cast<std::size_t>(stack_.rend() - it) - 1;
    while (stack_.size() > target) {
        const Entry e = stack_.back();
        stack_.pop_back();
        close(e, t);
    }
}

void ScriptProfiler::reset() {
    // `active` stays: the open frames are still open.
    for (Stat& st : stats_) {
        st.s.calls = 0;
        st.s.self_ns = 0;
        st.s.inclusive_ns = 0;
    }
    const std::uint64_t t = now();
    for (Entry& e : stack_) {
        e.start = t;
        e.child_ns = 0;
    }
}

std::vector<ScriptProfiler::FunctionStats> ScriptProfiler::snapshot() const {
    std::vector<FunctionStats> out;
    for (const Stat& st : stats_) {
        if (st.s.calls > 0 || st.s.self_ns > 0) out.push_back(st.s);
    }
    std::sort(out.begin(), out.end(), [](const FunctionStats& a, const FunctionStats& b) {
        if (a.self_ns != b.self_ns) return a.self_ns > b.self_ns;
        return a.name < b.name;
    });
    return out;
}

std::uint64_t ScriptProfiler::total_ns() const {
    std::uint64_t total = 0;
    for (const Stat& st : stats_) total += st.s.self_ns;
    return total;
}

std::string ScriptProfiler::report(std::size_t top) const {
    const std::vector<FunctionStats> all = snapshot();
    const std::uint64_t total = total_ns();
    std::string out = std::format("script time: {:.3f} ms in {} function(s)\n", static_cast<double>(total) / 1e6,
                                  all.size());
    out += std::format("  {:<52} {:>9} {:>11} {:>11} {:>7}\n", "function", "calls", "self ms", "incl ms", "% self");
    for (std::size_t i = 0; i < all.size() && i < top; ++i) {
        const FunctionStats& f = all[i];
        const double pct = total > 0 ? 100.0 * static_cast<double>(f.self_ns) / static_cast<double>(total) : 0.0;
        std::string label = f.name;
        if (!f.location.empty()) label += " (" + f.location + ")";
        out += std::format("  {:<52} {:>9} {:>11.3f} {:>11.3f} {:>6.1f}%\n", label, f.calls, f.self_ms(),
                           f.inclusive_ms(), pct);
    }
    if (all.size() > top) out += std::format("  ... {} more\n", all.size() - top);
    return out;
}

} // namespace pluma
