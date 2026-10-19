#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cuervo/math/affine2.hpp"
#include "cuervo/vector/document.hpp"

// SVG importer for the practical subset of DESIGN.md 4.5:
//   <svg> (viewBox + preserveAspectRatio, nested <svg>), <g>, <a>, <switch>,
//   <defs>, <symbol>, <use>, <path> (all commands, arcs), <rect> (rx/ry),
//   <circle>, <ellipse>, <line>, <polyline>, <polygon>;
//   fill / stroke paints (colors, currentColor, url(#gradient) with fallback),
//   fill-rule, stroke-width / -linejoin / -linecap / -miterlimit /
//   -dasharray / -dashoffset, opacity / fill-opacity / stroke-opacity,
//   <linearGradient> / <radialGradient> (gradientUnits, gradientTransform,
//   spreadMethod, href inheritance, focal point), transforms, <clipPath>
//   (clipPathUnits, clip-rule, path and basic-shape children),
//   `style` attributes and presentation attributes, display / visibility,
//   paint-order (fill vs stroke).
//
// Everything else (filters, masks, patterns, markers, CSS <style> sheets,
// <text>, <image>, SMIL animation, ...) is skipped; each skipped feature is
// listed once in SvgResult::unsupported and in one warning in the log.
// DESIGN.md 4.5 plans a rasterized fallback for those; it isn't built yet.
namespace cuervo::vector {

struct SvgResult {
    bool ok = false;
    std::string error;                   // set when !ok (malformed XML, no <svg> root)
    VectorDocument document;
    std::vector<std::string> unsupported; // sorted, unique
};

// `name` is only used in log messages.
SvgResult parse_svg(std::string_view text, std::string_view name = "svg");

// SVG transform list ("translate(10) rotate(45 5 5) matrix(...)"). Returns
// false on a syntax error (out is then the identity, as SVG ignores the attribute).
bool parse_transform(std::string_view text, Affine2& out);

// CSS/SVG color: #rgb, #rgba, #rrggbb, #rrggbbaa, rgb()/rgba() (integers or
// percentages), the 147 named colors, "transparent". Not "currentColor" or
// "none" (paint-level keywords).
bool parse_color(std::string_view text, Color& out);

} // namespace cuervo::vector
