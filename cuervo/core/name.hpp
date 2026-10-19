#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace cuervo {

// Interned string: the C++ side of the language's `#name` type. Comparing two
// Names compares integers. Ids are handed out in first-use order, so they are
// stable within a run but must not be written to save files (store the text).
//
// The intern table is not thread-safe; create Names on the main thread.
class Name {
public:
    constexpr Name() = default;
    explicit Name(std::string_view text);

    std::string_view str() const;
    constexpr std::uint32_t id() const { return id_; }
    constexpr bool empty() const { return id_ == 0; }

    friend constexpr bool operator==(Name a, Name b) { return a.id_ == b.id_; }

private:
    std::uint32_t id_ = 0; // 0 is the empty name
};

inline namespace literals {
inline Name operator""_name(const char* text, std::size_t size) { return Name({text, size}); }
} // namespace literals

} // namespace cuervo

template<>
struct std::hash<cuervo::Name> {
    std::size_t operator()(cuervo::Name name) const noexcept { return name.id(); }
};
