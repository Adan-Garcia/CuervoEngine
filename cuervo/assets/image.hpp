#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Raster image import (DESIGN.md 4.19 "11.2a", slice 1): decodes PNG, JPEG and
// QOI to straight-alpha RGBA8, builds the mip chain at import time, and writes
// the engine's texture asset ("CTEX") into `.import/` so shipping builds never
// decode source formats. WebP is deferred: no small pinned decoder is
// practical (libwebp is large), so it is reported as unsupported.
//
// Everything here is integer-only and therefore bit-identical on every host.
namespace cuervo::assets {

struct Image {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4, top row first, straight alpha
};

enum class ImageFormat : std::uint8_t { Unknown, Png, Jpeg, Qoi, WebP };

ImageFormat sniff_image_format(std::span<const std::byte> bytes);

struct DecodeResult {
    std::optional<Image> image;
    std::string error; // empty on success
};

// Decodes by magic number, not extension. Rejects images larger than
// `max_dimension` per side (untrusted input: bounds the allocation).
DecodeResult decode_image(std::span<const std::byte> bytes, std::int32_t max_dimension = 16384);

// A texture ready for the RHI: level 0 first, each level tightly packed,
// back to back in `data`.
struct TextureData {
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool srgb = true; // colour data (sprites) true; data textures (normals, masks) false
    std::vector<std::uint8_t> data;
    std::vector<std::size_t> level_offsets; // byte offset of each level in `data`

    std::int32_t mip_count() const { return static_cast<std::int32_t>(level_offsets.size()); }
    std::int32_t level_width(std::int32_t level) const;
    std::int32_t level_height(std::int32_t level) const;
    std::span<const std::uint8_t> level(std::int32_t level) const;
};

// Box-filter mip chain down to 1x1. sRGB textures average in linear light with
// alpha weighting (so transparent texels don't bleed colour); non-sRGB
// textures average the bytes directly. `max_levels` 0 = the full chain.
TextureData build_texture(const Image& image, bool srgb, std::int32_t max_levels = 0);

// The "CTEX" file (little-endian): magic, u32 version, u32 width, height,
// flags (bit 0 = sRGB), mip count, then the levels. Deterministic bytes.
std::vector<std::byte> encode_ctex(const TextureData& texture);
std::optional<TextureData> decode_ctex(std::span<const std::byte> bytes);

struct ImportOptions {
    bool srgb = true;
    std::int32_t max_levels = 0;
};

struct ImportResult {
    std::filesystem::path output; // empty on failure
    std::string error;
};

// source -> `<import_dir>/<source filename>.ctex`. Creates import_dir.
ImportResult import_image(const std::filesystem::path& source, const std::filesystem::path& import_dir,
                          const ImportOptions& options = {});

} // namespace cuervo::assets
