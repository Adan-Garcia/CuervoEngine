#include "pluma/collections.hpp"

#include <string_view>

namespace pluma {

namespace {

// splitmix64's finalizer: a cheap, well-mixed integer hash.
std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

std::uint64_t fnv1a(std::string_view s) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}

void rebuild(HashObj* h, std::size_t capacity) {
    // Compacts the dense arrays (dropping removed entries) and re-indexes.
    std::size_t w = 0;
    const bool is_map = h->kind == ObjKind::Map;
    for (std::size_t r = 0; r < h->keys.size(); ++r) {
        if (h->keys[r].tag == Tag::Nil) continue;
        h->keys[w] = h->keys[r];
        if (is_map) h->vals[w] = h->vals[r];
        ++w;
    }
    h->keys.resize(w);
    if (is_map) h->vals.resize(w);
    h->table.assign(capacity, 0);
    const std::size_t mask = capacity - 1;
    for (std::size_t i = 0; i < w; ++i) {
        std::size_t s = key_hash(h->keys[i]) & mask;
        while (h->table[s] != 0) s = (s + 1) & mask;
        h->table[s] = static_cast<std::uint32_t>(i + 1);
    }
}

} // namespace

std::uint64_t key_hash(const Value& k) {
    switch (k.tag) {
        case Tag::Obj:
            if (k.o->kind == ObjKind::Str) return mix(fnv1a(static_cast<const StrObj*>(k.o)->s));
            // Class references hash by identity. The table's layout then
            // depends on addresses, but no order is ever read from it.
            return mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(k.o)));
        case Tag::Null:
        case Tag::Nil: return 0x9e3779b97f4a7c15ull;
        default: return mix(static_cast<std::uint64_t>(k.i) ^ (static_cast<std::uint64_t>(k.tag) << 56));
    }
}

bool key_equal(const Value& a, const Value& b) {
    if (a.is_null() || b.is_null()) return a.is_null() && b.is_null();
    if (a.tag != b.tag) return false;
    if (a.tag == Tag::Obj) {
        if (a.o == b.o) return true;
        if (a.o->kind == ObjKind::Str && b.o->kind == ObjKind::Str) {
            return static_cast<const StrObj*>(a.o)->s == static_cast<const StrObj*>(b.o)->s;
        }
        return false;
    }
    if (a.tag == Tag::Bool) return a.b == b.b;
    return a.i == b.i && a.aux == b.aux;
}

std::int64_t hash_find(const HashObj* h, const Value& k) {
    if (h->table.empty()) return -1;
    const std::size_t mask = h->table.size() - 1;
    std::size_t s = key_hash(k) & mask;
    for (;;) {
        std::uint32_t e = h->table[s];
        if (e == 0) return -1;
        if (e != HashObj::tombstone && key_equal(h->keys[e - 1], k)) return static_cast<std::int64_t>(e - 1);
        s = (s + 1) & mask;
    }
}

bool hash_put(HashObj* h, const Value& k, const Value* v) {
    const bool is_map = h->kind == ObjKind::Map;
    if (std::int64_t i = hash_find(h, k); i >= 0) {
        if (is_map && v != nullptr) assign(h->vals[static_cast<std::size_t>(i)], *v);
        return false;
    }
    // Load (live + removed entries still in the dense arrays) below 3/4.
    if ((h->keys.size() + 1) * 4 >= h->table.size() * 3) {
        std::size_t cap = h->table.empty() ? 8 : h->table.size();
        while ((h->live + 1) * 2 >= cap) cap *= 2;
        rebuild(h, cap);
    }
    h->keys.emplace_back();
    assign(h->keys.back(), k);
    if (is_map) {
        h->vals.emplace_back();
        if (v != nullptr) assign(h->vals.back(), *v);
    }
    const std::size_t mask = h->table.size() - 1;
    std::size_t s = key_hash(k) & mask;
    while (h->table[s] != 0 && h->table[s] != HashObj::tombstone) s = (s + 1) & mask;
    h->table[s] = static_cast<std::uint32_t>(h->keys.size());
    ++h->live;
    ++h->version;
    return true;
}

bool hash_remove(HashObj* h, const Value& k) {
    std::int64_t i = hash_find(h, k);
    if (i < 0) return false;
    const auto idx = static_cast<std::size_t>(i);
    const std::size_t mask = h->table.size() - 1;
    std::size_t s = key_hash(k) & mask;
    while (h->table[s] != idx + 1) s = (s + 1) & mask;
    h->table[s] = HashObj::tombstone;
    Value old_k = h->keys[idx];
    h->keys[idx] = Value{};
    Value old_v;
    if (h->kind == ObjKind::Map) {
        old_v = h->vals[idx];
        h->vals[idx] = Value{};
    }
    --h->live;
    ++h->version;
    // Compact when more than half the dense entries are dead.
    if (h->keys.size() > 16 && h->live * 2 < h->keys.size()) rebuild(h, h->table.size());
    release(old_k);
    release(old_v);
    return true;
}

void hash_clear(HashObj* h) {
    std::vector<Value> keys;
    std::vector<Value> vals;
    keys.swap(h->keys);
    vals.swap(h->vals);
    h->table.clear();
    h->live = 0;
    ++h->version;
    for (Value& v : keys) release(v);
    for (Value& v : vals) release(v);
}

namespace {

void deque_grow(DequeObj* d) {
    std::size_t cap = d->buf.empty() ? 8 : d->buf.size() * 2;
    std::vector<Value> nb(cap);
    for (std::size_t i = 0; i < d->count; ++i) nb[i] = d->at(i);
    d->buf.swap(nb);
    d->head = 0;
}

} // namespace

void deque_push_back(DequeObj* d, const Value& v) {
    if (d->count == d->buf.size()) deque_grow(d);
    assign(d->at(d->count), v);
    ++d->count;
    ++d->version;
}

void deque_push_front(DequeObj* d, const Value& v) {
    if (d->count == d->buf.size()) deque_grow(d);
    d->head = (d->head + d->buf.size() - 1) & (d->buf.size() - 1);
    assign(d->buf[d->head], v);
    ++d->count;
    ++d->version;
}

Value deque_pop_back(DequeObj* d) {
    Value& slot = d->at(d->count - 1);
    Value v = slot;
    slot = Value{};
    --d->count;
    ++d->version;
    return v;
}

Value deque_pop_front(DequeObj* d) {
    Value& slot = d->buf[d->head];
    Value v = slot;
    slot = Value{};
    d->head = (d->head + 1) & (d->buf.size() - 1);
    --d->count;
    ++d->version;
    return v;
}

void deque_clear(DequeObj* d) {
    std::vector<Value> buf;
    buf.swap(d->buf);
    d->head = 0;
    d->count = 0;
    ++d->version;
    for (Value& v : buf) release(v);
}

} // namespace pluma
