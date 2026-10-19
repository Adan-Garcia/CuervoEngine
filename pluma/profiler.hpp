#pragma once

// Script profiler (DESIGN.md 5.14 "a profiler view", "Clarified (Phase 8 --
// Profiler)"): a DebugHooks that uses only the `calls` event and measures,
// per script function, the entries, the inclusive time and the self time
// (inclusive minus the time of script functions it called). Natives a
// function calls count as its self time. Bytecode only: AOT-native code
// (DESIGN.md 5.12) reports no calls.
//
// Install with VM::set_debug_hooks(&profiler). A VM has one DebugHooks at a
// time, so a debugger session (which installs its own) simply doesn't
// profile. One profiler may outlive many VMs (a host makes a fresh VM on
// every hot reload): functions are aggregated by Proto::key, which is stable
// across recompiles.
//
// Coroutines: the VM leaves a coroutine's frames when it suspends and enters
// them again when it resumes, so time spent suspended is not counted, and
// each resume counts as an entry of every resumed frame ("calls" are
// entries). A panic leaves every unwound frame; the stack stays balanced.
//
// Timing reads steady_clock: it observes and never feeds the program.

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "pluma/debug.hpp"

namespace pluma {

class ScriptProfiler final : public DebugHooks {
public:
    // Nanoseconds from an arbitrary origin; nullptr = steady_clock (tests
    // inject a fake clock).
    using Clock = std::uint64_t (*)();
    explicit ScriptProfiler(Clock clock = nullptr);

    std::uint32_t events() const override { return calls; }
    void on_call(VM& vm, const Proto& fn) override;
    void on_return(VM& vm, const Proto& fn) override;

    struct FunctionStats {
        std::string name;     // as in stack traces ("Player.update")
        std::string location; // its first instruction, "scripts/player.plm:12" (empty if unknown)
        std::uint64_t calls = 0;
        std::uint64_t self_ns = 0;
        std::uint64_t inclusive_ns = 0;
        double self_ms() const { return static_cast<double>(self_ns) / 1e6; }
        double inclusive_ms() const { return static_cast<double>(inclusive_ns) / 1e6; }
    };

    // Zeroes every count and time; frames open right now keep running and
    // count from this moment.
    void reset();
    // Functions entered since the last reset, by self time (highest first,
    // then by name).
    std::vector<FunctionStats> snapshot() const;
    // Time inside any script function (the sum of every self time).
    std::uint64_t total_ns() const;
    // Open frames (0 between calls into the VM).
    std::size_t depth() const { return stack_.size(); }
    // A table of the `top` functions by self time, with % of script time.
    std::string report(std::size_t top = 20) const;

private:
    struct Entry {
        std::uint32_t stat;
        std::uint64_t start;
        std::uint64_t child_ns; // inclusive time of script callees
    };
    struct Stat {
        std::string key; // Proto::key (or the name when it has none)
        FunctionStats s;
        std::uint32_t active = 0; // open frames (recursion counts once)
    };
    std::uint32_t stat_of(const Proto& fn);
    void close(const Entry& e, std::uint64_t now);
    std::uint64_t now() const;

    Clock clock_;
    std::vector<Stat> stats_;
    // Lookups only, never iterated: Proto::key -> stats_, and a cache by
    // Proto address (checked against the key on every hit).
    std::unordered_map<std::string, std::uint32_t> by_key_;
    std::unordered_map<const Proto*, std::uint32_t> by_proto_;
    std::vector<Entry> stack_;
};

} // namespace pluma
