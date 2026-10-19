#pragma once

// Run-time values and heap objects of the VM.
//
// A Value is a 16-byte tagged slot. Types are static, so instructions know
// what they operate on; the tag exists for generic (type-erased) code,
// refcounting and printing. Integers of every kind are stored as int64 bits
// (see numeric.hpp); f32 as float, f64 as double.
//
// Heap objects are reference-counted (DESIGN.md 5.10). Values are plain data:
// whoever stores a Value into a slot retains it and releases the old one
// (see `assign`). A release that leaves the count above zero makes the object
// a cycle-collector suspect (pluma/heap.hpp) unless its type is acyclic.
//
// Manual objects (`alloc`, `new(frame)`, `new(scene)`, `manual class`,
// DESIGN.md 5.10) are not refcounted: their count starts at `manual_rc_bias`
// so retain/release never free them, and script references to them are
// *checked*: `Value::ref == 1` and `aux` holds the object's generation, which
// `free` bumps, so a stale reference is detected on use.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "pluma/sema.hpp"

namespace pluma {

enum class Tag : std::uint8_t {
    Nil,  // uninitialized / void
    Null,
    Bool,
    I8, I16, I32, I64, U8, U16, U32, U64, F32, F64, // in NumKind order
    Name, // interned name: i = id
    Enum, // plain enum: i = discriminant, aux = enum index
    Obj,
    Vec,  // vec1 / vec2 stored inline: aux = size, components in `v2`. Larger vectors are VecObj.
};

constexpr Tag num_tag(NumKind k) { return static_cast<Tag>(static_cast<int>(Tag::I8) + static_cast<int>(k)); }
constexpr bool is_num_tag(Tag t) { return t >= Tag::I8 && t <= Tag::F64; }
constexpr NumKind tag_num(Tag t) { return static_cast<NumKind>(static_cast<int>(t) - static_cast<int>(Tag::I8)); }

struct Object;

struct Value {
    Tag tag = Tag::Nil;
    // Tag::Obj only: 1 = a checked reference to a manual object, `aux` = the
    // generation it was taken at (DESIGN.md 5.10 "pointer + generation").
    std::uint8_t ref = 0;
    std::uint16_t pad1 = 0;
    std::uint32_t aux = 0;
    union {
        std::int64_t i = 0;
        float f;
        double d;
        bool b;
        Object* o;
        float v2[2];
    };

    static Value null() {
        Value v;
        v.tag = Tag::Null;
        return v;
    }
    static Value boolean(bool x) {
        Value v;
        v.tag = Tag::Bool;
        v.b = x;
        return v;
    }
    static Value integer(NumKind k, std::int64_t x) {
        Value v;
        v.tag = num_tag(k);
        v.i = x;
        return v;
    }
    static Value f32(float x) {
        Value v;
        v.tag = Tag::F32;
        v.f = x;
        return v;
    }
    static Value f64(double x) {
        Value v;
        v.tag = Tag::F64;
        v.d = x;
        return v;
    }
    static Value object(Object* p) {
        Value v;
        v.tag = Tag::Obj;
        v.o = p;
        return v;
    }
    bool is_obj() const { return tag == Tag::Obj; }
    bool is_null() const { return tag == Tag::Null || tag == Tag::Nil; }
};
static_assert(sizeof(Value) == 16);

// Array is a ListObj of fixed length with value semantics ([T; N]); Map and
// Set share HashObj; Result values are EnumObjs of `result_rt()`.
enum class ObjKind : std::uint8_t {
    Str, List, Instance, Enum, Closure, Cell, Vec, Coro, Signal, Weak,
    Array, Map, Set, Deque, PQueue, StrBuilder,
};

// Object::flags. The low two bits are the collector's color for automatic
// objects and the arena (pluma::ArenaKind) for manual ones.
namespace objflag {
constexpr std::uint8_t ColorMask = 0x03;
constexpr std::uint8_t Buffered = 0x04; // in the suspect buffer
constexpr std::uint8_t Untraced = 0x08; // acyclic (or manual): never a suspect, never traced
constexpr std::uint8_t Manual = 0x10;
// Automatic: an `@acyclic` class the compiler could not prove acyclic; traced
// in development so a promise broken by a garbage cycle is reported.
// Manual: the object owns coroutines (cancelled when it is freed).
constexpr std::uint8_t Promised = 0x20;
constexpr std::uint8_t HasWeak = 0x40; // a WeakObj points at it
// Automatic: freed while in the suspect buffer; the collector deletes the
// shell. Manual: freed (a tombstone until the slot is reused).
constexpr std::uint8_t Dead = 0x80;
} // namespace objflag

constexpr std::uint32_t manual_rc_bias = 0x80000000u;

struct Object {
    std::uint32_t rc = 0;
    ObjKind kind = ObjKind::Str;
    std::uint8_t flags = 0;
    std::uint16_t gen = 0; // manual objects: generation, bumped by free
};
static_assert(sizeof(Object) == 8);

struct RecordRT;
struct EnumRT;
struct Proto;

struct StrObj : Object {
    std::string s;
};

struct ListObj : Object {
    std::vector<Value> items;
};

// Class instance or (boxed) struct value. `native` is host data attached to
// an instance of a class with `@native` methods (pluma/bindings.hpp); the
// class's finalizer frees it with the instance.
struct InstanceObj : Object {
    const RecordRT* type = nullptr;
    std::vector<Value> fields;
    void* native = nullptr;
};

// A value of an enum with payloads (immutable, so it can be shared).
struct EnumObj : Object {
    const EnumRT* type = nullptr;
    std::uint32_t variant = 0;
    std::vector<Value> fields;
};

struct ClosureObj : Object {
    const Proto* fn = nullptr;
    std::vector<Value> captures; // cells for captured `var`s, values otherwise
};

// A captured mutable local (closures capture by reference, LANGUAGE.md 2.4).
struct CellObj : Object {
    Value value;
};

// vecN with N > 2 (smaller vectors are inline values). Immutable, so it is
// shared freely and vectors keep value semantics without copies.
struct VecObj : Object {
    std::vector<float> c;
};

// A signal's connections (DESIGN.md 5.9): closures called by `emit`, in
// connection order.
struct SignalObj : Object {
    std::vector<Value> handlers;
};

// A coroutine (DESIGN.md 5.9) and the Task value that refers to it. While
// suspended, its frames live here ("heap frames"); while it runs they are on
// the VM stack. See VM for the scheduler.
struct CoroObj : Object {
    enum class State : std::uint8_t { Fresh, Running, Suspended, Done, Cancelled };
    enum class Wait : std::uint8_t { None, Ticks, Until, Task, Signal };
    struct SavedFrame {
        const Proto* fn = nullptr;
        const ClosureObj* closure = nullptr; // holds a reference
        std::uint32_t base = 0;              // relative to the coroutine's first register
        std::uint32_t pc = 0;
        std::uint32_t ret = 0;               // relative, like base
    };
    State state = State::Fresh;
    Wait wait = Wait::None;
    bool cancel_requested = false; // cancelled while running: stop at the next suspension
    bool signaled = false;         // Wait::Signal: the signal was emitted
    std::uint16_t await_dst = 0;   // register (in the top frame) receiving the await result
    std::uint64_t wake_tick = 0;   // Wait::Ticks
    Value wait_value;              // Until: the condition; Task: the task; Signal: the signal (owned)
    Value resume_value;            // value of the `await` expression when resumed (owned)
    std::vector<SavedFrame> frames;
    std::vector<Value> regs;       // saved registers of `frames` (owned)
    // How it was started, so hot reload can restart it (DESIGN.md 5.13).
    std::uint32_t start_proto = 0;
    std::vector<Value> start_args; // owned
    Object* owner = nullptr;       // not owned; cancel_owned() stops its coroutines
    Value result;                  // return value once Done (owned)
    bool finished() const { return state == State::Done || state == State::Cancelled; }
};

// A weak reference's target (LANGUAGE.md 2.11 `weak var`): the field holds
// this instead of the object; it does not keep the target alive and the
// target's death sets `target` to null. One per target, shared.
struct WeakObj : Object {
    Object* target = nullptr;
};

// Map<K, V> and Set<T> (LANGUAGE.md 9.16): entries live in insertion order
// in `keys`/`vals` (a removed entry's key has Tag::Nil until compaction);
// `table` is an open-addressing index into them (0 = empty, `tombstone` =
// deleted, else entry index + 1). Iteration walks the dense arrays, never the
// table, so it is deterministic even though class keys hash by address.
// `version` changes on every insertion or removal: a loop over the
// collection panics if it changed (pluma/collections.hpp).
struct HashObj : Object {
    static constexpr std::uint32_t tombstone = 0xffffffffu;
    std::vector<Value> keys;
    std::vector<Value> vals; // Map only
    std::vector<std::uint32_t> table;
    std::uint32_t live = 0;
    std::uint32_t version = 0;
};

// Deque<T>: a ring buffer.
struct DequeObj : Object {
    std::vector<Value> buf; // capacity: a power of two (or empty)
    std::size_t head = 0;
    std::size_t count = 0;
    std::uint32_t version = 0;
    Value& at(std::size_t i) { return buf[(head + i) & (buf.size() - 1)]; }
    const Value& at(std::size_t i) const { return buf[(head + i) & (buf.size() - 1)]; }
};

// PriorityQueue<T>: a binary min-heap. `seq` breaks ties in push order, so
// equal elements come out first-in first-out. `less` is the ordering
// closure, or null for the element type's `<`.
struct PQueueObj : Object {
    std::vector<Value> items;
    std::vector<std::uint64_t> seq;
    Value less;
    std::uint64_t next_seq = 0;
};

struct StrBuilderObj : Object {
    std::string s;
};

// Live heap objects, manual ones included (tests check refcounting with it).
std::size_t live_objects();

void free_object(Object* o);
// Adds `o` to the cycle collector's suspect buffer (pluma/heap.hpp).
void possible_root(Object* o);

// A checked reference whose object was freed (DESIGN.md 5.10).
inline bool is_stale(const Value& v) { return v.ref != 0 && v.o->gen != v.aux; }
// A checked reference to manual object `o` at its current generation.
inline Value manual_ref(Object* o) {
    Value v = Value::object(o);
    v.ref = 1;
    v.aux = o->gen;
    return v;
}

inline void retain(const Value& v) {
    if (v.tag == Tag::Obj) ++v.o->rc;
}
inline void release(const Value& v) {
    if (v.tag != Tag::Obj) return;
    Object* o = v.o;
    if (--o->rc == 0) {
        free_object(o);
    } else if ((o->flags & (objflag::Buffered | objflag::Untraced)) == 0) {
        // Bacon-Rajan: only a decrement that leaves a count can orphan a cycle.
        possible_root(o);
    }
}
// dst = src, with refcounting. Retains first, so self-assignment is safe.
inline void assign(Value& dst, const Value& src) {
    retain(src);
    Value old = dst;
    dst = src;
    release(old);
}
// dst = src where src already carries a reference (moves it in).
inline void assign_owned(Value& dst, const Value& src) {
    Value old = dst;
    dst = src;
    release(old);
}

StrObj* new_str(std::string s);
// `untraced`: the element type is acyclic (the compiler knows; pluma/acyclic.hpp).
ListObj* new_list(bool untraced = false);
InstanceObj* new_instance(const RecordRT* type, std::size_t field_count);
EnumObj* new_enum(const EnumRT* type, std::uint32_t variant, std::size_t field_count);
ClosureObj* new_closure(const Proto* fn, std::size_t capture_count);
CellObj* new_cell(bool untraced = false);
SignalObj* new_signal();
CoroObj* new_coro();
// [T; N] (value semantics: VM::copy_value copies it).
ListObj* new_array(bool untraced = false);
HashObj* new_hash(ObjKind kind, bool untraced = false); // ObjKind::Map or ObjKind::Set
DequeObj* new_deque(bool untraced = false);
PQueueObj* new_pqueue();
StrBuilderObj* new_str_builder();
// The run-time enum of every Result<T, E> value: variant 0 `Ok(value)`,
// variant 1 `Err(error)` (shared by all VMs, like strings).
const EnumRT* result_rt();
inline bool is_result(const Object* o) { return o->kind == ObjKind::Enum && static_cast<const EnumObj*>(o)->type == result_rt(); }

inline Value str_value(std::string s) { return Value::object(new_str(std::move(s))); } // rc 0: assign() it

// ---- vectors

inline bool is_vec(const Value& v) {
    return v.tag == Tag::Vec || (v.tag == Tag::Obj && v.o->kind == ObjKind::Vec);
}
inline std::uint32_t vec_size(const Value& v) {
    return v.tag == Tag::Vec ? v.aux : static_cast<std::uint32_t>(static_cast<const VecObj*>(v.o)->c.size());
}
inline const float* vec_data(const Value& v) {
    return v.tag == Tag::Vec ? v.v2 : static_cast<const VecObj*>(v.o)->c.data();
}
// A vector value with `n` components (rc 0 when boxed: assign() it).
Value vec_value(const float* data, std::uint32_t n);
inline Value vec2_value(float x, float y) {
    const float d[2] = {x, y};
    return vec_value(d, 2);
}

} // namespace pluma
