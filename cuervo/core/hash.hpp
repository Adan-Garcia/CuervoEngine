#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace cuervo {

// FNV-1a over raw bytes: a cheap, stable fingerprint of simulation state for
// replay comparison (DESIGN.md 4.1, "verified by replay comparison"). Not for
// hash tables or anything adversarial.
class StateHasher {
public:
    void bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash_ = (hash_ ^ p[i]) * 0x100000001b3ULL;
        }
    }

    template<typename T>
        requires std::is_trivially_copyable_v<T>
    void value(const T& v) {
        bytes(&v, sizeof(T));
    }

    template<typename T>
        requires std::is_trivially_copyable_v<T>
    void span(std::span<const T> values) {
        value(static_cast<std::uint64_t>(values.size()));
        bytes(values.data(), values.size_bytes());
    }

    std::uint64_t digest() const { return hash_; }

private:
    std::uint64_t hash_ = 0xcbf29ce484222325ULL;
};

} // namespace cuervo
