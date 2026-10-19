#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Little-endian byte writer/reader for the engine's binary asset formats
// (DESIGN.md 4.11/4.12). The reader never throws: the first failure (truncated
// data, absurd counts) is latched into error() and later reads return zeros.
namespace cuervo {

static_assert(std::endian::native == std::endian::little, "binary asset I/O assumes a little-endian host");

class ByteWriter {
public:
    void u8(std::uint8_t v) { out_.push_back(static_cast<std::byte>(v)); }
    void u32(std::uint32_t v) { raw(&v, 4); }
    void i32(std::int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void u64(std::uint64_t v) { raw(&v, 8); }
    void i64(std::int64_t v) { raw(&v, 8); }
    void f64(double v) { raw(&v, 8); }
    void str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        raw(s.data(), s.size());
    }
    void raw(const void* data, std::size_t size) {
        if (size == 0) {
            return; // empty vectors may hand us a null data pointer
        }
        const auto* b = static_cast<const std::byte*>(data);
        out_.insert(out_.end(), b, b + size);
    }
    std::vector<std::byte> take() { return std::move(out_); }

private:
    std::vector<std::byte> out_;
};

class ByteReader {
public:
    // `what` prefixes error messages, e.g. "FontAtlas".
    ByteReader(std::span<const std::byte> data, std::string_view what) : data_(data), what_(what) {}

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }
    bool at_end() const { return pos_ == data_.size(); }

    void fail(std::string_view message) {
        if (ok_) {
            ok_ = false;
            error_ = std::format("{} offset {}: {}", what_, pos_, message);
        }
    }

    bool raw(void* out, std::size_t size) {
        if (size == 0) {
            return ok_;
        }
        if (!ok_ || size > data_.size() - pos_) {
            fail("truncated");
            std::memset(out, 0, size);
            return false;
        }
        std::memcpy(out, data_.data() + pos_, size);
        pos_ += size;
        return true;
    }
    std::uint8_t u8() { std::uint8_t v = 0; raw(&v, 1); return v; }
    std::uint32_t u32() { std::uint32_t v = 0; raw(&v, 4); return v; }
    std::int32_t i32() { std::int32_t v = 0; raw(&v, 4); return v; }
    float f32() { float v = 0; raw(&v, 4); return v; }
    std::uint64_t u64() { std::uint64_t v = 0; raw(&v, 8); return v; }
    std::int64_t i64() { std::int64_t v = 0; raw(&v, 8); return v; }
    double f64() { double v = 0; raw(&v, 8); return v; }

    // An element count, rejected if the remaining data can't hold that many
    // elements of `element_size` bytes (so corrupt counts never allocate).
    std::uint32_t count(std::size_t element_size) {
        const std::uint32_t n = u32();
        if (static_cast<std::uint64_t>(n) * element_size > data_.size() - pos_) {
            fail("bad element count");
            return 0;
        }
        return n;
    }
    std::string str() {
        std::string s(count(1), '\0');
        raw(s.data(), s.size());
        return s;
    }

private:
    std::span<const std::byte> data_;
    std::string_view what_;
    std::size_t pos_ = 0;
    bool ok_ = true;
    std::string error_;
};

} // namespace cuervo
