#pragma once

// Run-time operations of the Milestone 9.2a containers (LANGUAGE.md 9.16):
// the insertion-ordered hash table behind Map and Set, the Deque ring buffer,
// and the stable merge sort used by `List.sort`/`sort_by` and PriorityQueue
// ordering helpers.
//
// Determinism (DESIGN.md 4.1): nothing here iterates in hash order. Hashes
// only place entries in the lookup table; every observable order (iteration,
// keys(), values(), to_list()) is the dense insertion order. The sort is our
// own merge sort rather than std::stable_sort, so its result for a given
// comparator is the same on every standard library, even when a script's
// comparator is not a strict weak order.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pluma/value.hpp"

namespace pluma {

// ---- keys: integers, bool, str, name, plain enums and class references

std::uint64_t key_hash(const Value& k);
bool key_equal(const Value& a, const Value& b);

// ---- HashObj (Map / Set)

// Index of the entry holding `k`, or -1.
std::int64_t hash_find(const HashObj* h, const Value& k);
// Inserts `k` (and, for a Map, sets its value to `v`). Returns true when the
// key was new. Keys and values are retained.
bool hash_put(HashObj* h, const Value& k, const Value* v);
// Removes `k`; returns true if it was present.
bool hash_remove(HashObj* h, const Value& k);
void hash_clear(HashObj* h);
// Next live entry at or after `pos`, or keys.size().
inline std::size_t hash_next(const HashObj* h, std::size_t pos) {
    while (pos < h->keys.size() && h->keys[pos].tag == Tag::Nil) ++pos;
    return pos;
}

// ---- DequeObj

void deque_push_back(DequeObj* d, const Value& v);
void deque_push_front(DequeObj* d, const Value& v);
// The removed value's reference moves to the caller. The deque must not be empty.
Value deque_pop_back(DequeObj* d);
Value deque_pop_front(DequeObj* d);
void deque_clear(DequeObj* d);

// ---- sorting

// Stable merge sort of `v` by `less` (a callable `bool(const Value&, const
// Value&)` that may throw a panic). Only Value bits are moved, never
// refcounts, so a panic leaves `v` a permutation of its input.
template <class Less>
void merge_sort(std::vector<Value>& v, Less&& less) {
    const std::size_t n = v.size();
    if (n < 2) return;
    // Insertion sort for short runs, then bottom-up merges.
    constexpr std::size_t run = 16;
    for (std::size_t lo = 0; lo < n; lo += run) {
        std::size_t hi = lo + run < n ? lo + run : n;
        for (std::size_t i = lo + 1; i < hi; ++i) {
            Value x = v[i];
            std::size_t j = i;
            try {
                while (j > lo && less(x, v[j - 1])) {
                    v[j] = v[j - 1];
                    --j;
                }
            } catch (...) {
                v[j] = x; // slot j holds a duplicate of j + 1: keep a permutation
                throw;
            }
            v[j] = x;
        }
    }
    std::vector<Value> buf(n);
    for (std::size_t width = run; width < n; width *= 2) {
        for (std::size_t lo = 0; lo < n; lo += 2 * width) {
            std::size_t mid = lo + width < n ? lo + width : n;
            std::size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            std::size_t i = lo;
            std::size_t j = mid;
            std::size_t k = lo;
            while (i < mid && j < hi) buf[k++] = less(v[j], v[i]) ? v[j++] : v[i++];
            while (i < mid) buf[k++] = v[i++];
            while (j < hi) buf[k++] = v[j++];
        }
        v.swap(buf);
    }
}

// The total order `sort()` uses for floats: ordinary `<`, with every NaN
// after every number (NaNs equal to each other). -0.0 and 0.0 are equal, so
// they keep their order (the sort is stable).
template <class F>
bool float_sort_less(F a, F b) {
    const bool an = a != a;
    const bool bn = b != b;
    if (an || bn) return !an && bn;
    return a < b;
}

} // namespace pluma
