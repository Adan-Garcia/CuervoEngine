#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <vector>

namespace pluma {

// A read-only array view whose storage lives in an Arena. AST child lists use
// this instead of std::vector so every node is trivially destructible and the
// whole tree is freed at once with its arena.
template <class T>
struct List {
    T* data = nullptr;
    std::uint32_t count = 0;

    T* begin() const { return data; }
    T* end() const { return data + count; }
    std::uint32_t size() const { return count; }
    bool empty() const { return count == 0; }
    T& operator[](std::size_t i) const { return data[i]; }
    T& back() const { return data[count - 1]; }
};

// Bump allocator for AST nodes. Nodes must be trivially destructible: the arena
// releases memory without running destructors.
class Arena {
public:
    Arena() = default;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = default;
    Arena& operator=(Arena&&) = default;

    void* allocate(std::size_t size, std::size_t align) {
        std::size_t pad = (align - (reinterpret_cast<std::uintptr_t>(cursor_) & (align - 1))) & (align - 1);
        if (cursor_ == nullptr || pad + size > left_) {
            std::size_t block = size + align > block_size ? size + align : block_size;
            blocks_.push_back(std::make_unique<std::byte[]>(block));
            cursor_ = blocks_.back().get();
            left_ = block;
            pad = (align - (reinterpret_cast<std::uintptr_t>(cursor_) & (align - 1))) & (align - 1);
        }
        std::byte* out = cursor_ + pad;
        cursor_ = out + size;
        left_ -= pad + size;
        bytes_used_ += size;
        return out;
    }

    template <class T>
    T* make() {
        static_assert(std::is_trivially_destructible_v<T>, "arena objects are never destroyed");
        return new (allocate(sizeof(T), alignof(T))) T();
    }

    template <class T>
    List<T> list(const std::vector<T>& items) {
        static_assert(std::is_trivially_destructible_v<T>, "arena objects are never destroyed");
        List<T> out;
        if (items.empty()) {
            return out;
        }
        out.data = static_cast<T*>(allocate(sizeof(T) * items.size(), alignof(T)));
        for (std::size_t i = 0; i < items.size(); ++i) {
            new (out.data + i) T(items[i]);
        }
        out.count = static_cast<std::uint32_t>(items.size());
        return out;
    }

    std::string_view str(std::string_view s) {
        if (s.empty()) {
            return {};
        }
        char* p = static_cast<char*>(allocate(s.size(), 1));
        std::memcpy(p, s.data(), s.size());
        return {p, s.size()};
    }

    std::size_t bytes_used() const { return bytes_used_; }

private:
    static constexpr std::size_t block_size = 64 * 1024;
    std::vector<std::unique_ptr<std::byte[]>> blocks_;
    std::byte* cursor_ = nullptr;
    std::size_t left_ = 0;
    std::size_t bytes_used_ = 0;
};

} // namespace pluma
