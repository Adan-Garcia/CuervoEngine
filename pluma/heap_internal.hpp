#pragma once

// Internal state of pluma/heap.cpp, shared with value.cpp (allocation
// counting). Not part of the language's public API.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "pluma/heap.hpp"
#include "pluma/value.hpp"

namespace pluma {

struct HeapState {
    HeapState() = default;
    HeapState(const HeapState&) = delete;
    HeapState& operator=(const HeapState&) = delete;
    ~HeapState();

    std::size_t live = 0;
    std::size_t manual = 0;
    std::uint64_t collected = 0;
    std::uint64_t sets = 0;
    std::uint64_t work = 0;

    // Bacon-Rajan's root buffer, in the order objects became suspects.
    std::vector<Object*> suspects;
    // Weak reference cell of each target that has one. Only looked up, never
    // iterated (determinism, DESIGN.md 4.1).
    std::unordered_map<const Object*, WeakObj*> weak;
    // Manual objects: free slots by kind, retired slots, last free site.
    std::vector<InstanceObj*> free_instances;
    std::vector<ListObj*> free_lists;
    std::vector<Object*> retired;
    std::unordered_map<const Object*, FreeSite> free_sites;
};

extern HeapState g_heap;

// Calls `f(const Value&)` for every reference `o` holds (counted ones: the
// coroutine owner is one, see CoroObj::owner).
template <class F>
void for_each_child(Object* o, F&& f) {
    switch (o->kind) {
        case ObjKind::Str:
        case ObjKind::Vec:
        case ObjKind::Weak: return;
        case ObjKind::StrBuilder: return;
        case ObjKind::List:
        case ObjKind::Array:
            for (const Value& v : static_cast<ListObj*>(o)->items) f(v);
            return;
        case ObjKind::Map:
        case ObjKind::Set: {
            auto* h = static_cast<HashObj*>(o);
            for (const Value& v : h->keys) f(v);
            for (const Value& v : h->vals) f(v);
            return;
        }
        case ObjKind::Deque: {
            // Unused slots are Nil: visiting the whole buffer is enough.
            for (const Value& v : static_cast<DequeObj*>(o)->buf) f(v);
            return;
        }
        case ObjKind::PQueue: {
            auto* q = static_cast<PQueueObj*>(o);
            for (const Value& v : q->items) f(v);
            f(q->less);
            return;
        }
        case ObjKind::Instance:
            for (const Value& v : static_cast<InstanceObj*>(o)->fields) f(v);
            return;
        case ObjKind::Enum:
            for (const Value& v : static_cast<EnumObj*>(o)->fields) f(v);
            return;
        case ObjKind::Closure:
            for (const Value& v : static_cast<ClosureObj*>(o)->captures) f(v);
            return;
        case ObjKind::Cell: f(static_cast<CellObj*>(o)->value); return;
        case ObjKind::Signal:
            for (const Value& v : static_cast<SignalObj*>(o)->handlers) f(v);
            return;
        case ObjKind::Coro: {
            auto* c = static_cast<CoroObj*>(o);
            f(c->wait_value);
            f(c->resume_value);
            f(c->result);
            for (const Value& v : c->regs) f(v);
            for (const Value& v : c->start_args) f(v);
            for (const CoroObj::SavedFrame& sf : c->frames) {
                if (sf.closure != nullptr) f(Value::object(const_cast<ClosureObj*>(sf.closure)));
            }
            if (c->owner != nullptr) f(Value::object(c->owner));
            return;
        }
    }
}

} // namespace pluma
