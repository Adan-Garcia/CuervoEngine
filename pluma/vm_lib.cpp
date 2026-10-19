// Natives of the Milestone 9.2a library (LANGUAGE.md 9.16 - 9.19):
// collections, List / str / number methods, conversions, Result and
// optionals. Dispatched from VM::call_native; the AOT path reaches them
// through the same CallN (pluma/aot.cpp), so VM and native builds share one
// implementation.
//
// Natives that call script code (comparators, map/filter functions) copy
// their arguments first: the call can grow the VM stack, which moves the
// registers `args` points into, and the callback could drop the last other
// reference to a value, so the values are retained while it runs.

#include <algorithm>
#include <charconv>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

#include "pluma/builtins.hpp"
#include "pluma/collections.hpp"
#include "pluma/numeric.hpp"
#include "pluma/vm.hpp"

namespace pluma {

namespace {

const std::string& str_of(const Value& v) { return static_cast<const StrObj*>(v.o)->s; }

Value owned(Value v) {
    retain(v);
    return v;
}

Value new_str_value(std::string s) { return owned(str_value(std::move(s))); }

// A retained local copy of a value, released at scope exit.
struct Hold {
    Value v;
    explicit Hold(const Value& x) : v(x) { retain(v); }
    ~Hold() { release(v); }
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
};

// Retains a vector of values; releases them at scope exit unless taken.
struct HoldAll {
    std::vector<Value> v;
    explicit HoldAll(std::vector<Value> x) : v(std::move(x)) {
        for (const Value& e : v) retain(e);
    }
    ~HoldAll() {
        for (const Value& e : v) release(e);
    }
    std::vector<Value> take() { return std::move(v); }
    HoldAll(const HoldAll&) = delete;
    HoldAll& operator=(const HoldAll&) = delete;
};

bool ascii_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// UTF-8 continuation byte.
bool cont(unsigned char c) { return (c & 0xc0) == 0x80; }

std::size_t utf8_count(std::string_view s) {
    std::size_t n = 0;
    for (char c : s) n += cont(static_cast<unsigned char>(c)) ? 0 : 1;
    return n;
}

std::string_view int_text(char* buf, std::size_t size, std::int64_t v, NumKind k, int base) {
    std::to_chars_result r{};
    if (k == NumKind::U64) {
        r = std::to_chars(buf, buf + size, static_cast<std::uint64_t>(v), base);
    } else {
        r = std::to_chars(buf, buf + size, v, base);
    }
    return std::string_view(buf, static_cast<std::size_t>(r.ptr - buf));
}

} // namespace

std::uint32_t VM::random_below(std::uint32_t bound) {
    if (options_.random_below) return options_.random_below(bound);
    // PCG32 (O'Neill, pcg-random.org, XSH RR), seeded from
    // VmOptions::random_seed; unbiased bounded values by rejection.
    auto next = [this]() {
        std::uint64_t old = rng_state_;
        rng_state_ = old * 6364136223846793005ull + 1442695040888963407ull;
        auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        auto rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    };
    if (bound <= 1) return 0;
    std::uint32_t threshold = (0u - bound) % bound;
    for (;;) {
        std::uint32_t r = next();
        if (r >= threshold) return r % bound;
    }
}

bool VM::iter_next(Value* dst, const Value& coll, Value& cursor, std::uint8_t k) {
    if (!coll.is_obj()) panic("null reference: the collection is null");
    auto c = static_cast<std::uint64_t>(cursor.i);
    auto pos = static_cast<std::uint32_t>(c & 0xffffffffu);
    auto ver = static_cast<std::uint32_t>(c >> 32);
    auto check = [&](std::uint32_t version, std::string_view what) {
        std::uint32_t v = version + 1 == 0 ? 1 : version + 1;
        if (ver == 0) {
            ver = v;
        } else if (ver != v) {
            panic(std::format("the {} changed (an element was added or removed) while a 'for' loop iterates over "
                              "it",
                              what));
        }
    };
    bool found = false;
    if (coll.o->kind == ObjKind::Deque) {
        auto* d = static_cast<DequeObj*>(coll.o);
        check(d->version, "deque");
        if (pos < d->count) {
            assign(dst[0], d->at(pos));
            ++pos;
            found = true;
        }
    } else {
        auto* h = static_cast<HashObj*>(coll.o);
        check(h->version, h->kind == ObjKind::Map ? "map" : "set");
        std::size_t p = hash_next(h, pos);
        if (p < h->keys.size()) {
            assign(dst[0], h->keys[p]);
            if ((k & opk::IterPair) != 0) assign(dst[1], h->vals[p]);
            pos = static_cast<std::uint32_t>(p + 1);
            found = true;
        }
    }
    cursor.i = static_cast<std::int64_t>((static_cast<std::uint64_t>(ver) << 32) | pos);
    return found;
}

Value VM::call_lib(std::uint16_t id, Value* args, std::uint32_t argc) {
    (void)argc;
    auto list_of = [&](const Value& v) -> ListObj* {
        if (!v.is_obj()) panic("null reference: the list is null");
        if (is_stale(v)) stale_panic(v, "the list");
        return static_cast<ListObj*>(v.o);
    };
    auto hash_of = [&](const Value& v, std::string_view what) -> HashObj* {
        if (!v.is_obj()) panic(std::format("null reference: the {} is null", what));
        return static_cast<HashObj*>(v.o);
    };
    auto map_of = [&](const Value& v) { return hash_of(v, "map"); };
    auto set_of = [&](const Value& v) { return hash_of(v, "set"); };
    auto deque_of = [&](const Value& v) -> DequeObj* {
        if (!v.is_obj()) panic("null reference: the deque is null");
        return static_cast<DequeObj*>(v.o);
    };
    auto pq_of = [&](const Value& v) -> PQueueObj* {
        if (!v.is_obj()) panic("null reference: the priority queue is null");
        return static_cast<PQueueObj*>(v.o);
    };
    auto sb_of = [&](const Value& v) -> StrBuilderObj* {
        if (!v.is_obj()) panic("null reference: the StrBuilder is null");
        return static_cast<StrBuilderObj*>(v.o);
    };
    auto fn_of = [&](const Value& v) -> const ClosureObj* {
        if (!v.is_obj()) panic("null reference: the function is null");
        return static_cast<const ClosureObj*>(v.o);
    };
    // Calls a closure with retained arguments; the result is owned.
    auto call = [&](const Value& f, std::initializer_list<Value> xs) {
        Value a[3];
        std::uint32_t n = 0;
        for (const Value& x : xs) {
            a[n] = x;
            retain(a[n]);
            ++n;
        }
        Value r;
        try {
            r = invoke_closure(fn_of(f), a, n);
        } catch (...) {
            for (std::uint32_t i = 0; i < n; ++i) release(a[i]);
            throw;
        }
        for (std::uint32_t i = 0; i < n; ++i) release(a[i]);
        return r;
    };
    auto call_bool = [&](const Value& f, std::initializer_list<Value> xs) {
        Value r = call(f, xs);
        bool b = r.b;
        release(r);
        return b;
    };
    // The element order `sort()`, min(), max() and PriorityQueue use.
    auto natural_less = [&](const Value& a, const Value& b) {
        if (a.tag == Tag::F32 && b.tag == Tag::F32) return float_sort_less(a.f, b.f);
        if (a.tag == Tag::F64 && b.tag == Tag::F64) return float_sort_less(a.d, b.d);
        if (is_num_tag(a.tag) && a.tag == b.tag) return num::int_less(tag_num(a.tag), a.i, b.i);
        return dyn_less(a, b, false);
    };
    auto new_list_from = [&](std::vector<Value> items, bool untraced = false) {
        ListObj* l = new_list(untraced);
        l->items = std::move(items); // already owned
        return owned(Value::object(l));
    };
    auto copy_items = [&](const std::vector<Value>& src) {
        std::vector<Value> out(src.size());
        for (std::size_t i = 0; i < src.size(); ++i) assign(out[i], copy_value(src[i]));
        return out;
    };
    auto index_check = [&](std::int64_t i, std::size_t size, std::string_view what) {
        if (i < 0 || static_cast<std::size_t>(i) >= size) {
            panic(std::format("index {} is out of bounds for a {} of length {}", i, what, size));
        }
        return static_cast<std::size_t>(i);
    };
    auto make_result = [&](bool ok, const Value& v) {
        EnumObj* e = new_enum(result_rt(), ok ? 0 : 1, 1);
        if (!v.is_obj()) e->flags |= objflag::Untraced; // immutable: a leaf forever
        assign(e->fields[0], v);
        return owned(Value::object(e));
    };
    auto result_of = [&](const Value& v) -> const EnumObj* {
        if (!v.is_obj()) panic("null reference: the result is null");
        return static_cast<const EnumObj*>(v.o);
    };

    switch (static_cast<Builtin>(id)) {
        // ---------------------------------------------------------- conversions
        case Builtin::ParseInt:
        case Builtin::StrParseInt: {
            std::string_view s = str_of(args[0]);
            std::int64_t radix = args[1].i;
            if (radix < 2 || radix > 36) panic(std::format("parse_int: radix {} is not in 2..36", radix));
            bool neg = false;
            if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
                neg = s[0] == '-';
                s.remove_prefix(1);
            }
            if (s.empty()) return Value::null();
            std::uint64_t mag = 0;
            auto r = std::from_chars(s.data(), s.data() + s.size(), mag, static_cast<int>(radix));
            if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return Value::null();
            if (neg ? mag > 0x80000000ull : mag > 0x7fffffffull) return Value::null();
            std::int64_t v = neg ? -static_cast<std::int64_t>(mag) : static_cast<std::int64_t>(mag);
            return Value::integer(NumKind::I32, v);
        }
        case Builtin::ParseFloat:
        case Builtin::StrParseFloat: {
            std::string_view s = str_of(args[0]);
            if (!s.empty() && s[0] == '+') s.remove_prefix(1);
            if (s.empty() || s[0] == '+') return Value::null();
            float f = 0.0f;
            auto r = std::from_chars(s.data(), s.data() + s.size(), f);
            if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return Value::null();
            return Value::f32(f);
        }
        case Builtin::NumToHex:
        case Builtin::NumToRadix: {
            std::int64_t base = static_cast<Builtin>(id) == Builtin::NumToHex ? 16 : args[1].i;
            if (base < 2 || base > 36) panic(std::format("to_radix: base {} is not in 2..36", base));
            char buf[72];
            return new_str_value(std::string(int_text(buf, sizeof(buf), args[0].i, tag_num(args[0].tag),
                                                      static_cast<int>(base))));
        }
        case Builtin::NumToFixed: {
            std::int64_t digits = args[1].i;
            if (digits < 0 || digits > 32) panic(std::format("to_fixed: {} digits is not in 0..32", digits));
            // std::to_chars is exact and locale-independent, so the text is
            // the same on every platform (DESIGN.md 4.1).
            char buf[512];
            std::to_chars_result r{};
            if (args[0].tag == Tag::F32) {
                r = std::to_chars(buf, buf + sizeof(buf), args[0].f, std::chars_format::fixed, static_cast<int>(digits));
            } else {
                r = std::to_chars(buf, buf + sizeof(buf), args[0].d, std::chars_format::fixed, static_cast<int>(digits));
            }
            if (r.ec != std::errc()) panic("to_fixed: the number is too large to format");
            return new_str_value(std::string(buf, static_cast<std::size_t>(r.ptr - buf)));
        }

        // ---------------------------------------------------------- Result / optionals
        case Builtin::ResultOk: return make_result(true, args[0]);
        case Builtin::ResultErr: return make_result(false, args[0]);
        case Builtin::ResIsOk: return Value::boolean(result_of(args[0])->variant == 0);
        case Builtin::ResIsErr: return Value::boolean(result_of(args[0])->variant == 1);
        case Builtin::ResUnwrap:
        case Builtin::ResExpect: {
            const EnumObj* r = result_of(args[0]);
            if (r->variant != 0) {
                std::string err = describe(r->fields[0]);
                if (static_cast<Builtin>(id) == Builtin::ResExpect) panic(std::format("{}: {}", str_of(args[1]), err));
                panic(std::format("unwrap() on Err({})", err));
            }
            return owned(r->fields[0]);
        }
        case Builtin::ResUnwrapErr: {
            const EnumObj* r = result_of(args[0]);
            if (r->variant != 1) panic(std::format("unwrap_err() on Ok({})", describe(r->fields[0])));
            return owned(r->fields[0]);
        }
        case Builtin::ResUnwrapOr: {
            const EnumObj* r = result_of(args[0]);
            return owned(r->variant == 0 ? r->fields[0] : args[1]);
        }
        case Builtin::ResOk: {
            const EnumObj* r = result_of(args[0]);
            return r->variant == 0 ? owned(r->fields[0]) : Value::null();
        }
        case Builtin::ResErr: {
            const EnumObj* r = result_of(args[0]);
            return r->variant == 1 ? owned(r->fields[0]) : Value::null();
        }
        case Builtin::OptUnwrap:
        case Builtin::OptExpect:
            if (args[0].is_null()) {
                if (static_cast<Builtin>(id) == Builtin::OptExpect) panic(str_of(args[1]));
                panic("unwrap() on null");
            }
            return owned(args[0]);
        case Builtin::OptIsSome: return Value::boolean(!args[0].is_null());
        case Builtin::OptIsNone: return Value::boolean(args[0].is_null());

        // ---------------------------------------------------------- constructors
        case Builtin::MapNew: return owned(Value::object(new_hash(ObjKind::Map, args[0].b)));
        case Builtin::SetNew: return owned(Value::object(new_hash(ObjKind::Set, args[0].b)));
        case Builtin::DequeNew: return owned(Value::object(new_deque(args[0].b)));
        case Builtin::PQueueNew: {
            PQueueObj* q = new_pqueue();
            assign(q->less, args[0]);
            return owned(Value::object(q));
        }
        case Builtin::StrBuilderNew: return owned(Value::object(new_str_builder()));
        case Builtin::ArrayNew: {
            ListObj* a = new_array(args[2].b);
            Value elem = args[1];
            a->items.resize(static_cast<std::size_t>(args[0].i));
            for (Value& v : a->items) assign(v, copy_value(elem));
            return owned(Value::object(a));
        }

        // ---------------------------------------------------------- List / [T; N]
        case Builtin::ListSort: {
            ListObj* l = list_of(args[0]);
            Hold less(args[1]);
            HoldAll tmp(l->items);
            if (less.v.is_null()) {
                merge_sort(tmp.v, natural_less);
            } else {
                merge_sort(tmp.v, [&](const Value& a, const Value& b) { return call_bool(less.v, {a, b}); });
            }
            std::vector<Value> old;
            old.swap(l->items);
            l->items = tmp.take();
            for (Value& v : old) release(v);
            return {};
        }
        case Builtin::ListSortBy: {
            // Keys are computed once per element (decorate-sort-undecorate).
            ListObj* l = list_of(args[0]);
            Hold key(args[1]);
            HoldAll items(l->items);
            std::vector<Value> keys;
            try {
                for (const Value& v : items.v) keys.push_back(call(key.v, {v}));
            } catch (...) {
                for (Value& k : keys) release(k);
                throw;
            }
            std::vector<Value> idx(items.v.size());
            for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = Value::integer(NumKind::I64, static_cast<std::int64_t>(i));
            try {
                merge_sort(idx, [&](const Value& a, const Value& b) {
                    return natural_less(keys[static_cast<std::size_t>(a.i)], keys[static_cast<std::size_t>(b.i)]);
                });
            } catch (...) {
                for (Value& k : keys) release(k);
                throw;
            }
            for (Value& k : keys) release(k);
            std::vector<Value> sorted(idx.size());
            for (std::size_t i = 0; i < idx.size(); ++i) sorted[i] = items.v[static_cast<std::size_t>(idx[i].i)];
            std::vector<Value> old;
            old.swap(l->items);
            l->items = std::move(sorted); // takes the references `items` held
            items.v.clear();
            for (Value& v : old) release(v);
            return {};
        }
        case Builtin::ListReverse: {
            ListObj* l = list_of(args[0]);
            std::reverse(l->items.begin(), l->items.end());
            return {};
        }
        case Builtin::ListIndexOf: {
            ListObj* l = list_of(args[0]);
            Hold x(args[1]);
            for (std::size_t i = 0; i < l->items.size(); ++i) {
                if (dyn_equal(l->items[i], x.v)) return Value::integer(NumKind::I32, static_cast<std::int64_t>(i));
            }
            return Value::integer(NumKind::I32, -1);
        }
        case Builtin::ListSlice: {
            ListObj* l = list_of(args[0]);
            auto n = static_cast<std::int64_t>(l->items.size());
            std::int64_t lo = std::clamp<std::int64_t>(args[1].i, 0, n);
            std::int64_t hi = std::clamp<std::int64_t>(args[2].i, lo, n);
            std::vector<Value> out(l->items.begin() + lo, l->items.begin() + hi);
            return new_list_from(copy_items(out), (l->flags & objflag::Untraced) != 0);
        }
        case Builtin::ListExtend: {
            ListObj* l = list_of(args[0]);
            ListObj* o = list_of(args[1]);
            std::vector<Value> add = copy_items(o->items); // o may be l
            l->items.insert(l->items.end(), add.begin(), add.end());
            return {};
        }
        case Builtin::ListSwap: {
            ListObj* l = list_of(args[0]);
            std::size_t i = index_check(args[1].i, l->items.size(), "list");
            std::size_t j = index_check(args[2].i, l->items.size(), "list");
            std::swap(l->items[i], l->items[j]);
            return {};
        }
        case Builtin::ListFirst: {
            ListObj* l = list_of(args[0]);
            return l->items.empty() ? Value::null() : owned(copy_value(l->items.front()));
        }
        case Builtin::ListLast: {
            ListObj* l = list_of(args[0]);
            return l->items.empty() ? Value::null() : owned(copy_value(l->items.back()));
        }
        case Builtin::ListFill: {
            ListObj* l = list_of(args[0]);
            Hold x(args[1]);
            for (Value& v : l->items) assign(v, copy_value(x.v));
            return {};
        }
        case Builtin::ListToList: {
            ListObj* l = list_of(args[0]);
            return new_list_from(copy_items(l->items), (l->flags & objflag::Untraced) != 0);
        }
        case Builtin::ListUnique: {
            ListObj* l = list_of(args[0]);
            std::vector<Value> out;
            bool hashable = true;
            for (const Value& v : l->items) {
                hashable = hashable && (v.tag != Tag::Obj || v.o->kind == ObjKind::Str) && !is_vec(v) &&
                           v.tag != Tag::F32 && v.tag != Tag::F64;
            }
            if (hashable) {
                Hold seen(Value::object(new_hash(ObjKind::Set, true)));
                for (const Value& v : l->items) {
                    if (hash_put(static_cast<HashObj*>(seen.v.o), v, nullptr)) {
                        out.emplace_back();
                        assign(out.back(), v);
                    }
                }
            } else {
                for (const Value& v : l->items) {
                    bool dup = false;
                    for (const Value& u : out) {
                        if (dyn_equal(u, v)) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) {
                        out.emplace_back();
                        assign(out.back(), copy_value(v));
                    }
                }
            }
            return new_list_from(std::move(out), (l->flags & objflag::Untraced) != 0);
        }
        case Builtin::ListShuffle: {
            // Fisher-Yates over the VM's seeded stream (LANGUAGE.md 9.17).
            ListObj* l = list_of(args[0]);
            for (std::size_t i = l->items.size(); i > 1; --i) {
                std::size_t j = random_below(static_cast<std::uint32_t>(i));
                std::swap(l->items[i - 1], l->items[j]);
            }
            return {};
        }
        case Builtin::ListMin:
        case Builtin::ListMax: {
            ListObj* l = list_of(args[0]);
            if (l->items.empty()) return Value::null();
            HoldAll items(l->items);
            const bool is_min = static_cast<Builtin>(id) == Builtin::ListMin;
            std::size_t best = 0;
            for (std::size_t i = 1; i < items.v.size(); ++i) {
                if (is_min ? natural_less(items.v[i], items.v[best]) : natural_less(items.v[best], items.v[i])) best = i;
            }
            return owned(copy_value(items.v[best]));
        }
        case Builtin::ListSum: {
            ListObj* l = list_of(args[0]);
            HoldAll items(l->items);
            Value acc = owned(args[1]);
            for (const Value& v : items.v) {
                Value next;
                try {
                    next = dyn_arith(Op::Add, acc, v);
                } catch (...) {
                    release(acc);
                    throw;
                }
                release(acc);
                acc = next; // dyn_arith results are owned
            }
            return acc;
        }
        case Builtin::ListMap:
        case Builtin::ListFilter: {
            ListObj* l = list_of(args[0]);
            Hold f(args[1]);
            HoldAll items(l->items);
            std::vector<Value> out;
            try {
                for (const Value& v : items.v) {
                    if (static_cast<Builtin>(id) == Builtin::ListMap) {
                        out.push_back(call(f.v, {v}));
                    } else if (call_bool(f.v, {v})) {
                        out.emplace_back();
                        assign(out.back(), copy_value(v));
                    }
                }
            } catch (...) {
                for (Value& v : out) release(v);
                throw;
            }
            return new_list_from(std::move(out));
        }
        case Builtin::ListAny:
        case Builtin::ListAll:
        case Builtin::ListCount: {
            ListObj* l = list_of(args[0]);
            Hold f(args[1]);
            HoldAll items(l->items);
            std::int64_t n = 0;
            for (const Value& v : items.v) {
                bool b = call_bool(f.v, {v});
                if (static_cast<Builtin>(id) == Builtin::ListAny && b) return Value::boolean(true);
                if (static_cast<Builtin>(id) == Builtin::ListAll && !b) return Value::boolean(false);
                n += b ? 1 : 0;
            }
            if (static_cast<Builtin>(id) == Builtin::ListCount) return Value::integer(NumKind::I32, n);
            return Value::boolean(static_cast<Builtin>(id) == Builtin::ListAll);
        }
        case Builtin::ListReduce: {
            ListObj* l = list_of(args[0]);
            Hold f(args[2]);
            HoldAll items(l->items);
            Value acc = owned(args[1]);
            for (const Value& v : items.v) {
                Value next;
                try {
                    next = call(f.v, {acc, v});
                } catch (...) {
                    release(acc);
                    throw;
                }
                release(acc);
                acc = next;
            }
            return acc;
        }
        case Builtin::ListZip: {
            ListObj* a = list_of(args[0]);
            ListObj* b = list_of(args[1]);
            Hold f(args[2]);
            HoldAll xs(a->items);
            HoldAll ys(b->items);
            std::size_t n = std::min(xs.v.size(), ys.v.size());
            std::vector<Value> out;
            try {
                for (std::size_t i = 0; i < n; ++i) out.push_back(call(f.v, {xs.v[i], ys.v[i]}));
            } catch (...) {
                for (Value& v : out) release(v);
                throw;
            }
            return new_list_from(std::move(out));
        }

        // ---------------------------------------------------------- Map
        case Builtin::MapLen: return Value::integer(NumKind::I32, map_of(args[0])->live);
        case Builtin::MapIsEmpty: return Value::boolean(map_of(args[0])->live == 0);
        case Builtin::MapHas: return Value::boolean(hash_find(map_of(args[0]), args[1]) >= 0);
        case Builtin::MapGet: {
            HashObj* h = map_of(args[0]);
            std::int64_t i = hash_find(h, args[1]);
            return i < 0 ? Value::null() : owned(copy_value(h->vals[static_cast<std::size_t>(i)]));
        }
        case Builtin::MapGetOr: {
            HashObj* h = map_of(args[0]);
            std::int64_t i = hash_find(h, args[1]);
            return owned(copy_value(i < 0 ? args[2] : h->vals[static_cast<std::size_t>(i)]));
        }
        case Builtin::MapIndex: {
            HashObj* h = map_of(args[0]);
            std::int64_t i = hash_find(h, args[1]);
            if (i < 0) panic(std::format("key {} is not in the map", describe(args[1])));
            return owned(h->vals[static_cast<std::size_t>(i)]);
        }
        case Builtin::MapSet: {
            Value v = args[2];
            hash_put(map_of(args[0]), args[1], &v);
            return {};
        }
        case Builtin::MapRemove: return Value::boolean(hash_remove(map_of(args[0]), args[1]));
        case Builtin::MapClear: hash_clear(map_of(args[0])); return {};
        case Builtin::MapKeys:
        case Builtin::MapValues: {
            HashObj* h = map_of(args[0]);
            const bool keys = static_cast<Builtin>(id) == Builtin::MapKeys;
            std::vector<Value> out;
            out.reserve(h->live);
            bool all_leaf = true;
            for (std::size_t i = 0; i < h->keys.size(); ++i) {
                if (h->keys[i].tag == Tag::Nil) continue;
                out.emplace_back();
                assign(out.back(), copy_value(keys ? h->keys[i] : h->vals[i]));
                all_leaf = all_leaf && (!out.back().is_obj() || out.back().o->kind == ObjKind::Str);
            }
            return new_list_from(std::move(out), keys && all_leaf);
        }

        // ---------------------------------------------------------- Set
        case Builtin::SetLen: return Value::integer(NumKind::I32, set_of(args[0])->live);
        case Builtin::SetIsEmpty: return Value::boolean(set_of(args[0])->live == 0);
        case Builtin::SetAdd: return Value::boolean(hash_put(set_of(args[0]), args[1], nullptr));
        case Builtin::SetRemove: return Value::boolean(hash_remove(set_of(args[0]), args[1]));
        case Builtin::SetContains: return Value::boolean(hash_find(set_of(args[0]), args[1]) >= 0);
        case Builtin::SetClear: hash_clear(set_of(args[0])); return {};
        case Builtin::SetToList: {
            HashObj* h = set_of(args[0]);
            std::vector<Value> out;
            for (const Value& k : h->keys) {
                if (k.tag == Tag::Nil) continue;
                out.emplace_back();
                assign(out.back(), k);
            }
            return new_list_from(std::move(out), (h->flags & objflag::Untraced) != 0);
        }
        case Builtin::SetUnion:
        case Builtin::SetIntersection:
        case Builtin::SetDifference: {
            // Elements keep the left set's order, then (union) the right's.
            HashObj* a = set_of(args[0]);
            HashObj* b = set_of(args[1]);
            HashObj* out = new_hash(ObjKind::Set, (a->flags & objflag::Untraced) != 0);
            Value result = owned(Value::object(out));
            const auto op = static_cast<Builtin>(id);
            for (const Value& k : a->keys) {
                if (k.tag == Tag::Nil) continue;
                bool in_b = hash_find(b, k) >= 0;
                if (op == Builtin::SetUnion || (op == Builtin::SetIntersection) == in_b) hash_put(out, k, nullptr);
            }
            if (op == Builtin::SetUnion) {
                for (const Value& k : b->keys) {
                    if (k.tag != Tag::Nil) hash_put(out, k, nullptr);
                }
            }
            return result;
        }

        // ---------------------------------------------------------- Deque
        case Builtin::DequeLen: return Value::integer(NumKind::I32, static_cast<std::int64_t>(deque_of(args[0])->count));
        case Builtin::DequeIsEmpty: return Value::boolean(deque_of(args[0])->count == 0);
        case Builtin::DequePushBack: deque_push_back(deque_of(args[0]), args[1]); return {};
        case Builtin::DequePushFront: deque_push_front(deque_of(args[0]), args[1]); return {};
        case Builtin::DequePopBack:
        case Builtin::DequePopFront: {
            DequeObj* d = deque_of(args[0]);
            const bool back = static_cast<Builtin>(id) == Builtin::DequePopBack;
            if (d->count == 0) panic(std::format("{}() on an empty deque", back ? "pop_back" : "pop_front"));
            return back ? deque_pop_back(d) : deque_pop_front(d);
        }
        case Builtin::DequeFront:
        case Builtin::DequeBack: {
            DequeObj* d = deque_of(args[0]);
            const bool back = static_cast<Builtin>(id) == Builtin::DequeBack;
            if (d->count == 0) panic(std::format("{}() on an empty deque", back ? "back" : "front"));
            return owned(copy_value(d->at(back ? d->count - 1 : 0)));
        }
        case Builtin::DequeGet: {
            DequeObj* d = deque_of(args[0]);
            return owned(d->at(index_check(args[1].i, d->count, "deque")));
        }
        case Builtin::DequeSet: {
            DequeObj* d = deque_of(args[0]);
            assign(d->at(index_check(args[1].i, d->count, "deque")), args[2]);
            return {};
        }
        case Builtin::DequeClear: deque_clear(deque_of(args[0])); return {};
        case Builtin::DequeContains: {
            DequeObj* d = deque_of(args[0]);
            Hold x(args[1]);
            for (std::size_t i = 0; i < d->count; ++i) {
                if (dyn_equal(d->at(i), x.v)) return Value::boolean(true);
            }
            return Value::boolean(false);
        }
        case Builtin::DequeToList: {
            DequeObj* d = deque_of(args[0]);
            std::vector<Value> out(d->count);
            for (std::size_t i = 0; i < d->count; ++i) assign(out[i], copy_value(d->at(i)));
            return new_list_from(std::move(out), (d->flags & objflag::Untraced) != 0);
        }

        // ---------------------------------------------------------- PriorityQueue
        case Builtin::PQPush:
        case Builtin::PQPop:
        case Builtin::PQPeek:
        case Builtin::PQToList: {
            PQueueObj* q = pq_of(args[0]);
            Hold self(args[0]);
            Hold less(q->less);
            // Strict order with push order breaking ties: FIFO among equals.
            auto before = [&](std::size_t i, std::size_t j) {
                const Value& a = q->items[i];
                const Value& b = q->items[j];
                bool lt = less.v.is_null() ? natural_less(a, b) : call_bool(less.v, {a, b});
                if (lt) return true;
                bool gt = less.v.is_null() ? natural_less(b, a) : call_bool(less.v, {b, a});
                return !gt && q->seq[i] < q->seq[j];
            };
            auto swap_at = [&](std::size_t i, std::size_t j) {
                std::swap(q->items[i], q->items[j]);
                std::swap(q->seq[i], q->seq[j]);
            };
            auto sift_down = [&](std::size_t i) {
                for (;;) {
                    std::size_t l = 2 * i + 1;
                    std::size_t r = l + 1;
                    std::size_t m = i;
                    if (l < q->items.size() && before(l, m)) m = l;
                    if (r < q->items.size() && before(r, m)) m = r;
                    if (m == i) return;
                    swap_at(i, m);
                    i = m;
                }
            };
            switch (static_cast<Builtin>(id)) {
                case Builtin::PQPush: {
                    q->items.emplace_back();
                    assign(q->items.back(), args[1]);
                    q->seq.push_back(q->next_seq++);
                    std::size_t i = q->items.size() - 1;
                    while (i > 0) {
                        std::size_t p = (i - 1) / 2;
                        if (!before(i, p)) break;
                        swap_at(i, p);
                        i = p;
                    }
                    return {};
                }
                case Builtin::PQPeek:
                    if (q->items.empty()) panic("peek() on an empty priority queue");
                    return owned(copy_value(q->items[0]));
                case Builtin::PQPop: {
                    if (q->items.empty()) panic("pop() on an empty priority queue");
                    swap_at(0, q->items.size() - 1);
                    Value top = q->items.back(); // the queue's reference moves to the caller
                    q->items.pop_back();
                    q->seq.pop_back();
                    try {
                        sift_down(0);
                    } catch (...) {
                        release(top);
                        throw;
                    }
                    return top;
                }
                default: {
                    // Pop order, without changing the queue.
                    HoldAll items(q->items);
                    std::vector<Value> idx(items.v.size());
                    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = Value::integer(NumKind::I64, static_cast<std::int64_t>(i));
                    merge_sort(idx, [&](const Value& a, const Value& b) {
                        return before(static_cast<std::size_t>(a.i), static_cast<std::size_t>(b.i));
                    });
                    std::vector<Value> out(idx.size());
                    for (std::size_t i = 0; i < idx.size(); ++i) {
                        assign(out[i], copy_value(items.v[static_cast<std::size_t>(idx[i].i)]));
                    }
                    return new_list_from(std::move(out));
                }
            }
        }
        case Builtin::PQLen: return Value::integer(NumKind::I32, static_cast<std::int64_t>(pq_of(args[0])->items.size()));
        case Builtin::PQIsEmpty: return Value::boolean(pq_of(args[0])->items.empty());
        case Builtin::PQClear: {
            PQueueObj* q = pq_of(args[0]);
            std::vector<Value> items;
            items.swap(q->items);
            q->seq.clear();
            for (Value& v : items) release(v);
            return {};
        }

        // ---------------------------------------------------------- StrBuilder
        case Builtin::SbAppend: sb_of(args[0])->s += str_of(args[1]); return owned(args[0]);
        case Builtin::SbLen: return Value::integer(NumKind::I32, static_cast<std::int64_t>(sb_of(args[0])->s.size()));
        case Builtin::SbClear: sb_of(args[0])->s.clear(); return {};
        case Builtin::SbToStr: return new_str_value(sb_of(args[0])->s);
        case Builtin::SbIsEmpty: return Value::boolean(sb_of(args[0])->s.empty());

        // ---------------------------------------------------------- str
        case Builtin::StrSplit: {
            const std::string& s = str_of(args[0]);
            const std::string& sep = str_of(args[1]);
            if (sep.empty()) panic("split: the separator is empty (use chars() to split into characters)");
            std::vector<Value> out;
            std::size_t at = 0;
            for (;;) {
                std::size_t p = s.find(sep, at);
                out.push_back(new_str_value(s.substr(at, p == std::string::npos ? std::string::npos : p - at)));
                if (p == std::string::npos) break;
                at = p + sep.size();
            }
            return new_list_from(std::move(out), true);
        }
        case Builtin::StrJoin: {
            const std::string& sep = str_of(args[0]);
            ListObj* l = list_of(args[1]);
            std::string out;
            for (std::size_t i = 0; i < l->items.size(); ++i) {
                if (i > 0) out += sep;
                out += str_of(l->items[i]);
            }
            return new_str_value(std::move(out));
        }
        case Builtin::StrReplace: {
            const std::string& s = str_of(args[0]);
            const std::string& from = str_of(args[1]);
            const std::string& to = str_of(args[2]);
            if (from.empty()) return owned(args[0]);
            std::string out;
            std::size_t at = 0;
            for (;;) {
                std::size_t p = s.find(from, at);
                if (p == std::string::npos) break;
                out.append(s, at, p - at);
                out += to;
                at = p + from.size();
            }
            out.append(s, at, std::string::npos);
            return new_str_value(std::move(out));
        }
        case Builtin::StrTrim:
        case Builtin::StrTrimStart:
        case Builtin::StrTrimEnd: {
            std::string_view s = str_of(args[0]);
            const auto op = static_cast<Builtin>(id);
            if (op != Builtin::StrTrimEnd) {
                while (!s.empty() && ascii_space(s.front())) s.remove_prefix(1);
            }
            if (op != Builtin::StrTrimStart) {
                while (!s.empty() && ascii_space(s.back())) s.remove_suffix(1);
            }
            return new_str_value(std::string(s));
        }
        case Builtin::StrToUpper:
        case Builtin::StrToLower: {
            std::string s = str_of(args[0]);
            const bool up = static_cast<Builtin>(id) == Builtin::StrToUpper;
            for (char& c : s) {
                if (up && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                if (!up && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            return new_str_value(std::move(s));
        }
        case Builtin::StrFind: {
            const std::string& s = str_of(args[0]);
            std::int64_t from = std::clamp<std::int64_t>(args[2].i, 0, static_cast<std::int64_t>(s.size()));
            std::size_t p = s.find(str_of(args[1]), static_cast<std::size_t>(from));
            return Value::integer(NumKind::I32, p == std::string::npos ? -1 : static_cast<std::int64_t>(p));
        }
        case Builtin::StrSubstr: {
            const std::string& s = str_of(args[0]);
            auto n = static_cast<std::int64_t>(s.size());
            std::int64_t start = std::clamp<std::int64_t>(args[1].i, 0, n);
            std::int64_t len = std::clamp<std::int64_t>(args[2].i, 0, n - start);
            auto boundary = [&](std::int64_t i) {
                return i >= n || !cont(static_cast<unsigned char>(s[static_cast<std::size_t>(i)]));
            };
            if (!boundary(start) || !boundary(start + len)) {
                panic(std::format("substr({}, {}) splits a UTF-8 character", args[1].i, args[2].i));
            }
            return new_str_value(s.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(len)));
        }
        case Builtin::StrByteAt: {
            const std::string& s = str_of(args[0]);
            std::size_t i = index_check(args[1].i, s.size(), "string");
            return Value::integer(NumKind::I32, static_cast<unsigned char>(s[i]));
        }
        case Builtin::StrChars: {
            const std::string& s = str_of(args[0]);
            std::vector<Value> out;
            for (std::size_t i = 0; i < s.size();) {
                std::size_t j = i + 1;
                while (j < s.size() && cont(static_cast<unsigned char>(s[j]))) ++j;
                out.push_back(new_str_value(s.substr(i, j - i)));
                i = j;
            }
            return new_list_from(std::move(out), true);
        }
        case Builtin::StrCharCount:
            return Value::integer(NumKind::I32, static_cast<std::int64_t>(utf8_count(str_of(args[0]))));
        case Builtin::StrRepeat: {
            const std::string& s = str_of(args[0]);
            std::int64_t n = args[1].i;
            if (n < 0) panic(std::format("repeat: count {} is negative", n));
            if (!s.empty() && static_cast<std::uint64_t>(n) > (std::uint64_t{1} << 30) / s.size()) {
                panic("repeat: the result would be larger than 1 GiB");
            }
            std::string out;
            out.reserve(s.size() * static_cast<std::size_t>(n));
            for (std::int64_t i = 0; i < n; ++i) out += s;
            return new_str_value(std::move(out));
        }
        case Builtin::StrPadLeft:
        case Builtin::StrPadRight: {
            // Width counts characters (code points), like char_count().
            const std::string& s = str_of(args[0]);
            const std::string& fill = str_of(args[2]);
            if (utf8_count(fill) != 1) panic("pad: the fill must be exactly one character");
            std::int64_t width = args[1].i;
            auto have = static_cast<std::int64_t>(utf8_count(s));
            if (width > (1 << 24)) panic(std::format("pad: width {} is too large", width));
            std::string pad;
            for (std::int64_t i = have; i < width; ++i) pad += fill;
            return new_str_value(static_cast<Builtin>(id) == Builtin::StrPadLeft ? pad + s : s + pad);
        }
        case Builtin::StrLines: {
            const std::string& s = str_of(args[0]);
            std::vector<Value> out;
            std::size_t at = 0;
            while (at < s.size()) {
                std::size_t p = s.find('\n', at);
                std::size_t end = p == std::string::npos ? s.size() : p;
                std::size_t e = end > at && s[end - 1] == '\r' ? end - 1 : end;
                out.push_back(new_str_value(s.substr(at, e - at)));
                if (p == std::string::npos) break;
                at = p + 1;
            }
            return new_list_from(std::move(out), true);
        }
        case Builtin::StrIsEmpty: return Value::boolean(str_of(args[0]).empty());
        default: break;
    }
    panic("unknown native function");
}

} // namespace pluma
