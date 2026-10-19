#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cuervo/math/affine2.hpp"
#include "cuervo/math/vec.hpp"
#include "cuervo/vector/path.hpp"
#include "cuervo/vector/stroke.hpp"
#include "cuervo/vector/tessellate.hpp"

// A resolved vector document (DESIGN.md 4.5): what the SVG importer produces
// and what the ImportedVector binary format stores. Styles are fully resolved
// (inheritance, `style` attributes, gradient hrefs, `use` references are gone),
// group transforms are folded into each shape's transform, but curves are
// kept, so the runtime can re-flatten them at any zoom.
namespace cuervo::vector {

// Straight (non-premultiplied) sRGB color, components in [0, 1]. Colors are
// blended in sRGB space, as browsers and vector tools do (DESIGN.md 7.2).
struct Color {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
    friend bool operator==(const Color&, const Color&) = default;
};

enum class GradientKind : std::uint8_t { Linear, Radial };
enum class Spread : std::uint8_t { Pad, Reflect, Repeat };

struct GradientStop {
    float offset = 0.0f; // [0, 1], non-decreasing
    Color color;         // stop-opacity folded into color.a
    friend bool operator==(const GradientStop&, const GradientStop&) = default;
};

struct Gradient {
    GradientKind kind = GradientKind::Linear;
    Spread spread = Spread::Pad;
    // gradientUnits="objectBoundingBox" (the SVG default): coordinates are
    // fractions of the painted shape's bounding box.
    bool bbox_units = true;
    Affine2 transform; // gradientTransform: gradient space -> (bbox or user) space
    // Linear: p0 = (x1, y1), p1 = (x2, y2).
    // Radial: p0 = center (cx, cy), p1 = focal point (fx, fy), radius = r.
    vec2 p0{0.0f, 0.0f};
    vec2 p1{1.0f, 0.0f};
    float radius = 0.5f;
    std::vector<GradientStop> stops;
    friend bool operator==(const Gradient&, const Gradient&) = default;
};

enum class PaintKind : std::uint8_t { None, Solid, Gradient };

struct Paint {
    PaintKind kind = PaintKind::None;
    Color color;               // Solid
    std::uint32_t gradient = 0; // Gradient: index into VectorDocument::gradients
    friend bool operator==(const Paint&, const Paint&) = default;
};

// One clip shape; its region is `path` (in its own coordinates) mapped by
// `transform` into document space, filled with `rule`.
struct ClipShape {
    Path path;
    Affine2 transform;
    FillRule rule = FillRule::NonZero;
    friend bool operator==(const ClipShape&, const ClipShape&) = default;
};

// A clip region: the union of its shapes. Every reference to a <clipPath>
// becomes its own entry, with the referencing element's transform applied.
struct ClipPath {
    std::vector<ClipShape> shapes;
    friend bool operator==(const ClipPath&, const ClipPath&) = default;
};

struct Shape {
    std::string id;     // SVG id (empty if none); groups' ids are kept for future cutout animation
    Path path;          // in the shape's own user space
    Affine2 transform;  // user space -> document space (all ancestors' transforms + viewBox)
    Paint fill;
    FillRule fill_rule = FillRule::NonZero;
    float fill_opacity = 1.0f;
    Paint stroke;
    StrokeStyle stroke_style;
    float stroke_opacity = 1.0f;
    bool stroke_first = false; // paint-order puts the stroke under the fill
    // Element opacity times every ancestor group's opacity. Group opacity is
    // applied per shape, not as an offscreen layer (see cuervo/vector/README.md).
    float opacity = 1.0f;
    std::vector<std::uint32_t> clips; // indices into VectorDocument::clip_paths; all apply (intersection)
    friend bool operator==(const Shape&, const Shape&) = default;
};

struct VectorDocument {
    vec2 size{0.0f, 0.0f}; // viewport size in document units (SVG width/height)
    std::vector<Shape> shapes; // in paint order
    std::vector<Gradient> gradients;
    std::vector<ClipPath> clip_paths;
    friend bool operator==(const VectorDocument&, const VectorDocument&) = default;
};

} // namespace cuervo::vector
