#include "pluma/value.hpp"

#include <vector>

#include "pluma/bytecode.hpp"
#include "pluma/heap_internal.hpp"

namespace pluma {

namespace {

template <class T>
T* track(T* o, ObjKind kind, std::uint8_t flags = 0) {
    o->kind = kind;
    o->flags = flags;
    ++g_heap.live;
    return o;
}

} // namespace

std::size_t live_objects() { return g_heap.live; }

// Strings and vectors hold no references: never part of a cycle.
StrObj* new_str(std::string s) {
    auto* o = track(new StrObj, ObjKind::Str, objflag::Untraced);
    o->s = std::move(s);
    return o;
}

ListObj* new_list(bool untraced) { return track(new ListObj, ObjKind::List, untraced ? objflag::Untraced : 0); }

InstanceObj* new_instance(const RecordRT* type, std::size_t field_count) {
    auto* o = track(new InstanceObj, ObjKind::Instance, type->gc_flags);
    o->type = type;
    o->fields.resize(field_count);
    return o;
}

EnumObj* new_enum(const EnumRT* type, std::uint32_t variant, std::size_t field_count) {
    auto* o = track(new EnumObj, ObjKind::Enum, field_count == 0 ? objflag::Untraced : type->gc_flags);
    o->type = type;
    o->variant = variant;
    o->fields.resize(field_count);
    return o;
}

// A closure without captures is a leaf.
ClosureObj* new_closure(const Proto* fn, std::size_t capture_count) {
    auto* o = track(new ClosureObj, ObjKind::Closure, capture_count == 0 ? objflag::Untraced : 0);
    o->fn = fn;
    o->captures.resize(capture_count);
    return o;
}

CellObj* new_cell(bool untraced) { return track(new CellObj, ObjKind::Cell, untraced ? objflag::Untraced : 0); }
SignalObj* new_signal() { return track(new SignalObj, ObjKind::Signal); }
CoroObj* new_coro() { return track(new CoroObj, ObjKind::Coro); }

ListObj* new_array(bool untraced) { return track(new ListObj, ObjKind::Array, untraced ? objflag::Untraced : 0); }
HashObj* new_hash(ObjKind kind, bool untraced) { return track(new HashObj, kind, untraced ? objflag::Untraced : 0); }
DequeObj* new_deque(bool untraced) { return track(new DequeObj, ObjKind::Deque, untraced ? objflag::Untraced : 0); }
PQueueObj* new_pqueue() { return track(new PQueueObj, ObjKind::PQueue); }
StrBuilderObj* new_str_builder() { return track(new StrBuilderObj, ObjKind::StrBuilder, objflag::Untraced); }

const EnumRT* result_rt() {
    static const EnumRT rt = [] {
        EnumRT e;
        e.name = "Result";
        e.plain = false;
        e.variants.push_back(VariantRT{"Ok", {"value"}, 0});
        e.variants.push_back(VariantRT{"Err", {"error"}, 1});
        return e;
    }();
    return &rt;
}

Value vec_value(const float* data, std::uint32_t n) {
    if (n <= 2) {
        Value v;
        v.tag = Tag::Vec;
        v.aux = n;
        v.v2[0] = data[0];
        v.v2[1] = n == 2 ? data[1] : 0.0f;
        return v;
    }
    auto* o = track(new VecObj, ObjKind::Vec, objflag::Untraced);
    o->c.assign(data, data + n);
    return Value::object(o);
}

} // namespace pluma
