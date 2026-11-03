#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/assets/atlas.hpp"
#include "cuervo/assets/image.hpp"
#include "cuervo/assets/sprite_sheet.hpp"

// Atlas assets (DESIGN.md 4.19 "11.2a"): an `*.atlas` source file (JSON, see
// AtlasSpec) is built into pages with their mip chains plus the named frames,
// and stored as the engine format `*.catlas` ("CATL") that exports pack, so
// shipping builds never decode source images or run the packer. Building is
// deterministic: the same spec and sources give byte-identical `.catlas` data.
namespace cuervo::assets {

struct PackedAtlas {
    std::vector<TextureData> pages; // each with its mip chain; sprites are sRGB colour data
    std::vector<AtlasFrame> frames; // sorted by name
    const AtlasFrame* find(std::string_view name) const;
};

// Reads a project file (a Vfs open, a disk read ...). Returns false with `error` set.
using FileReader = std::function<bool(std::string_view path, std::vector<std::byte>& out, std::string& error)>;

struct AtlasBuildResult {
    std::optional<PackedAtlas> atlas;
    std::vector<std::string> warnings; // e.g. SVG features the raster fallback can't draw
    std::string error;                 // empty on success
};

// Reads every source the spec names through `read` (PNG, JPEG, QOI, SVG or
// `.evec`; sheet images are sliced into frames) and packs them.
AtlasBuildResult build_atlas(const AtlasSpec& spec, const FileReader& read);
AtlasBuildResult build_atlas_from_json(std::string_view spec_json, const FileReader& read);

// "CATL" (little-endian): magic, u32 version, u32 page count, u32 frame
// count; per page u32 size + a CTEX blob; per frame u32 name length, the
// name, then i32 page, x, y, w, h.
std::vector<std::byte> encode_catlas(const PackedAtlas& atlas);
std::optional<PackedAtlas> decode_catlas(std::span<const std::byte> bytes);

} // namespace cuervo::assets
