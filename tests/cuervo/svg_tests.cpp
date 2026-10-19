#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "cuervo/render/mesh_batcher.hpp"
#include "cuervo/vector/imported_vector.hpp"
#include "cuervo/vector/mesh.hpp"
#include "cuervo/vector/svg.hpp"
#include "cuervo/vector/vector_asset.hpp"
#include "cuervo/vector/xml.hpp"

using namespace cuervo;
using namespace cuervo::vector;

namespace {

VectorDocument load(std::string_view text, std::vector<std::string>* unsupported = nullptr) {
    SvgResult r = parse_svg(text, "test");
    REQUIRE_MESSAGE(r.ok, r.error);
    if (unsupported) *unsupported = r.unsupported;
    return r.document;
}

bool contains(const std::vector<std::string>& v, std::string_view s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

double mesh_area(const VectorMesh& mesh) {
    TriMesh t;
    for (const MeshVertex& v : mesh.vertices) t.vertices.push_back(v.position);
    t.indices = mesh.indices;
    return t.area();
}

constexpr const char* gradient_svg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="200" height="100">
  <defs>
    <linearGradient id="base" x1="0" y1="0" x2="1" y2="0" spreadMethod="reflect">
      <stop offset="0" stop-color="red"/>
      <stop offset="50%" style="stop-color:#00ff00; stop-opacity: 0.5"/>
      <stop offset="0.25" stop-color="blue"/>
    </linearGradient>
    <linearGradient id="derived" xlink:href="#base" x2="0" y2="1" gradientTransform="rotate(90)"/>
    <radialGradient id="radial" gradientUnits="userSpaceOnUse" cx="100" cy="50" r="40" fx="90" fy="45">
      <stop offset="0" stop-color="white"/><stop offset="1" stop-color="black"/>
    </radialGradient>
    <radialGradient id="single"><stop offset="0.3" stop-color="#123456"/></radialGradient>
    <radialGradient id="empty"/>
  </defs>
  <rect width="100" height="50" fill="url(#base)"/>
  <rect y="50" width="100" height="50" fill="url(#derived)"/>
  <circle cx="100" cy="50" r="40" fill="url(#radial)" stroke="url(#base)"/>
  <rect x="150" width="10" height="10" fill="url(#single)"/>
  <rect x="170" width="10" height="10" fill="url(#empty)" stroke="red"/>
</svg>)svg";

} // namespace

TEST_CASE("XML: entities, CDATA, comments, DOCTYPE, errors") {
    xml::Document doc;
    std::string error;
    REQUIRE(xml::parse(R"svg(<?xml version="1.0"?><!DOCTYPE svg [ <!ENTITY x "y"> ]><!-- c -->
        <svg:svg a='1 &amp; 2' b="&#x41;&#66;"><g><![CDATA[<raw>]]></g><p/></svg:svg>)svg", doc, &error));
    CHECK(doc.root->name == "svg");
    CHECK(*doc.root->attribute("a") == "1 & 2");
    CHECK(*doc.root->attribute("b") == "AB");
    REQUIRE(doc.root->children.size() == 2);
    CHECK(doc.root->children[0]->text == "<raw>");
    CHECK_FALSE(xml::parse("<a><b></a>", doc, &error));
    CHECK(error.find("closes") != std::string::npos);
    CHECK_FALSE(xml::parse("<a x=1/>", doc, &error));
    CHECK_FALSE(xml::parse("", doc, &error));
}

TEST_CASE("SVG: basic shapes, viewBox and transforms") {
    const VectorDocument doc = load(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 100 50">
        <g transform="translate(10 5)">
          <rect x="1" y="2" width="10" height="20" rx="2"/>
          <circle cx="5" cy="5" r="3" transform="scale(2)"/>
          <ellipse cx="5" cy="5" rx="3" ry="2"/>
          <line x1="0" y1="0" x2="5" y2="5" stroke="black"/>
          <polyline points="0,0 1,1 2,0" fill="none" stroke="black"/>
          <polygon points="0,0 1,1 2,0"/>
          <path d="M0 0 H5 V5 Z"/>
        </g>
      </svg>)svg");
    CHECK(doc.size == vec2(200, 100));
    REQUIRE(doc.shapes.size() == 7);
    // viewBox scale 2, then translate(10 5): local (0,0) -> (20, 10).
    CHECK(doc.shapes[0].transform.apply({0, 0}) == vec2(20, 10));
    CHECK(doc.shapes[1].transform.apply({1, 0}) == vec2(24, 10));
    const Bounds rect = doc.shapes[0].path.bounds();
    CHECK(rect.min == vec2(1, 2));
    CHECK(rect.max == vec2(11, 22));
    // <line> has no fill area but keeps its stroke; polyline is open, polygon closed.
    CHECK(doc.shapes[3].stroke.kind == PaintKind::Solid);
    CHECK(doc.shapes[4].fill.kind == PaintKind::None);
    CHECK(doc.shapes[4].path.verbs.back() != Verb::Close);
    CHECK(doc.shapes[5].path.verbs.back() == Verb::Close);
}

TEST_CASE("SVG: preserveAspectRatio") {
    const VectorDocument meet = load(R"svg(<svg width="200" height="100" viewBox="0 0 10 10"><rect width="10" height="10"/></svg>)svg");
    CHECK(meet.shapes[0].transform.apply({0, 0}) == vec2(50, 0)); // xMidYMid meet: scale 10, centered
    const VectorDocument none = load(
        R"svg(<svg width="200" height="100" viewBox="0 0 10 10" preserveAspectRatio="none"><rect width="10" height="10"/></svg>)svg");
    CHECK(none.shapes[0].transform.apply({10, 10}) == vec2(200, 100));
    const VectorDocument slice = load(
        R"svg(<svg width="200" height="100" viewBox="0 0 10 10" preserveAspectRatio="xMinYMin slice"><rect width="10" height="10"/></svg>)svg");
    CHECK(slice.shapes[0].transform.apply({10, 10}) == vec2(200, 200));
    const VectorDocument no_size = load(R"svg(<svg viewBox="0 0 30 20"><rect width="1" height="1"/></svg>)svg");
    CHECK(no_size.size == vec2(30, 20));
}

TEST_CASE("SVG: style attribute wins over presentation attributes; inheritance") {
    const VectorDocument doc = load(R"svg(<svg width="10" height="10">
        <g fill="red" stroke="blue" stroke-width="3" fill-opacity="0.5" opacity="0.5" color="lime">
          <rect width="1" height="1" fill="green" style="fill: #0000ff; stroke-linejoin:round ; stroke-dasharray: 4, 2"/>
          <rect width="1" height="1" opacity="0.5" fill-rule="evenodd" stroke="none"/>
          <rect width="1" height="1" fill="currentColor" stroke-linecap="square" stroke-miterlimit="10"/>
          <rect width="1" height="1" fill="inherit" style="display:none"/>
          <rect width="1" height="1" visibility="hidden"/>
        </g>
      </svg>)svg");
    REQUIRE(doc.shapes.size() == 3);
    const Shape& a = doc.shapes[0];
    CHECK(a.fill.color == Color{0, 0, 1, 1});
    CHECK(a.stroke.color == Color{0, 0, 1, 1});
    CHECK(a.stroke_style.width == 3.0f);
    CHECK(a.stroke_style.join == LineJoin::Round);
    CHECK(a.stroke_style.dashes == std::vector<float>{4, 2});
    CHECK(a.fill_opacity == 0.5f);
    CHECK(a.opacity == 0.5f);
    const Shape& b = doc.shapes[1];
    CHECK(b.fill.color == Color{1, 0, 0, 1});
    CHECK(b.fill_rule == FillRule::EvenOdd);
    CHECK(b.stroke.kind == PaintKind::None);
    CHECK(b.opacity == 0.25f); // element opacity times the group's
    const Shape& c = doc.shapes[2];
    CHECK(c.fill.color == Color{0, 1, 0, 1});
    CHECK(c.stroke_style.cap == LineCap::Square);
    CHECK(c.stroke_style.miter_limit == 10.0f);
}

TEST_CASE("SVG: gradients (stops, href inheritance, units, transform, spread)") {
    const VectorDocument doc = load(gradient_svg);
    REQUIRE(doc.shapes.size() == 5);
    REQUIRE(doc.gradients.size() == 3);

    const Shape& base_rect = doc.shapes[0];
    REQUIRE(base_rect.fill.kind == PaintKind::Gradient);
    const Gradient& base = doc.gradients[base_rect.fill.gradient];
    CHECK(base.kind == GradientKind::Linear);
    CHECK(base.bbox_units);
    CHECK(base.spread == Spread::Reflect);
    CHECK(base.p0 == vec2(0, 0));
    CHECK(base.p1 == vec2(1, 0));
    REQUIRE(base.stops.size() == 3);
    CHECK(base.stops[0].color == Color{1, 0, 0, 1});
    CHECK(base.stops[1].offset == 0.5f);
    CHECK(base.stops[1].color == Color{0, 1, 0, 0.5f}); // stop-opacity folded into alpha
    CHECK(base.stops[2].offset == 0.5f);               // offsets never decrease

    const Gradient& derived = doc.gradients[doc.shapes[1].fill.gradient];
    CHECK(derived.stops == base.stops);       // inherited through href
    CHECK(derived.spread == Spread::Reflect); // inherited attribute
    CHECK(derived.p1 == vec2(0, 1));          // own attributes win
    CHECK(std::fabs(derived.transform.apply({1, 0}).y() - 1.0f) < 1e-6f);

    const Shape& circle = doc.shapes[2];
    const Gradient& radial = doc.gradients[circle.fill.gradient];
    CHECK(radial.kind == GradientKind::Radial);
    CHECK_FALSE(radial.bbox_units);
    CHECK(radial.p0 == vec2(100, 50));
    CHECK(radial.p1 == vec2(90, 45));
    CHECK(radial.radius == 40.0f);
    CHECK(circle.stroke.kind == PaintKind::Gradient);

    // A single stop paints a solid color; no stops paints nothing (stroke stays).
    CHECK(doc.shapes[3].fill.kind == PaintKind::Solid);
    CHECK(doc.shapes[3].fill.color == Color{0x12 / 255.0f, 0x34 / 255.0f, 0x56 / 255.0f, 1});
    CHECK(doc.shapes[4].fill.kind == PaintKind::None);
    CHECK(doc.shapes[4].stroke.kind == PaintKind::Solid);
}

TEST_CASE("SVG: paint-order") {
    const VectorDocument doc = load(R"svg(<svg width="10" height="10" paint-order="stroke">
        <rect width="4" height="4" fill="red" stroke="blue" stroke-width="2"/>
        <rect width="4" height="4" fill="red" stroke="blue" paint-order="normal"/>
        <rect width="4" height="4" fill="red" stroke="blue" paint-order="markers"/>
        <rect width="4" height="4" fill="red" stroke="blue" paint-order="markers stroke fill"/></svg>)svg");
    REQUIRE(doc.shapes.size() == 4);
    CHECK(doc.shapes[0].stroke_first); // inherited
    CHECK_FALSE(doc.shapes[1].stroke_first);
    CHECK_FALSE(doc.shapes[2].stroke_first);
    CHECK(doc.shapes[3].stroke_first);
    // The stroke's triangles come first in the mesh.
    VectorDocument one = doc;
    one.shapes = {doc.shapes[0]};
    VectorMesh mesh;
    DocumentTessellator().build(one, 0, mesh);
    CHECK(mesh.vertices.front().color == pack_color({0, 0, 1, 1}));
    CHECK(mesh.vertices.back().color == pack_color({1, 0, 0, 1}));
}

TEST_CASE("SVG: paint fallbacks and patterns") {
    std::vector<std::string> unsupported;
    const VectorDocument doc = load(R"svg(<svg width="10" height="10">
        <pattern id="p" width="2" height="2"><rect width="1" height="1"/></pattern>
        <rect width="1" height="1" fill="url(#missing) #ff0000"/>
        <rect width="1" height="1" fill="url(#missing)" stroke="black"/>
        <rect width="1" height="1" fill="url(#p) blue"/>
      </svg>)svg", &unsupported);
    REQUIRE(doc.shapes.size() == 3);
    CHECK(doc.shapes[0].fill.color == Color{1, 0, 0, 1});
    CHECK(doc.shapes[1].fill.kind == PaintKind::None);
    CHECK(doc.shapes[2].fill.color == Color{0, 0, 1, 1});
    CHECK(contains(unsupported, "pattern paint"));
}

TEST_CASE("SVG: clip paths (rect, path, rule, bbox units, groups)") {
    const VectorDocument doc = load(R"svg(<svg width="100" height="100">
        <clipPath id="c1"><rect x="10" y="10" width="20" height="20"/></clipPath>
        <clipPath id="c2" clip-rule="evenodd"><path d="M0 0 H50 V50 H0 Z M10 10 H40 V40 H10 Z"/></clipPath>
        <clipPath id="bb" clipPathUnits="objectBoundingBox"><rect width="0.5" height="1"/></clipPath>
        <rect width="100" height="100" clip-path="url(#c1)"/>
        <g clip-path="url(#c2)" transform="translate(5 0)"><rect width="100" height="100"/><circle r="5"/></g>
        <rect x="20" y="20" width="40" height="10" clip-path="url(#bb)"/>
        <rect width="1" height="1" clip-path="url(#nope)"/>
      </svg>)svg");
    REQUIRE(doc.shapes.size() == 5);
    REQUIRE(doc.clip_paths.size() == 3);
    CHECK(doc.shapes[0].clips == std::vector<std::uint32_t>{0});
    CHECK(doc.shapes[1].clips == std::vector<std::uint32_t>{1});
    CHECK(doc.shapes[2].clips == std::vector<std::uint32_t>{1}); // the group's clip applies to every child
    CHECK(doc.clip_paths[1].shapes[0].rule == FillRule::EvenOdd);
    CHECK(doc.clip_paths[1].shapes[0].transform.apply({0, 0}) == vec2(5, 0)); // referencing element's space
    const ClipShape& bb = doc.clip_paths[2].shapes[0];
    CHECK(bb.transform.apply({0.5f, 1.0f}) == vec2(40, 30)); // fractions of the rect's bbox
    CHECK(doc.shapes[4].clips.empty());                      // missing clip is ignored

    // Tessellated: the clipped shapes cover exactly the expected areas.
    DocumentTessellator tess;
    VectorMesh mesh;
    VectorDocument only = doc;
    only.shapes = {doc.shapes[0]};
    tess.build(only, 0, mesh);
    CHECK(mesh_area(mesh) == doctest::Approx(400.0));
    only.shapes = {doc.shapes[1]};
    tess.build(only, 0, mesh);
    CHECK(mesh_area(mesh) == doctest::Approx(2500.0 - 900.0)); // evenodd clip leaves a frame
    only.shapes = {doc.shapes[3]};
    tess.build(only, 0, mesh);
    CHECK(mesh_area(mesh) == doctest::Approx(200.0));
}

TEST_CASE("SVG: <use>, symbols, recursion guard") {
    std::vector<std::string> unsupported;
    const VectorDocument doc = load(R"svg(<svg xmlns:xlink="http://www.w3.org/1999/xlink" width="100" height="100">
        <defs><rect id="r" width="10" height="10" fill="red"/>
          <symbol id="s" viewBox="0 0 10 10"><circle cx="5" cy="5" r="5"/></symbol></defs>
        <use xlink:href="#r" x="20" y="30" fill="blue"/>
        <use href="#s" x="50" width="20" height="20"/>
        <g id="loop"><use href="#loop"/></g>
      </svg>)svg", &unsupported);
    REQUIRE(doc.shapes.size() == 2);
    CHECK(doc.shapes[0].transform.apply({0, 0}) == vec2(20, 30));
    CHECK(doc.shapes[0].fill.color == Color{1, 0, 0, 1}); // the element's own fill wins over the use's
    CHECK(doc.shapes[1].transform.apply({10, 10}) == vec2(70, 20)); // symbol viewBox scaled into 20x20
    CHECK(contains(unsupported, "recursive <use>"));
}

TEST_CASE("SVG: unsupported features are listed and skipped") {
    std::vector<std::string> unsupported;
    const VectorDocument doc = load(R"svg(<svg width="10" height="10">
        <style>rect { fill: red }</style>
        <filter id="f"/><mask id="m"/>
        <text x="0" y="5">hi</text>
        <image href="a.png" width="1" height="1"/>
        <rect width="1" height="1" filter="url(#f)"/>
        <rect width="1" height="1" mask="url(#m)" marker-end="url(#x)"/>
        <animate attributeName="x"/>
      </svg>)svg", &unsupported);
    CHECK(doc.shapes.size() == 2); // shapes still render, without the effects
    for (const char* what : {"CSS stylesheets (<style>)", "filters", "masks", "<text>", "<image>", "markers", "SMIL animation"}) {
        CHECK_MESSAGE(contains(unsupported, what), what);
    }
}

TEST_CASE("SVG: malformed input fails cleanly") {
    CHECK_FALSE(parse_svg("<svg><g></svg>").ok);
    CHECK_FALSE(parse_svg("<html/>").ok);
    // A broken path keeps its valid prefix.
    const VectorDocument doc = load(R"svg(<svg width="10" height="10"><path d="M0 0 L5 0 L5 5 Z L"/></svg>)svg");
    CHECK(doc.shapes.size() == 1);
}

// --- meshes -------------------------------------------------------------------------

TEST_CASE("Mesh: solid and gradient paint encoding") {
    const VectorDocument doc = load(gradient_svg);
    DocumentTessellator tess;
    VectorMesh mesh;
    tess.build(doc, 0, mesh);
    REQUIRE(mesh.batches.size() == 1);
    const MeshBatch& batch = mesh.batches[0];
    CHECK(batch.first_index == 0);
    CHECK(batch.index_count == mesh.indices.size());
    // base, derived, radial fill, base as the circle's stroke: 4 blocks of 3/3/2/3 stops.
    CHECK(batch.paints.size() == gradient_block_size(3) * 3 + gradient_block_size(2));

    // First vertex belongs to the base rect: gradient block 0, white tint.
    CHECK(mesh.vertices[0].paint == 0);
    CHECK(mesh.vertices[0].color == pack_color({1, 1, 1, 1}));
    // Block 0's inverse transform maps the bbox (0..100) to 0..1.
    const auto& r0 = batch.paints[0];
    CHECK(r0[0] * 100.0f + r0[2] == doctest::Approx(1.0f));
    CHECK(r0[3] == 0.0f); // linear
    CHECK(batch.paints[1][3] == 1.0f); // reflect
    CHECK(batch.paints[3][1] == 3.0f); // stop count
    CHECK(batch.paints[4] == std::array<float, 4>{1, 0, 0, 1}); // premultiplied red
    CHECK(batch.paints[5] == std::array<float, 4>{0, 0.5f, 0, 0.5f});
    // Solid shapes use solid_paint.
    const bool any_solid = std::any_of(mesh.vertices.begin(), mesh.vertices.end(),
                                       [](const MeshVertex& v) { return v.paint == solid_paint; });
    CHECK(any_solid);
}

TEST_CASE("Mesh: many gradients split into several material batches") {
    std::string svg = R"svg(<svg width="1000" height="10"><defs>)svg";
    for (int i = 0; i < 150; ++i) {
        svg += "<linearGradient id=\"g" + std::to_string(i) +
               "\"><stop offset=\"0\" stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/></linearGradient>";
    }
    svg += "</defs>";
    for (int i = 0; i < 150; ++i) {
        svg += "<rect x=\"" + std::to_string(i * 5) + "\" width=\"4\" height=\"4\" fill=\"url(#g" + std::to_string(i) + ")\"/>";
    }
    svg += "</svg>";
    const VectorDocument doc = load(svg);
    VectorMesh mesh;
    DocumentTessellator().build(doc, 0, mesh);
    // 7 vec4 per 2-stop gradient: 146 fit in a 1024-vec4 window.
    const std::size_t per_window = paint_window_vec4 / gradient_block_size(2);
    REQUIRE(mesh.batches.size() == (150 + per_window - 1) / per_window);
    std::uint32_t covered = 0;
    for (const MeshBatch& b : mesh.batches) {
        CHECK(b.first_index == covered);
        CHECK(b.paints.size() <= paint_window_vec4);
        covered += b.index_count;
    }
    CHECK(covered == mesh.indices.size());
}

TEST_CASE("Mesh: translucent shape with an opaque stroke doesn't show its fill under the stroke") {
    const VectorDocument doc = load(R"svg(<svg width="100" height="100">
        <rect x="10" y="10" width="40" height="40" fill="red" stroke="blue" stroke-width="10" opacity="0.5"/>
        <rect x="10" y="10" width="40" height="40" fill="red" stroke="blue" stroke-width="10"/></svg>)svg");
    CHECK(stroke_occludes_fill(doc.shapes[0], doc));
    CHECK_FALSE(stroke_occludes_fill(doc.shapes[1], doc)); // opaque: no need
    ShapeGeometry translucent, opaque;
    tessellate_shape(doc.shapes[0], 0.1f, nullptr, translucent, true);
    tessellate_shape(doc.shapes[1], 0.1f, nullptr, opaque, false);
    CHECK(translucent.fill.area() == doctest::Approx(30.0 * 30.0));
    CHECK(opaque.fill.area() == doctest::Approx(40.0 * 40.0));
    CHECK(translucent.stroke.area() == doctest::Approx(opaque.stroke.area()));
}

TEST_CASE("DocumentTessellator: only changed shapes are re-tessellated") {
    VectorDocument doc = load(R"svg(<svg width="100" height="100">
        <rect width="10" height="10" fill="red"/><circle cx="50" cy="50" r="20" fill="blue"/>
        <path d="M0 0 L10 0 L5 10 Z" fill="green"/></svg>)svg");
    DocumentTessellator tess;
    VectorMesh mesh;
    tess.build(doc, 0, mesh);
    CHECK(tess.last_tessellated() == 3);
    tess.build(doc, 0, mesh);
    CHECK(tess.last_tessellated() == 0);
    CHECK(tess.last_reused() == 3);

    doc.shapes[1].fill.color = {0, 0, 0, 1}; // paint only: geometry reused, color updated
    tess.build(doc, 0, mesh);
    CHECK(tess.last_tessellated() == 0);
    doc.shapes[2].path.points[1] = vec2(20, 0); // morph one path
    const std::size_t before = mesh.indices.size();
    tess.build(doc, 0, mesh);
    CHECK(tess.last_tessellated() == 1);
    CHECK(mesh.indices.size() == before);
    tess.build(doc, 3, mesh); // new zoom level: everything
    CHECK(tess.last_tessellated() == 3);
}

TEST_CASE("VectorAsset: zoom levels are cached and change only past 2x") {
    VectorAsset asset;
    ImportedVector imported;
    imported.document = load(R"svg(<svg width="100" height="100"><circle cx="50" cy="50" r="40"/></svg>)svg");
    asset.assign(std::move(imported));
    const auto a = asset.mesh_for_scale(1.0f);
    const auto b = asset.mesh_for_scale(0.6f); // same level (0.5, 1]
    CHECK(a.level == 0);
    CHECK(b.revision == a.revision);
    const auto c = asset.mesh_for_scale(8.0f);
    CHECK(c.level == 3);
    CHECK(c.revision != a.revision);
    CHECK(c.mesh->triangle_count() > a.mesh->triangle_count()); // finer curves at 8x
    const auto d = asset.mesh_for_scale(1.0f);                  // back: cached, no rebuild
    CHECK(d.revision == a.revision);
    CHECK(asset.builds() == 2);
    asset.edit_document().shapes[0].fill = {PaintKind::Solid, {1, 0, 0, 1}, 0};
    asset.document_changed();
    CHECK(asset.mesh_for_scale(1.0f).revision != a.revision);
}

// --- binary format -------------------------------------------------------------------

TEST_CASE("ImportedVector: round trip") {
    auto imported = import_svg(gradient_svg, "test", 1);
    REQUIRE(imported);
    // Add a clip and dashes so every field is exercised.
    auto clipped = import_svg(R"svg(<svg width="50" height="50"><clipPath id="c"><circle cx="5" cy="5" r="5"/></clipPath>
        <path id="p" d="M0 0 C10 0 10 10 0 10 Q5 5 0 0 Z" stroke="red" stroke-dasharray="1 2" stroke-dashoffset="0.5"
              stroke-linecap="round" stroke-linejoin="bevel" fill-rule="evenodd" clip-path="url(#c)"/></svg>)svg",
                              "clip", 0);
    REQUIRE(clipped);
    for (const ImportedVector* asset : {&*imported, &*clipped}) {
        const std::vector<std::byte> bytes = serialize_imported_vector(*asset);
        CHECK(is_imported_vector(bytes));
        ImportedVector back;
        std::string error;
        REQUIRE_MESSAGE(deserialize_imported_vector(bytes, back, &error), error);
        CHECK(back.document == asset->document);
        REQUIRE(back.base_mesh);
        CHECK(back.base_mesh->level == asset->base_mesh->level);
        CHECK(back.base_mesh->mesh.vertices == asset->base_mesh->mesh.vertices);
        CHECK(back.base_mesh->mesh.indices == asset->base_mesh->mesh.indices);
        CHECK(back.base_mesh->mesh.batches == asset->base_mesh->mesh.batches);
        CHECK(serialize_imported_vector(back) == bytes);
    }
    CHECK(clipped->document.shapes[0].id == "p");
}

TEST_CASE("ImportedVector: rejects bad magic, newer versions, truncation and corruption") {
    auto imported = import_svg(gradient_svg, "test", 0);
    REQUIRE(imported);
    const std::vector<std::byte> bytes = serialize_imported_vector(*imported);
    ImportedVector out;
    std::string error;

    std::vector<std::byte> bad = bytes;
    bad[0] = std::byte{'X'};
    CHECK_FALSE(deserialize_imported_vector(bad, out, &error));

    bad = bytes;
    const std::uint32_t future = imported_vector_version + 1;
    std::memcpy(bad.data() + 4, &future, 4);
    CHECK_FALSE(deserialize_imported_vector(bad, out, &error));
    CHECK(error.find("newer") != std::string::npos);

    for (std::size_t cut : {std::size_t{10}, bytes.size() / 2, bytes.size() - 1}) {
        CHECK_FALSE(deserialize_imported_vector(std::span(bytes).first(cut), out, &error));
    }
    bad = bytes;
    bad.push_back(std::byte{0});
    CHECK_FALSE(deserialize_imported_vector(bad, out, &error));

    // Corrupt a count field into something enormous.
    bad = bytes;
    const std::uint32_t huge = 0xFFFFFFF0u;
    std::memcpy(bad.data() + 24, &huge, 4); // gradient count, right after the header + size
    CHECK_FALSE(deserialize_imported_vector(bad, out, &error));
}

// --- MeshBatcher draw planning ------------------------------------------------------------

TEST_CASE("MeshBatcher: consecutive instances merge into one instanced draw") {
    render::MeshBatcher batcher; // no device: plans only
    VectorMesh single;
    single.vertices.resize(3);
    single.indices = {0, 1, 2};
    single.batches = {{0, 3, {}}};
    VectorMesh split = single;
    split.vertices.resize(6);
    split.indices = {0, 1, 2, 3, 4, 5};
    split.batches = {{0, 3, {}}, {3, 3, {}}};
    const render::MeshHandle a = batcher.create_mesh(single);
    const render::MeshHandle b = batcher.create_mesh(split);

    for (int i = 0; i < 5; ++i) batcher.draw(a, Affine2::translate({float(i), 0}));
    batcher.draw(b, {});
    batcher.draw(b, {});
    batcher.draw(a, {});
    const auto calls = batcher.plan();
    REQUIRE(calls.size() == 1 + 2 + 2 + 1);
    CHECK(calls[0].instance_count == 5);
    CHECK(calls[0].first_instance == 0);
    // Multi-batch meshes draw per instance, batch by batch (painter's order).
    CHECK(calls[1].batch == 0);
    CHECK(calls[2].batch == 1);
    CHECK(calls[1].instance_count == 1);
    CHECK(calls[3].first_instance == 6);
    CHECK(calls[5].first_instance == 7);

    const render::MeshInstance inst = render::make_mesh_instance(Affine2{1, 2, 3, 4, 5, 6}, {1, 1, 1, 1});
    CHECK(inst.row0[0] == 1);
    CHECK(inst.row0[1] == 3);
    CHECK(inst.row0[2] == 5);
    CHECK(inst.row1[0] == 2);
    CHECK(inst.row1[1] == 4);
    CHECK(inst.row1[2] == 6);
    CHECK(inst.tint == 0xFFFFFFFFu);
    batcher.destroy_mesh(a);
    batcher.clear();
    batcher.draw(a, {}); // destroyed: ignored
    CHECK(batcher.plan().empty());
}
