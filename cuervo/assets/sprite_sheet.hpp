#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Sprite-sheet slicing (DESIGN.md 4.19 "11.2a"): one image -> named, indexed
// frames, either by a fixed grid or from JSON metadata. Frames are rectangles
// in the source image; the atlas packer then packs each as its own sprite.
namespace cuervo::assets {

struct SheetFrame {
    std::string name;
    std::int32_t index = 0; // position in the sheet's frame list (row-major for grids)
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;
};

struct SpriteSheet {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<SheetFrame> frames;
    const SheetFrame* find(std::string_view name) const;
};

struct SheetResult {
    std::optional<SpriteSheet> sheet;
    std::string error; // empty on success
};

struct GridOptions {
    std::int32_t frame_w = 0;
    std::int32_t frame_h = 0;
    std::int32_t margin = 0;   // border around the whole grid
    std::int32_t spacing = 0;  // gap between cells
    std::int32_t columns = 0;  // 0 = as many as fit
    std::int32_t rows = 0;     // 0 = as many as fit
    std::string prefix = "frame"; // frames are named "<prefix>_<index>" ...
    std::vector<std::string> names; // ... unless names are given (taken in order; counts must match)
};

SheetResult slice_grid(std::int32_t sheet_w, std::int32_t sheet_h, const GridOptions& options);

// JSON metadata. Accepted shapes for "frames" (extra keys are ignored):
//   array:   [{"name":"idle_0","x":0,"y":0,"w":16,"h":16}, ...]  ("frame":{x,y,w,h} also works)
//   object:  {"idle_0":{"frame":{"x":0,"y":0,"w":16,"h":16}}, ...}  (Aseprite/TexturePacker hash)
// Frame order is the file's order; every rectangle must lie inside the sheet.
SheetResult slice_json(std::string_view json, std::int32_t sheet_w, std::int32_t sheet_h);

// An atlas source file (`*.atlas`, JSON): which images and sheets pack into
// one atlas (atlas_file.hpp builds it). Example:
//   {"max_size":2048,"padding":1,"extrude":true,
//    "sprites":["art/coin.png",{"path":"art/logo.svg","scale":2,"name":"logo"}],
//    "sheets":[{"image":"art/hero.png","frame_w":16,"frame_h":16,"prefix":"hero"}]}
// A sprite is named by its path without the extension unless "name" is given;
// sheet frames are "<prefix>_<index>". SVG sprites are rasterized at `scale`
// pixels per document unit (the raster fallback, DESIGN.md 4.5).
struct AtlasSpec {
    struct Sprite {
        std::string path;
        std::string name; // empty = path without extension
        float scale = 1.0f;
    };
    struct Sheet {
        std::string image;
        std::string metadata; // JSON path for slice_json; empty = use `grid`
        GridOptions grid;
    };
    std::int32_t max_size = 2048;
    std::int32_t padding = 1;
    bool extrude = true;
    bool power_of_two = true;
    std::vector<Sprite> sprites;
    std::vector<Sheet> sheets;
};

struct AtlasSpecResult {
    std::optional<AtlasSpec> spec;
    std::string error; // empty on success
};

AtlasSpecResult parse_atlas_spec(std::string_view json);

} // namespace cuervo::assets
