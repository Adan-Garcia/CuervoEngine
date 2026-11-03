#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cuervo/assets/image.hpp"

// Atlas packer (DESIGN.md 4.19 "11.2a"): loose sprites -> power-of-two atlas
// pages, MaxRects with the best-short-side-fit rule. Deterministic: sprites
// are ordered by (height, width, name, input index) before packing and every
// tie is broken by free-rect order, so the same set always yields the same
// placements and byte-identical page pixels, independent of input order
// (names must be unique).
namespace cuervo::assets {

struct AtlasInput {
    std::string name;
    Image image;
};

struct AtlasOptions {
    std::int32_t max_size = 2048; // largest page side (power of two); GLES3 tier guarantees >= 2048
    std::int32_t padding = 1;     // transparent gap between sprites, in texels
    bool extrude = true;          // replicate each sprite's border texels outward by 1 so
                                  // bilinear sampling at the edge never reads a neighbour
    bool power_of_two = true;     // page sides are powers of two (pages shrink to the smallest fit)
};

struct AtlasFrame {
    std::string name;
    std::int32_t page = 0;
    // The sprite's own texels (excluding extrusion), in page pixels, top-left origin.
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;
};

struct Atlas {
    std::vector<Image> pages;        // RGBA8, straight alpha
    std::vector<AtlasFrame> frames;  // sorted by name
    const AtlasFrame* find(const std::string& name) const;
};

struct AtlasResult {
    std::optional<Atlas> atlas;
    std::string error; // empty on success
};

AtlasResult pack_atlas(const std::vector<AtlasInput>& sprites, const AtlasOptions& options = {});

} // namespace cuervo::assets
