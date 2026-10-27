#pragma once

// SHA-256 (FIPS 180-4) for package and plugin checksums (milestone 10.5d,
// docs/PLUGINS.md "Sharing plugins"). Not for passwords or signatures.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace cuervo {

class Sha256 {
public:
    Sha256();
    void update(std::span<const std::byte> data);
    void update(std::string_view text) { update(std::as_bytes(std::span(text.data(), text.size()))); }
    // The digest; the object must not be updated afterwards.
    std::array<std::uint8_t, 32> finish();

private:
    void block(const std::uint8_t* p);
    std::uint32_t h_[8];
    std::uint8_t buffer_[64];
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0; // bytes
};

// Lower-case hex digest of `data`.
std::string sha256_hex(std::span<const std::byte> data);
std::string sha256_hex(std::string_view text);

} // namespace cuervo
