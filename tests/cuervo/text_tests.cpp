#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "cuervo/render/font.hpp"
#include "cuervo/render/font_import.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/render/text_layout.hpp"
#include "cuervo/render/text_renderer.hpp"
#include "cuervo/vector/imported_vector.hpp"
#include "recording_device.hpp"

using namespace cuervo;
using namespace cuervo::render;
using cuervo::test::RecordingDevice;

namespace {

// A tiny hand-made atlas: glyph metrics chosen so layout results are exact.
//   'A' advance 0.6, 'V' advance 0.6, kerning A->V = -0.1
//   'i' advance 0.25, ' ' advance 0.25 (no quad), '?' advance 0.5
// Every visible glyph's quad is (0.05, -0.75)..(advance - 0.05, 0.05) ems and
// occupies a 10x10 texel cell of a 64x16 atlas.
FontAtlas make_test_font() {
    FontAtlas font;
    font.name = "test";
    font.em_size = 10.0f;
    font.distance_range = 2.0f;
    font.metrics = {0.8f, -0.2f, 1.25f, -0.1f, 0.05f};
    font.width = 64;
    font.height = 16;
    font.pixels.assign(64 * 16 * 4, std::uint8_t{0});
    float cell = 0.0f;
    const auto glyph = [&](std::uint32_t cp, float advance, bool visible) {
        FontGlyph g;
        g.codepoint = cp;
        g.advance = advance;
        if (visible) {
            g.left = 0.05f;
            g.top = -0.75f;
            g.right = advance - 0.05f;
            g.bottom = 0.05f;
            g.atlas_left = cell;
            g.atlas_top = 2.0f;
            g.atlas_right = cell + 10.0f;
            g.atlas_bottom = 12.0f;
            cell += 12.0f;
        }
        return g;
    };
    font.glyphs = {glyph(' ', 0.25f, false), glyph('?', 0.5f, true), glyph('A', 0.6f, true),
                   glyph('V', 0.6f, true), glyph('i', 0.25f, true)};
    font.kerning = {{'A', 'V', -0.1f}};
    return font;
}

std::string assets_dir() { return CUERVO_ASSETS_DIR; }

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// Imports the bundled font once per test binary (a few hundred ms).
const FontAtlas* bundled_font() {
    static const std::optional<FontAtlas> font = [] {
        std::vector<std::byte> data;
        if (!font_import_available() ||
            !vector::read_file(assets_dir() + "/fonts/LiberationSans-Regular.ttf", data)) {
            return std::optional<FontAtlas>{};
        }
        return import_font(data, "LiberationSans-Regular");
    }();
    return font ? &*font : nullptr;
}

std::uint8_t median_at(const FontAtlas& font, int x, int y) {
    const std::uint8_t* p = font.pixels.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(font.width) +
                                                  static_cast<std::size_t>(x)) * 4;
    return std::max(std::min(p[0], p[1]), std::min(std::max(p[0], p[1]), p[2]));
}

} // namespace

TEST_CASE("decode_utf8 decodes 1-4 byte sequences") {
    CHECK(decode_utf8("A") == std::vector<std::uint32_t>{'A'});
    CHECK(decode_utf8("\xC3\xA9") == std::vector<std::uint32_t>{0xE9});           // é
    CHECK(decode_utf8("\xE2\x82\xAC") == std::vector<std::uint32_t>{0x20AC});     // €
    CHECK(decode_utf8("\xF0\x9F\x98\x80") == std::vector<std::uint32_t>{0x1F600}); // 😀
    CHECK(decode_utf8("a\xC3\xA9z") == std::vector<std::uint32_t>{'a', 0xE9, 'z'});
    CHECK(decode_utf8("").empty());
}

TEST_CASE("decode_utf8 replaces malformed input with U+FFFD and keeps going") {
    CHECK(decode_utf8("\x80") == std::vector<std::uint32_t>{0xFFFD});                 // stray continuation
    CHECK(decode_utf8("\xC0\x80") == std::vector<std::uint32_t>{0xFFFD, 0xFFFD});     // overlong NUL
    CHECK(decode_utf8("\xED\xA0\x80") == std::vector<std::uint32_t>{0xFFFD, 0xFFFD, 0xFFFD}); // surrogate
    CHECK(decode_utf8("\xF4\x90\x80\x80").front() == 0xFFFD);                          // > U+10FFFF
    CHECK(decode_utf8("\xE2\x82") == std::vector<std::uint32_t>{0xFFFD, 0xFFFD});     // truncated
    CHECK(decode_utf8("\xC3x") == std::vector<std::uint32_t>{0xFFFD, 'x'});           // bad continuation
}

TEST_CASE("FontAtlas lookups: glyphs by codepoint, kerning by pair") {
    const FontAtlas font = make_test_font();
    CHECK(validate_font_atlas(font).empty());
    REQUIRE(font.find('A') != nullptr);
    CHECK(font.find('A')->advance == 0.6f);
    CHECK(font.find('B') == nullptr);
    CHECK(font.kerning_between('A', 'V') == -0.1f);
    CHECK(font.kerning_between('V', 'A') == 0.0f);
    CHECK(fallback_codepoint(font) == '?');
}

TEST_CASE("layout_text: advances, kerning and quads on one line") {
    const FontAtlas font = make_test_font();
    TextStyle style;
    style.size = 20.0f;
    const TextLayout layout = layout_text(font, "AV", style);
    REQUIRE(layout.lines.size() == 1);
    REQUIRE(layout.glyphs.size() == 2);
    // Width: 0.6 + (-0.1 + 0.6) = 1.1 em = 22 units.
    CHECK(near(layout.lines[0].width, 22.0f));
    CHECK(near(layout.size().x(), 22.0f));
    // Top anchor: baseline at the ascender (0.8 em = 16 units).
    CHECK(near(layout.lines[0].baseline, 16.0f));
    // 'A' quad from its plane bounds; 'V' is pulled left by the kerning.
    CHECK(near(layout.glyphs[0].min.x(), 1.0f));
    CHECK(near(layout.glyphs[0].min.y(), 16.0f - 15.0f));
    CHECK(near(layout.glyphs[0].max.y(), 16.0f + 1.0f));
    CHECK(near(layout.glyphs[1].min.x(), (0.6f - 0.1f + 0.05f) * 20.0f));
    // uv = atlas texels / atlas size.
    CHECK(near(layout.glyphs[0].uv_min.x(), 12.0f / 64.0f)); // 'A' is the second cell
    CHECK(near(layout.glyphs[0].uv_min.y(), 2.0f / 16.0f));
    CHECK(near(layout.glyphs[0].uv_max.y(), 12.0f / 16.0f));
    // Height: ascender + descender = 1.0 em.
    CHECK(near(layout.size().y(), 20.0f));
}

TEST_CASE("layout_text: whitespace has no quads, unknown codepoints use the fallback") {
    const FontAtlas font = make_test_font();
    TextStyle style;
    style.size = 10.0f;
    const TextLayout layout = layout_text(font, "A A\xE2\x82\xAC", style); // "A A€", € not in the font
    REQUIRE(layout.glyphs.size() == 3);
    CHECK(layout.glyphs[2].codepoint == '?');
    CHECK(near(layout.lines[0].width, (0.6f + 0.25f + 0.6f + 0.5f) * 10.0f));
    // Tabs advance by tab_width spaces.
    const TextLayout tabbed = layout_text(font, "A\tA", style);
    CHECK(near(tabbed.lines[0].width, (0.6f + 4 * 0.25f + 0.6f) * 10.0f));
}

TEST_CASE("layout_text: line breaks, line spacing and baselines") {
    const FontAtlas font = make_test_font();
    TextStyle style;
    style.size = 10.0f;
    style.line_spacing = 2.0f;
    const TextLayout layout = layout_text(font, "A\nVV\r\ni", style);
    REQUIRE(layout.lines.size() == 3);
    CHECK(near(layout.lines[0].baseline, 8.0f));
    CHECK(near(layout.lines[1].baseline, 8.0f + 25.0f)); // 1.25 em x 10 x 2
    CHECK(near(layout.lines[2].baseline, 8.0f + 50.0f));
    CHECK(layout.lines[1].glyph_count == 2);
    CHECK(layout.lines[2].glyph_count == 1); // "\r\n" is one break
    CHECK(near(layout.size().x(), 12.0f));
    CHECK(near(layout.size().y(), 8.0f + 50.0f + 2.0f));
    // An empty string is one empty line.
    const TextLayout empty = layout_text(font, "", style);
    CHECK(empty.lines.size() == 1);
    CHECK(empty.glyphs.empty());
}

TEST_CASE("layout_text: horizontal alignment per line and vertical anchors") {
    const FontAtlas font = make_test_font();
    TextStyle style;
    style.size = 10.0f;
    style.align = TextAlign::Center;
    const TextLayout centered = layout_text(font, "A\nii", style);
    CHECK(near(centered.lines[0].x, -3.0f));
    CHECK(near(centered.lines[1].x, -2.5f));
    CHECK(near(centered.min.x(), -3.0f));
    CHECK(near(centered.max.x(), 3.0f));
    style.align = TextAlign::Right;
    const TextLayout right = layout_text(font, "A\nii", style);
    CHECK(near(right.lines[0].x, -6.0f));
    CHECK(near(right.lines[1].x, -5.0f));
    CHECK(near(right.max.x(), 0.0f));

    style.align = TextAlign::Left;
    style.baseline = TextBaseline::Alphabetic;
    CHECK(near(layout_text(font, "A", style).lines[0].baseline, 0.0f));
    style.baseline = TextBaseline::Bottom;
    const TextLayout bottom = layout_text(font, "A", style);
    CHECK(near(bottom.max.y(), 0.0f));
    CHECK(near(bottom.lines[0].baseline, -2.0f)); // descender is 0.2 em
    style.baseline = TextBaseline::Middle;
    const TextLayout middle = layout_text(font, "A", style);
    CHECK(near(middle.min.y(), -5.0f));
    CHECK(near(middle.max.y(), 5.0f));
}

TEST_CASE("layout_text: word wrap breaks at spaces and trims them") {
    const FontAtlas font = make_test_font();
    TextStyle style;
    style.size = 10.0f;
    style.max_width = 15.0f; // "AV" = 11 fits, "AV AV" = 26.5 doesn't
    const TextLayout layout = layout_text(font, "AV  AV AV", style);
    REQUIRE(layout.lines.size() == 3);
    for (const TextLine& line : layout.lines) {
        CHECK(near(line.width, 11.0f));
        CHECK(line.glyph_count == 2);
    }
    // A word longer than the width stays whole on its own line.
    style.max_width = 5.0f;
    const TextLayout narrow = layout_text(font, "AVAV i", style);
    REQUIRE(narrow.lines.size() == 2);
    CHECK(narrow.lines[0].glyph_count == 4);
    CHECK(narrow.lines[1].glyph_count == 1);
    // Without max_width nothing wraps.
    style.max_width = 0.0f;
    CHECK(layout_text(font, "AV  AV AV", style).lines.size() == 1);
}

TEST_CASE("FontAtlas binary round trip and corrupt data") {
    const FontAtlas font = make_test_font();
    const std::vector<std::byte> bytes = serialize_font_atlas(font);
    CHECK(is_font_atlas(bytes));

    FontAtlas loaded;
    std::string error;
    REQUIRE(deserialize_font_atlas(bytes, loaded, &error));
    CHECK(loaded.name == font.name);
    CHECK(loaded.em_size == font.em_size);
    CHECK(loaded.metrics.line_height == font.metrics.line_height);
    CHECK(loaded.pixels == font.pixels);
    REQUIRE(loaded.glyphs.size() == font.glyphs.size());
    CHECK(loaded.glyphs[2].atlas_right == font.glyphs[2].atlas_right);
    CHECK(loaded.kerning_between('A', 'V') == -0.1f);

    // Truncated anywhere: rejected, never crashes.
    for (std::size_t cut : {std::size_t{3}, std::size_t{17}, bytes.size() / 2, bytes.size() - 1}) {
        FontAtlas partial;
        CHECK_FALSE(deserialize_font_atlas(std::span(bytes).first(cut), partial, &error));
    }
    // Newer version: rejected with a clear error.
    std::vector<std::byte> newer = bytes;
    const std::uint32_t future = font_atlas_version + 1;
    std::memcpy(newer.data() + 4, &future, 4);
    CHECK_FALSE(deserialize_font_atlas(newer, loaded, &error));
    CHECK(error.find("version") != std::string::npos);
    // Unsorted glyphs fail validation.
    FontAtlas unsorted = make_test_font();
    std::swap(unsorted.glyphs[0], unsorted.glyphs[1]);
    CHECK_FALSE(validate_font_atlas(unsorted).empty());
    CHECK_FALSE(deserialize_font_atlas(serialize_font_atlas(unsorted), loaded, &error));
}

TEST_CASE("import_font: the bundled TTF becomes a valid MTSDF atlas") {
    const FontAtlas* font = bundled_font();
    if (!font_import_available()) {
        MESSAGE("font import compiled out; skipping");
        return;
    }
    REQUIRE(font != nullptr);
    CHECK(validate_font_atlas(*font).empty());
    // Printable ASCII + Latin-1 (U+00A0..U+00FF): Liberation Sans has all of them.
    CHECK(font->glyphs.size() == 95 + 96);
    CHECK(font->width == font->height);
    CHECK(font->width % 4 == 0);
    CHECK(font->width <= 1024);
    CHECK(font->em_size == 48.0f);
    // Liberation Sans metrics (2048 units/em): ascender 1854, descender -434.
    CHECK(near(font->metrics.ascender, 1854.0f / 2048.0f, 1e-3f));
    CHECK(near(font->metrics.descender, -434.0f / 2048.0f, 1e-3f));
    CHECK(font->metrics.line_height > 1.0f);
    // The font's kern table: A followed by V tightens (-152/2048 em).
    CHECK(near(font->kerning_between('A', 'V'), -152.0f / 2048.0f, 1e-4f));
    CHECK(!font->kerning.empty());

    const FontGlyph* space = font->find(' ');
    REQUIRE(space != nullptr);
    CHECK_FALSE(space->has_quad());
    CHECK(space->advance > 0.0f);

    // 'I' is a plain vertical bar: the field is inside (median > 0.5) at the
    // middle of its quad and outside at the quad's corner (the range margin).
    const FontGlyph* bar = font->find('I');
    REQUIRE(bar != nullptr);
    REQUIRE(bar->has_quad());
    CHECK(bar->top < 0.0f);
    CHECK(near(bar->bottom - bar->top, (bar->atlas_bottom - bar->atlas_top) / font->em_size, 1e-3f));
    const int cx = static_cast<int>((bar->atlas_left + bar->atlas_right) * 0.5f);
    const int cy = static_cast<int>((bar->atlas_top + bar->atlas_bottom) * 0.5f);
    CHECK(median_at(*font, cx, cy) > 160);
    CHECK(median_at(*font, static_cast<int>(bar->atlas_left) + 1, static_cast<int>(bar->atlas_top) + 1) < 96);
    // Glyph rectangles never overlap.
    for (std::size_t i = 0; i < font->glyphs.size(); ++i) {
        for (std::size_t j = i + 1; j < font->glyphs.size(); ++j) {
            const FontGlyph& a = font->glyphs[i];
            const FontGlyph& b = font->glyphs[j];
            if (!a.has_quad() || !b.has_quad()) continue;
            const bool overlap = a.atlas_left < b.atlas_right && b.atlas_left < a.atlas_right &&
                                 a.atlas_top < b.atlas_bottom && b.atlas_top < a.atlas_bottom;
            CHECK_FALSE(overlap);
        }
    }
}

TEST_CASE("import_font: deterministic regardless of thread count, survives .efont round trip") {
    if (!font_import_available()) {
        return;
    }
    std::vector<std::byte> data;
    REQUIRE(vector::read_file(assets_dir() + "/fonts/LiberationSans-Regular.ttf", data));
    FontImportOptions options;
    options.ranges = {{'0', '9'}, {'A', 'Z'}};
    options.threads = 1;
    const std::optional<FontAtlas> one = import_font(data, "a", options);
    options.threads = 4;
    const std::optional<FontAtlas> four = import_font(data, "a", options);
    REQUIRE(one);
    REQUIRE(four);
    CHECK(one->glyphs.size() == 36);
    CHECK(one->pixels == four->pixels);

    FontAtlas loaded;
    REQUIRE(deserialize_font_atlas(serialize_font_atlas(*one), loaded));
    CHECK(loaded.pixels == one->pixels);
    CHECK(serialize_font_atlas(loaded) == serialize_font_atlas(*one));

    // Not a font: a clean failure.
    const std::vector<std::byte> junk(64, std::byte{0x42});
    CHECK_FALSE(is_sfnt_font(junk));
    CHECK_FALSE(import_font(junk, "junk"));
}

TEST_CASE("layout_text with the bundled font: kerning tightens 'AV'") {
    const FontAtlas* font = bundled_font();
    if (!font) {
        return;
    }
    TextStyle style;
    style.size = 100.0f;
    const float av = layout_text(*font, "AV", style).lines[0].width;
    const float a = layout_text(*font, "A", style).lines[0].width;
    const float v = layout_text(*font, "V", style).lines[0].width;
    CHECK(av < a + v);
    CHECK(near(av, a + v + font->kerning_between('A', 'V') * 100.0f, 1e-2f));
}

TEST_CASE("GlyphInstance layout matches the text.vert instance stream") {
    CHECK(sizeof(GlyphInstance) == 48);
    CHECK(offsetof(GlyphInstance, uv_min) == 24);
    CHECK(offsetof(GlyphInstance, color) == 40);
    CHECK(offsetof(GlyphInstance, distance_range) == 44);
    CHECK(sizeof(TextGlobals) == 32);
}

TEST_CASE("make_glyph_instance maps the quad through the transform") {
    LaidOutGlyph glyph;
    glyph.min = {1.0f, 2.0f};
    glyph.max = {4.0f, 7.0f};
    glyph.uv_min = {0.25f, 0.5f};
    glyph.uv_max = {0.75f, 1.0f};
    const GlyphInstance moved = make_glyph_instance(glyph, Affine2::translate({10.0f, 20.0f}), 0x01020304u, 3.0f);
    CHECK(moved.origin == vec2{11.0f, 22.0f});
    CHECK(moved.axis_x == vec2{3.0f, 0.0f});
    CHECK(moved.axis_y == vec2{0.0f, 5.0f});
    CHECK(moved.uv_max == vec2{0.75f, 1.0f});
    CHECK(moved.color == 0x01020304u);
    CHECK(moved.distance_range == 3.0f);
    // A quarter turn clockwise (y down): +x maps to +y, +y maps to -x.
    const GlyphInstance turned = make_glyph_instance(glyph, Affine2::rotate(1.5707963f), 0, 1.0f);
    CHECK(near(turned.axis_x.x(), 0.0f));
    CHECK(near(turned.axis_x.y(), 3.0f));
    CHECK(near(turned.axis_y.x(), -5.0f));
    CHECK(near(turned.axis_y.y(), 0.0f));
}

TEST_CASE("TextRenderer batches consecutive text of one font into one draw") {
    TextRenderer text;
    const FontHandle a = text.add_font(make_test_font());
    const FontHandle b = text.add_font(make_test_font());
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a != b);
    CHECK(text.font(a)->name == "test");

    TextStyle style;
    style.size = 10.0f;
    text.draw(a, "AV", vec2{0.0f, 0.0f}, style, {1, 1, 1, 1});
    text.draw(a, "i i", vec2{0.0f, 50.0f}, style, {1, 0, 0, 1});
    text.draw(b, "A", vec2{0.0f, 0.0f}, style, {1, 1, 1, 1});
    text.draw(a, "?", vec2{0.0f, 0.0f}, style, {1, 1, 1, 1});
    text.draw(a, "   ", vec2{0.0f, 0.0f}, style, {1, 1, 1, 1}); // nothing visible: no run
    CHECK(text.instances().size() == 6);
    const auto plan = text.plan();
    REQUIRE(plan.size() == 3);
    CHECK(plan[0].font == a);
    CHECK(plan[0].first_instance == 0);
    CHECK(plan[0].instance_count == 4);
    CHECK(plan[1].font == b);
    CHECK(plan[1].instance_count == 1);
    CHECK(plan[2].first_instance == 5);
    // Colors are packed per text, positions offset by the draw position.
    CHECK(text.instances()[2].color == pack_rgba8({1, 0, 0, 1}));
    CHECK(text.instances()[2].origin.y() > 50.0f);

    // Invalid atlases are rejected.
    FontAtlas broken = make_test_font();
    broken.pixels.pop_back();
    CHECK_FALSE(text.add_font(broken));
    text.clear();
    CHECK(text.plan().empty());
}

TEST_CASE("TextRenderer uploads atlases and draws one instanced call per font run") {
    RecordingDevice device;
    TextRenderer text;
    const FontHandle early = text.add_font(make_test_font()); // before init: uploaded by init()
    REQUIRE(text.init(device));
    const FontHandle late = text.add_font(make_test_font());
    REQUIRE(device.textures.size() == 2);
    for (const auto& [id, texture] : device.textures) {
        CHECK(texture.desc.width == 64);
        CHECK(texture.desc.format == rhi::TextureFormat::RGBA8);
        CHECK(texture.desc.filter == rhi::Filter::Linear);
    }
    REQUIRE(device.pipelines.size() == 1);
    CHECK(device.pipelines[0].blend == rhi::BlendMode::Premultiplied);
    CHECK(device.pipelines[0].has_spirv);
    CHECK(device.pipelines[0].has_es300);
    REQUIRE(device.pipelines[0].textures.size() == 1);
    CHECK(device.pipelines[0].textures[0] == "u_atlas");
    REQUIRE(device.pipelines[0].streams.size() == 2);
    CHECK(device.pipelines[0].streams[1].stride == sizeof(GlyphInstance));
    CHECK(device.pipelines[0].streams[1].rate == rhi::VertexRate::PerInstance);

    TextStyle style;
    text.draw(early, "AV", vec2{0.0f, 0.0f}, style, {1, 1, 1, 1});
    text.draw(late, "i", vec2{0.0f, 0.0f}, style, {1, 1, 1, 1});
    text.flush(Affine2{});
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].instance_count == 2);
    CHECK(device.draws[0].index_count == 6);
    CHECK(device.draws[1].first_instance == 2);
    CHECK(device.draws[0].textures[0] != device.draws[1].textures[0]);
    CHECK(device.textures.contains(device.draws[0].textures[0]));
    CHECK(text.last_draw_calls() == 2);

    text.remove_font(late);
    CHECK(device.textures.size() == 1);
    text.shutdown();
    CHECK(device.textures.empty());
}

TEST_CASE("TextRenderer: each flush draws only the text queued since the previous one") {
    RecordingDevice device;
    TextRenderer text;
    REQUIRE(text.init(device));
    const FontHandle font = text.add_font(make_test_font());
    TextStyle style;
    text.draw(font, "AV", vec2{0.0f, 0.0f}, style, {1, 1, 1, 1}); // world labels
    text.flush(Affine2{});
    text.draw(font, "i", vec2{0.0f, 0.0f}, style, {1, 1, 1, 1}); // HUD, same font: a new run
    text.flush(Affine2::scale({2.0f, 2.0f}));
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].instance_count == 2);
    CHECK(device.draws[1].first_instance == 2);
    CHECK(device.draws[1].instance_count == 1);
    CHECK(text.plan().size() == 2);
    // The second upload wrote only the new instance, after the first two.
    const std::vector<std::byte>& gpu = device.buffers.at(device.draws[1].vertex_buffers[1]);
    GlyphInstance uploaded;
    std::memcpy(&uploaded, gpu.data() + 2 * sizeof(GlyphInstance), sizeof(GlyphInstance));
    CHECK(uploaded.origin == text.instances()[2].origin);
    // Nothing new queued: a flush draws nothing.
    text.flush(Affine2{});
    CHECK(device.draws.size() == 2);
    CHECK(text.last_draw_calls() == 0);
    text.clear();
    text.draw(font, "A", vec2{0.0f, 0.0f}, style, {1, 1, 1, 1});
    text.flush(Affine2{});
    REQUIRE(device.draws.size() == 3);
    CHECK(device.draws[2].first_instance == 0);
}
