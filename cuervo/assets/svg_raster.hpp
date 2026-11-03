#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/assets/image.hpp"
#include "cuervo/vector/imported_vector.hpp"

// SVG raster fallback (DESIGN.md 4.5, 11.2a): the engine's own software
// rasterizer draws a vector document into an RGBA8 image at a chosen scale
// ("target DPI"), so it can be packed into an atlas and drawn by the sprite
// batcher on every tier. It rasterizes the document's tessellation (the same
// triangles and gradient blocks the GPU path draws, vector/mesh.hpp), 4x4
// samples per pixel, so shapes look like their MSAA-drawn counterparts.
//
// Only what the importer understands is drawn: features the SVG subset skips
// (filters, masks, CSS, embedded text) stay skipped, and `unsupported` lists
// them so tools and the editor can warn. Rendering is render/asset-time only;
// nothing here feeds the simulation.
namespace cuervo::assets {

inline constexpr std::int32_t svg_raster_max_side = 4096;

struct RasterizedSvg {
    Image image;                    // straight alpha, top row first
    float pixels_per_unit = 1.0f;   // the scale it was drawn at
    vec2 document_size{0.0f, 0.0f}; // in document units
};

// Draws `vec` at `pixels_per_unit` pixels per document unit. The image is
// ceil(size * scale) per side, clamped to svg_raster_max_side (the scale
// shrinks to fit). Empty on a zero-sized document.
std::optional<RasterizedSvg> rasterize_vector(const vector::ImportedVector& vec, float pixels_per_unit);

struct SvgRasterResult {
    std::optional<RasterizedSvg> raster;
    std::vector<std::string> unsupported; // features the importer skipped (parse_svg)
    std::string error;
};

// Parses SVG text and rasterizes it.
SvgRasterResult rasterize_svg(std::string_view svg_text, float pixels_per_unit, std::string_view name = "svg");

// True when the SVG uses features outside the supported subset, i.e. when a
// raster fallback (instead of live tessellation) is what `unsupported` asks for.
inline bool needs_raster_fallback(const std::vector<std::string>& unsupported) { return !unsupported.empty(); }

} // namespace cuervo::assets
