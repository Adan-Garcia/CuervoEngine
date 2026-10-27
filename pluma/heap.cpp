#include "pluma/heap.hpp"

#include <algorithm>
#include <vector>

#include "pluma/bytecode.hpp"
#include "pluma/heap_internal.hpp"

namespace pluma {

HeapState g_heap;

namespace {

constexpr std::uint8_t black = 0;
constexpr std::uint8_t gray = 1;
constexpr std::uint8_t white = 2;

std::uint8_t color(const Object* o) { return o->flags & objflag::ColorMask; }
void set_color(Object* o, std::uint8_t c) {
    o->flags = static_cast<std::uint8_t>((o->flags & ~objflag::ColorMask) | c);
}
// An edge the collector follows: a counted reference to a traced object.
bool traced(const Value& v) { return v.tag == Tag::Obj && (v.o->flags & objflag::Untraced) == 0; }

void delete_object(Object* o) {
    switch (o->kind) {
        case ObjKind::Str: delete static_cast<StrObj*>(o); return;
        case ObjKind::List:
        case ObjKind::Array: delete static_cast<ListObj*>(o); return;
        case ObjKind::Map:
        case ObjKind::Set: delete static_cast<HashObj*>(o); return;
        case ObjKind::Deque: delete static_cast<DequeObj*>(o); return;
        case ObjKind::PQueue: delete static_cast<PQueueObj*>(o); return;
        case ObjKind::StrBuilder: delete static_cast<StrBuilderObj*>(o); return;
        case ObjKind::Instance: delete static_cast<InstanceObj*>(o); return;
        case ObjKind::Enum: delete static_cast<EnumObj*>(o); return;
        case ObjKind::Closure: delete static_cast<ClosureObj*>(o); return;
        case ObjKind::Cell: delete static_cast<CellObj*>(o); return;
        case ObjKind::Vec: delete static_cast<VecObj*>(o); return;
        case ObjKind::Coro: delete static_cast<CoroObj*>(o); return;
        case ObjKind::Signal: delete static_cast<SignalObj*>(o); return;
        case ObjKind::Weak: delete static_cast<WeakObj*>(o); return;
    }
}

// Drops the references a dead shell still lists (already released).
void clear_contents(Object* o) {
    switch (o->kind) {
        case ObjKind::List:
        case ObjKind::Array: std::vector<Value>().swap(static_cast<ListObj*>(o)->items); return;
        case ObjKind::Map:
        case ObjKind::Set: {
            auto* h = static_cast<HashObj*>(o);
            std::vector<Value>().swap(h->keys);
            std::vector<Value>().swap(h->vals);
            std::vector<std::uint32_t>().swap(h->table);
            h->live = 0;
            return;
        }
        case ObjKind::Deque: {
            auto* d = static_cast<DequeObj*>(o);
            std::vector<Value>().swap(d->buf);
            d->head = d->count = 0;
            return;
        }
        case ObjKind::PQueue: {
            auto* q = static_cast<PQueueObj*>(o);
            std::vector<Value>().swap(q->items);
            q->seq.clear();
            q->less = Value{};
            return;
        }
        case ObjKind::Instance: std::vector<Value>().swap(static_cast<InstanceObj*>(o)->fields); return;
        case ObjKind::Enum: std::vector<Value>().swap(static_cast<EnumObj*>(o)->fields); return;
        case ObjKind::Closure: std::vector<Value>().swap(static_cast<ClosureObj*>(o)->captures); return;
        case ObjKind::Cell: static_cast<CellObj*>(o)->value = Value{}; return;
        case ObjKind::Signal: std::vector<Value>().swap(static_cast<SignalObj*>(o)->handlers); return;
        case ObjKind::Coro: {
            auto* c = static_cast<CoroObj*>(o);
            c->wait_value = c->resume_value = c->result = Value{};
            std::vector<Value>().swap(c->regs);
            std::vector<Value>().swap(c->start_args);
            std::vector<CoroObj::SavedFrame>().swap(c->frames);
            c->owner = nullptr;
            return;
        }
        default: return;
    }
}

// The target of `o`'s weak cell (if any) died: the cell now reads null.
void clear_weak_target(Object* o) {
    auto it = g_heap.weak.find(o);
    if (it != g_heap.weak.end()) {
        it->second->target = nullptr;
        g_heap.weak.erase(it);
    }
    o->flags &= static_cast<std::uint8_t>(~objflag::HasWeak);
}

// Work done before an object's memory goes away, whoever frees it.
void finalize(Object* o) {
    if ((o->flags & objflag::HasWeak) != 0) clear_weak_target(o);
    if (o->kind == ObjKind::Instance) {
        auto* i = static_cast<InstanceObj*>(o);
        if (i->native != nullptr && i->type->finalizer) i->type->finalizer(i->native);
        i->native = nullptr;
    } else if (o->kind == ObjKind::Weak) {
        auto* w = static_cast<WeakObj*>(o);
        if (w->target != nullptr) {
            g_heap.weak.erase(w->target);
            w->target->flags &= static_cast<std::uint8_t>(~objflag::HasWeak);
            w->target = nullptr;
        }
    }
}

// ---------------------------------------------------------------- collector

// One candidate set: the whole buffer (see heap.hpp).
void collect_set(bool check_acyclic, CollectResult& result) {
    std::vector<Object*> roots;
    roots.swap(g_heap.suspects);
    std::uint64_t work = 0;
    std::vector<Object*> stack;

    // MarkGray: subtract the references internal to the subgraph.
    std::size_t n = 0;
    for (Object* o : roots) {
        ++work;
        if ((o->flags & objflag::Dead) != 0) { // freed while buffered
            delete_object(o);
            continue;
        }
        roots[n++] = o;
        if (color(o) == gray) continue;
        set_color(o, gray);
        stack.push_back(o);
        while (!stack.empty()) {
            Object* s = stack.back();
            stack.pop_back();
            for_each_child(s, [&](const Value& v) {
                if (!traced(v)) return;
                ++work;
                Object* c = v.o;
                --c->rc;
                if (color(c) != gray) {
                    set_color(c, gray);
                    stack.push_back(c);
                }
            });
        }
    }
    roots.resize(n);

    // Scan: whatever still has a count is referenced from outside; it and
    // everything it reaches are live again (ScanBlack restores the counts).
    std::vector<Object*> black_stack;
    auto scan_black = [&](Object* s0) {
        set_color(s0, black);
        black_stack.push_back(s0);
        while (!black_stack.empty()) {
            Object* s = black_stack.back();
            black_stack.pop_back();
            for_each_child(s, [&](const Value& v) {
                if (!traced(v)) return;
                ++work;
                Object* c = v.o;
                ++c->rc;
                if (color(c) != black) {
                    set_color(c, black);
                    black_stack.push_back(c);
                }
            });
        }
    };
    for (Object* r : roots) {
        stack.push_back(r);
        while (!stack.empty()) {
            Object* s = stack.back();
            stack.pop_back();
            if (color(s) != gray) continue;
            ++work;
            if (s->rc > 0) {
                scan_black(s);
                continue;
            }
            set_color(s, white);
            for_each_child(s, [&](const Value& v) {
                if (traced(v) && color(v.o) == gray) stack.push_back(v.o);
            });
        }
    }

    // CollectWhite: the white objects are garbage. Depth-first from the roots
    // in buffer order: the deterministic order they are finalized in.
    for (Object* r : roots) r->flags &= static_cast<std::uint8_t>(~objflag::Buffered);
    std::vector<Object*> garbage;
    for (Object* r : roots) {
        stack.push_back(r);
        while (!stack.empty()) {
            Object* s = stack.back();
            stack.pop_back();
            if (color(s) != white) continue;
            ++work;
            set_color(s, black);
            garbage.push_back(s);
            for_each_child(s, [&](const Value& v) {
                if (traced(v) && color(v.o) == white) stack.push_back(v.o);
            });
        }
    }

    // Free. References between garbage objects (and from garbage to live
    // traced objects, whose counts MarkGray already lowered) are not
    // released; untraced children are released normally.
    for (Object* g : garbage) {
        if (check_acyclic && (g->flags & objflag::Promised) != 0 && result.acyclic_violation.empty() &&
            g->kind == ObjKind::Instance) {
            result.acyclic_violation = static_cast<InstanceObj*>(g)->type->name;
        }
        finalize(g);
    }
    // Gathered before any is released: a release can free a live object
    // that a later garbage object still points at (its count was already
    // lowered by MarkGray), and reading its flags then would be a
    // use-after-free.
    std::vector<Value> untraced;
    for (Object* g : garbage) {
        for_each_child(g, [&](const Value& v) {
            if (v.tag == Tag::Obj && (v.o->flags & objflag::Untraced) != 0) untraced.push_back(v);
        });
    }
    for (const Value& v : untraced) release(v);
    for (Object* g : garbage) {
        delete_object(g);
        --g_heap.live;
    }
    g_heap.collected += garbage.size();
    g_heap.work += work;
    ++g_heap.sets;
    result.work += work;
    result.freed += garbage.size();
    ++result.sets;
}

} // namespace

HeapState::~HeapState() {
    for (InstanceObj* o : free_instances) delete o;
    for (ListObj* o : free_lists) delete o;
    for (Object* o : retired) delete_object(o);
}

void possible_root(Object* o) {
    o->flags |= objflag::Buffered;
    g_heap.suspects.push_back(o);
}

// Iterative, so freeing a long chain of objects can't overflow the C++ stack.
void free_object(Object* first) {
    std::vector<Object*> work{first};
    while (!work.empty()) {
        Object* o = work.back();
        work.pop_back();
        finalize(o);
        for_each_child(o, [&](const Value& v) {
            if (v.tag != Tag::Obj) return;
            Object* c = v.o;
            if (--c->rc == 0) {
                work.push_back(c);
            } else if ((c->flags & (objflag::Buffered | objflag::Untraced)) == 0) {
                possible_root(c);
            }
        });
        --g_heap.live;
        if ((o->flags & objflag::Buffered) != 0) {
            // Still listed in the suspect buffer: the next set deletes it.
            o->flags |= objflag::Dead;
            clear_contents(o);
        } else {
            delete_object(o);
        }
    }
}

HeapStats heap_stats() {
    HeapStats s;
    s.live = g_heap.live;
    s.manual = g_heap.manual;
    s.suspects = g_heap.suspects.size();
    s.collected = g_heap.collected;
    s.sets = g_heap.sets;
    s.work = g_heap.work;
    return s;
}

CollectResult collect_cycles(std::uint64_t budget, std::int64_t& credit, bool check_acyclic) {
    CollectResult result;
    auto b = static_cast<std::int64_t>(std::min<std::uint64_t>(budget, std::uint64_t{1} << 62));
    credit = std::min(credit + b, b);
    while (credit > 0 && !g_heap.suspects.empty()) {
        std::uint64_t before = result.work;
        collect_set(check_acyclic, result);
        credit -= static_cast<std::int64_t>(result.work - before);
    }
    return result;
}

CollectResult collect_all_cycles(bool check_acyclic) {
    CollectResult result;
    // Freeing garbage can release more suspects (through acyclic objects);
    // the loop ends because every set frees or unbuffers its roots.
    while (!g_heap.suspects.empty()) collect_set(check_acyclic, result);
    return result;
}

// ---------------------------------------------------------------- weak references

Value weak_store(const Value& v) {
    if (v.tag != Tag::Obj) return v;
    if (v.ref != 0) return v; // manual: the generation check is the weak test
    Object* t = v.o;
    if (is_manual(t)) return manual_ref(t);
    auto it = g_heap.weak.find(t);
    WeakObj* w = nullptr;
    if (it != g_heap.weak.end()) {
        w = it->second;
    } else {
        w = new WeakObj;
        w->kind = ObjKind::Weak;
        w->flags = objflag::Untraced;
        w->target = t;
        ++g_heap.live;
        g_heap.weak.emplace(t, w);
        t->flags |= objflag::HasWeak;
    }
    Value out = Value::object(w);
    retain(out);
    return out;
}

Value weak_load(const Value& stored) {
    if (stored.tag != Tag::Obj) return stored;
    if (stored.ref != 0) return is_stale(stored) ? Value::null() : stored;
    if (stored.o->kind != ObjKind::Weak) return stored;
    Object* t = static_cast<WeakObj*>(stored.o)->target;
    return t != nullptr ? Value::object(t) : Value::null();
}

// ---------------------------------------------------------------- manual objects

namespace {

std::uint8_t manual_flags(ArenaKind arena) {
    return static_cast<std::uint8_t>(objflag::Manual | objflag::Untraced | static_cast<std::uint8_t>(arena));
}

} // namespace

InstanceObj* manual_instance(const RecordRT* type, std::size_t field_count, ArenaKind arena) {
    InstanceObj* o = nullptr;
    if (!g_heap.free_instances.empty()) {
        o = g_heap.free_instances.back();
        g_heap.free_instances.pop_back();
    } else {
        o = new InstanceObj;
        o->kind = ObjKind::Instance;
        o->rc = manual_rc_bias;
    }
    o->flags = manual_flags(arena);
    o->type = type;
    o->native = nullptr;
    o->fields.assign(field_count, Value{});
    ++g_heap.live;
    ++g_heap.manual;
    return o;
}

ListObj* manual_list(ArenaKind arena) {
    ListObj* o = nullptr;
    if (!g_heap.free_lists.empty()) {
        o = g_heap.free_lists.back();
        g_heap.free_lists.pop_back();
    } else {
        o = new ListObj;
        o->kind = ObjKind::List;
        o->rc = manual_rc_bias;
    }
    o->flags = manual_flags(arena);
    o->items.clear();
    ++g_heap.live;
    ++g_heap.manual;
    return o;
}

void destroy_manual(Object* o, const FreeSite& site) {
    finalize(o);
    // Release what it holds. The slots are nulled first, so code reached
    // through a release (none today: no script destructors) sees no
    // dangling values.
    std::vector<Value> held;
    if (o->kind == ObjKind::Instance) {
        auto& fields = static_cast<InstanceObj*>(o)->fields;
        held = fields;
        std::fill(fields.begin(), fields.end(), Value{}); // size kept: hosts may still index it
    } else if (o->kind == ObjKind::List) {
        held.swap(static_cast<ListObj*>(o)->items);
    }
    ++o->gen;
    o->flags = static_cast<std::uint8_t>((o->flags & objflag::ColorMask) | objflag::Manual | objflag::Untraced |
                                         objflag::Dead);
    g_heap.free_sites[o] = site;
    --g_heap.live;
    --g_heap.manual;
    for (const Value& v : held) release(v);
    if (o->gen == 0xffff) {
        // A checked reference holds at most 0xfffe: the slot is never reused.
        g_heap.retired.push_back(o);
    } else if (o->kind == ObjKind::Instance) {
        g_heap.free_instances.push_back(static_cast<InstanceObj*>(o));
    } else {
        g_heap.free_lists.push_back(static_cast<ListObj*>(o));
    }
}

const FreeSite* free_site(const Object* o) {
    auto it = g_heap.free_sites.find(o);
    return it != g_heap.free_sites.end() ? &it->second : nullptr;
}

} // namespace pluma
