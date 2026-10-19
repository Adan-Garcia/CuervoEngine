#pragma once

// The heap beyond plain reference counting (DESIGN.md 5.10): the cycle
// collector, weak references and the storage of manual objects.
//
// Cycle collector: Bacon & Rajan's synchronous trial deletion ("Concurrent
// Cycle Collection in Reference Counted Systems", 2001, section 3) over a
// suspect buffer.
//   - A release that leaves a count above zero buffers the object (once) as a
//     possible root of a garbage cycle, unless it is Untraced: its type is
//     acyclic (pluma/acyclic.hpp), it is a string, vector, weak cell or
//     closure without captures, or it is manual.
//   - A *candidate set* is the whole buffer at the moment it is taken. It is
//     processed to completion without running script code: MarkGray
//     subtracts internal references, Scan finds what is still referenced
//     from outside (and restores it with ScanBlack), CollectWhite frees the
//     rest. Mutators never run while a set is in flight, so there is no
//     write barrier and no interleaving to repair: between steps the buffer
//     only accumulates suspects (an object freed while buffered becomes a
//     shell that the next set deletes).
//   - Incremental by *work units*, never time, so it is deterministic: one
//     unit per object colored or edge followed. `collect_cycles(budget)`
//     adds `budget` to the caller's credit (capped at `budget`; the VM keeps
//     one, so a new game starts from the same state) and runs whole sets
//     while the credit is positive; a set that costs more than the credit
//     leaves it negative, and the following steps repay it, so the average
//     cost per step stays at most `budget`.
//   - Order: garbage is found depth-first from the roots in buffer order and
//     freed in that order (first weak references are nulled and native
//     finalizers run for every object, then the memory is released), so
//     finalization is deterministic. There are no script-level destructors.
//
// Single-threaded, like scripts (DESIGN.md 4.14). All state is global
// because objects don't know their VM (hot reload shares strings between
// VMs). The collector never reads an object's type except to run its
// finalizer, so objects that outlive their VM's type tables stay safe.

#include <cstddef>
#include <cstdint>
#include <string>

#include "pluma/value.hpp"

namespace pluma {

struct HeapStats {
    std::size_t live = 0;          // live objects (automatic and manual)
    std::size_t manual = 0;        // live manual objects
    std::size_t suspects = 0;      // objects in the suspect buffer
    std::uint64_t collected = 0;   // objects freed by the cycle collector, in total
    std::uint64_t sets = 0;        // candidate sets processed, in total
    std::uint64_t work = 0;        // work units spent, in total
};
HeapStats heap_stats();

struct CollectResult {
    std::uint64_t work = 0;  // units spent by this call
    std::size_t freed = 0;   // objects freed by this call
    std::size_t sets = 0;    // candidate sets processed by this call
    // Development builds trace `@acyclic` classes the compiler could not
    // prove acyclic; the first one found in a garbage cycle is named here.
    std::string acyclic_violation;
};

// One incremental step (see above). `check_acyclic`: report broken
// `@acyclic` promises. Call it only when no script code is running.
CollectResult collect_cycles(std::uint64_t budget, std::int64_t& credit, bool check_acyclic = true);
// Runs candidate sets until the buffer is empty (tests, VM teardown).
CollectResult collect_all_cycles(bool check_acyclic = true);

// ---- weak references (`weak var` fields)

// What a weak field stores for `v` (owned): a WeakObj for an automatic
// object, the checked reference itself for a manual one, else `v`.
Value weak_store(const Value& v);
// The value a weak field reads as (borrowed: assign() it): the target, or
// null once it was freed.
Value weak_load(const Value& stored);

// ---- manual objects (DESIGN.md 5.10)

// Why and where a manual object was freed, for use-after-free panics.
struct FreeSite {
    const void* proto = nullptr; // pluma::Proto of an explicit `free`, else null
    std::uint32_t pc = 0;
    ArenaKind reset = ArenaKind::None;   // freed by this arena's reset (frame, scene, VM teardown)
    std::uint64_t tick = 0;      // VM tick of the reset
};

// New manual objects: count biased (never freed by release), untraced, with
// the arena in the color bits. Freed slots are reused (same kind) with the
// next generation; a slot whose generation is exhausted is retired, so a
// checked reference can never match a later occupant.
InstanceObj* manual_instance(const RecordRT* type, std::size_t field_count, ArenaKind arena);
ListObj* manual_list(ArenaKind arena);
// Frees a live manual object: nulls weak references, runs the finalizer,
// releases what it references (DESIGN.md 5.10: "releases them when freed"),
// bumps the generation and records `site`.
void destroy_manual(Object* o, const FreeSite& site);
// The last free of `o` (null if never freed).
const FreeSite* free_site(const Object* o);
inline ArenaKind arena_of(const Object* o) { return static_cast<ArenaKind>(o->flags & objflag::ColorMask); }
inline bool is_manual(const Object* o) { return (o->flags & objflag::Manual) != 0; }

} // namespace pluma
