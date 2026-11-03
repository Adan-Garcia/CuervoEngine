// Right-to-left UI mirroring and EN / AR / JA parity (DESIGN.md 4.19 "11.3").
// The same menu is built under three locales and checked for identical
// structure, mirrored geometry in Arabic, and software-rendered text (the
// engine's MSDF atlas sampled on the CPU) that fits its widgets.

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cuervo/render/font.hpp"
#include "cuervo/render/font_import.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/render/text_renderer.hpp"
#include "cuervo/script/script_host.hpp"
#include "cuervo/vector/imported_vector.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

struct Hosted {
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;

    Hosted(const std::string& source, const render::FontAtlas* font = nullptr,
           std::vector<const render::FontAtlas*> fallbacks = {}) {
        script::HostOptions o;
        o.font = font;
        o.font_fallbacks = std::move(fallbacks);
        host = std::make_unique<script::ScriptHost>(o, map);
        REQUIRE_MESSAGE(host->load({{"main.plm", source}}, "Main"), host->error());
        for (int i = 0; i < 2; ++i) {
            host->tick(InputFrame{});
            REQUIRE_MESSAGE(host->running(), host->error());
        }
    }
    pluma::Value global(std::string_view n) { return host->session()->global(n); }
};

const pluma::Value& field(const pluma::Value& obj, std::string_view name) {
    const auto* o = static_cast<const pluma::InstanceObj*>(obj.o);
    for (std::size_t i = 0; i < o->type->field_names.size(); ++i) {
        if (o->type->field_names[i] == name) return o->fields[i];
    }
    FAIL("no field " << name);
    static pluma::Value none;
    return none;
}

vec2 field_vec2(const pluma::Value& obj, std::string_view name) {
    const float* d = pluma::vec_data(field(obj, name));
    return {d[0], d[1]};
}

struct Rect {
    vec2 pos, size;
    float left() const { return pos.x(); }
    float right() const { return pos.x() + size.x(); }
};

Rect rect_of(const pluma::Value& node) { return {field_vec2(node, "position"), field_vec2(node, "size")}; }

std::vector<pluma::Value> children_of(const pluma::Value& node) {
    return static_cast<const pluma::ListObj*>(field(node, "children").o)->items;
}

// A row of two fixed-size buttons and a start-aligned label in a Margin; the
// locale picks the strings (Locale.add) before the tree is built.
constexpr const char* menu_source = R"(
class T {
    static var margin: Margin
    static var row: HBox
    static var first: Button
    static var second: Button
    static var label: Label
}
class Main extends Node {
    fn ready() {
        Locale.add("en", "title", "Settings and options")
        Locale.add("ar", "title", "الإعدادات والخيارات")
        Locale.add("ja", "title", "設定とオプション")
        Locale.add("en", "play", "Play")
        Locale.add("ar", "play", "العب")
        Locale.add("ja", "play", "プレイ")
        Locale.add("en", "quit", "Quit")
        Locale.add("ar", "quit", "خروج")
        Locale.add("ja", "quit", "終了")
        Locale.set_locale("@LOCALE@")
        var layer = CanvasLayer()
        add_child(layer)
        var margin = Margin()
        margin.margins = vec4(20.0, 10.0, 60.0, 10.0)
        margin.set_anchors(#top_left)
        var row = HBox()
        row.separation = 16.0
        var first = Button()
        first.text = tr(#play)
        first.min_size = vec2(160.0, 48.0)
        var second = Button()
        second.text = tr(#quit)
        second.min_size = vec2(120.0, 48.0)
        var label = Label()
        label.text = tr(#title)
        label.align = #start
        label.min_size = vec2(260.0, 48.0)
        row.add_child(first)
        row.add_child(second)
        row.add_child(label)
        margin.add_child(row)
        layer.add_child(margin)
        T.margin = margin
        T.row = row
        T.first = first
        T.second = second
        T.label = label
    }
}
)";

std::string menu_for(const std::string& locale) {
    std::string s = menu_source;
    s.replace(s.find("@LOCALE@"), 8, locale);
    return s;
}

TEST_CASE("ui rtl: containers mirror in a right-to-left locale and only then") {
    Hosted en(menu_for("en"));
    Hosted ar(menu_for("ar"));
    const Rect en_margin = rect_of(en.global("T.margin"));
    const Rect ar_margin = rect_of(ar.global("T.margin"));
    CHECK(en_margin.pos == ar_margin.pos);
    CHECK(en_margin.size == ar_margin.size);

    const auto en_kids = children_of(en.global("T.row"));
    const auto ar_kids = children_of(ar.global("T.row"));
    REQUIRE(en_kids.size() == 3);
    REQUIRE(ar_kids.size() == 3);
    // English: first, second, label from the left, after the 20 px left margin.
    CHECK(rect_of(en_kids[0]).left() == doctest::Approx(en_margin.left() + 20.0f));
    CHECK(rect_of(en_kids[0]).right() < rect_of(en_kids[1]).left());
    CHECK(rect_of(en_kids[1]).right() < rect_of(en_kids[2]).left());
    // Arabic: the same rects reflected inside the margin's rect, and the 20/60 margins swap sides.
    for (std::size_t i = 0; i < 3; ++i) {
        const Rect e = rect_of(en_kids[i]);
        const Rect a = rect_of(ar_kids[i]);
        CHECK(a.size == e.size);
        CHECK(a.pos.y() == e.pos.y());
        CHECK(a.right() <= en_margin.right() - 20.0f + 0.01f); // inside the mirrored margins
    }
    CHECK(rect_of(ar_kids[0]).left() > rect_of(ar_kids[1]).left());
    CHECK(rect_of(ar_kids[1]).left() > rect_of(ar_kids[2]).left());
    // The right margin of the left-to-right layout (60) becomes the left margin.
    CHECK(rect_of(ar.global("T.row")).left() == doctest::Approx(en_margin.left() + 60.0f));
    CHECK(rect_of(en.global("T.row")).left() == doctest::Approx(en_margin.left() + 20.0f));
}

TEST_CASE("ui rtl: mirror can be turned off globally and per container") {
    Hosted ar(menu_for("ar"));
    const float mirrored_first = rect_of(children_of(ar.global("T.row"))[0]).left();

    // Globally: UI.x.mirror_rtl = false before layout.
    std::string off = menu_for("ar");
    off.replace(off.find("var layer = CanvasLayer()"), 0, "UI.x.mirror_rtl = false\n        ");
    Hosted flat(off);
    const auto kids = children_of(flat.global("T.row"));
    CHECK(rect_of(kids[0]).left() < rect_of(kids[1]).left());
    CHECK(rect_of(kids[0]).left() != doctest::Approx(mirrored_first));

    // Per container: HBox.mirror = false keeps its order while the Margin still mirrors.
    std::string keep = menu_for("ar");
    keep.replace(keep.find("row.separation = 16.0"), 0, "row.mirror = false\n        ");
    Hosted partial(keep);
    const auto pk = children_of(partial.global("T.row"));
    CHECK(rect_of(pk[0]).left() < rect_of(pk[1]).left());
}

constexpr const char* split_scroll_source = R"(
class T {
    static var split: SplitContainer
    static var first: Control
    static var second: Control
    static var scroll: ScrollContainer
    static var content: Control
    static var wide: ScrollContainer
    static var wide_content: Control
    static var bar_nudge = 0.0
    static var split_after = 0.0
}
class Main extends Node {
    fn ready() {
        Locale.add("en", "x", "x")
        Locale.add("ar", "x", "x")
        Locale.set_locale("@LOCALE@")
        var layer = CanvasLayer()
        add_child(layer)
        var box = VBox()
        box.mirror = false
        var s = SplitContainer()
        s.min_size = vec2(400.0, 100.0)
        s.bar_size = 10.0
        var a = Control()
        a.min_size = vec2(20.0, 20.0)
        var b = Control()
        b.min_size = vec2(20.0, 20.0)
        s.add_child(a)
        s.add_child(b)
        var sc = ScrollContainer()
        sc.min_size = vec2(200.0, 100.0)
        var c = Control()
        c.min_size = vec2(100.0, 500.0)
        sc.add_child(c)
        var w = ScrollContainer()
        w.min_size = vec2(200.0, 100.0)
        w.vertical = false
        w.horizontal = true
        var wc = Control()
        wc.min_size = vec2(600.0, 50.0)
        w.add_child(wc)
        box.add_child(s)
        box.add_child(sc)
        box.add_child(w)
        layer.add_child(box)
        T.split = s
        T.first = a
        T.second = b
        T.scroll = sc
        T.content = c
        T.wide = w
        T.wide_content = wc
    }
}
)";

std::string split_scroll_for(const std::string& locale) {
    std::string s = split_scroll_source;
    s.replace(s.find("@LOCALE@"), 8, locale);
    return s;
}

TEST_CASE("ui rtl: split and scroll containers mirror in a right-to-left locale") {
    Hosted en(split_scroll_for("en"));
    Hosted ar(split_scroll_for("ar"));
    // Split: the first child is on the left in English and on the right in Arabic; the bar swaps sides.
    const Rect en_split = rect_of(en.global("T.split"));
    const Rect ar_split = rect_of(ar.global("T.split"));
    CHECK(en_split.pos == ar_split.pos);
    const Rect e1 = rect_of(en.global("T.first")), e2 = rect_of(en.global("T.second"));
    const Rect a1 = rect_of(ar.global("T.first")), a2 = rect_of(ar.global("T.second"));
    CHECK(e1.left() == doctest::Approx(en_split.left()));
    CHECK(e1.right() < e2.left());
    CHECK(a1.right() == doctest::Approx(ar_split.right()));
    CHECK(a2.right() < a1.left());
    CHECK(a1.size == e1.size);
    CHECK(a2.size == e2.size);
    CHECK(a2.left() == doctest::Approx(ar_split.left()));

    // Scroll: the vertical bar's width is taken on the left, so the content starts after it.
    const Rect e_sc = rect_of(en.global("T.scroll")), a_sc = rect_of(ar.global("T.scroll"));
    const Rect e_c = rect_of(en.global("T.content")), a_c = rect_of(ar.global("T.content"));
    CHECK(e_c.left() == doctest::Approx(e_sc.left()));
    CHECK(e_c.right() == doctest::Approx(e_sc.right() - 10.0f));
    CHECK(a_c.left() == doctest::Approx(a_sc.left() + 10.0f));
    CHECK(a_c.right() == doctest::Approx(a_sc.right()));
    CHECK(a_c.size == e_c.size);
    // Horizontal scrolling starts at the right edge in Arabic, the left in English.
    const Rect e_w = rect_of(en.global("T.wide")), a_w = rect_of(ar.global("T.wide"));
    CHECK(rect_of(en.global("T.wide_content")).left() == doctest::Approx(e_w.left()));
    CHECK(rect_of(ar.global("T.wide_content")).right() == doctest::Approx(a_w.right()));
}

#if defined(CUERVO_TEST_FONT_ARABIC) && defined(CUERVO_TEST_FONT_JAPANESE)

std::vector<std::byte> read_bytes(const std::string& path) {
    std::vector<std::byte> data;
    vector::read_file(path, data);
    return data;
}

struct Fonts {
    render::FontAtlas latin, arabic, japanese;
};

const Fonts* test_fonts() {
    static const std::optional<Fonts> fonts = [] () -> std::optional<Fonts> {
        if (!render::font_import_available() || !text::shaping_available()) return std::nullopt;
        render::FontImportOptions lo;
        lo.em_size = 32.0f;
        auto latin = render::import_font(read_bytes(std::string(CUERVO_ASSETS_DIR) + "/fonts/LiberationSans-Regular.ttf"),
                                         "Latin", lo);
        render::FontImportOptions ao;
        ao.ranges = {{0x20, 0x7E}, {0x600, 0x6FF}, {0xFB50, 0xFDFF}, {0xFE70, 0xFEFF}};
        ao.em_size = 32.0f;
        ao.shaping = true;
        auto arabic = render::import_font(read_bytes(CUERVO_TEST_FONT_ARABIC), "Arabic", ao);
        render::FontImportOptions jo;
        jo.ranges = {{0x20, 0x7E}, {0x3000, 0x30FF}, {0x4E86, 0x4E86}, {0x5B9A, 0x5B9A}, {0x7D42, 0x7D42},
                     {0x8A2D, 0x8A2D}, {0x30AA, 0x30AA}};
        jo.em_size = 32.0f;
        jo.shaping = true;
        auto japanese = render::import_font(read_bytes(CUERVO_TEST_FONT_JAPANESE), "Japanese", jo);
        if (!latin || !arabic || !japanese) return std::nullopt;
        return Fonts{std::move(*latin), std::move(*arabic), std::move(*japanese)};
    }();
    return fonts ? &*fonts : nullptr;
}

// A tiny software renderer for the text queue: samples the MSDF atlas the way
// text.frag does (median of rgb, smoothed by the screen pixel range) into a
// grayscale image. The screenshot stand-in of the parity test.
struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> ink; // 0..255 coverage

    Image(int w, int h) : width(w), height(h), ink(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0) {}

    float at(int x, int y) const { return ink[static_cast<std::size_t>(y * width + x)]; }
    std::uint64_t ink_pixels() const {
        std::uint64_t n = 0;
        for (std::uint8_t v : ink) n += v > 127 ? 1 : 0;
        return n;
    }
    // Bounding box of pixels above half coverage, {-1,...} when empty.
    bool bbox(int& x0, int& y0, int& x1, int& y1) const {
        x0 = width, y0 = height, x1 = -1, y1 = -1;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if (ink[static_cast<std::size_t>(y * width + x)] > 127) {
                    x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
                }
            }
        }
        return x1 >= 0;
    }
    void write_ppm(const fs::path& path) const {
        std::ofstream out(path, std::ios::binary);
        out << "P5\n" << width << " " << height << "\n255\n";
        out.write(reinterpret_cast<const char*>(ink.data()), static_cast<std::streamsize>(ink.size()));
    }
};

float median3(float a, float b, float c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }

void rasterize(Image& image, const render::TextRenderer& text) {
    for (const render::TextRenderer::DrawCall& call : text.plan()) {
        const render::FontAtlas* atlas = text.font(call.font);
        REQUIRE(atlas != nullptr);
        for (std::uint32_t i = 0; i < call.instance_count; ++i) {
            const render::GlyphInstance& g = text.instances()[call.first_instance + i];
            const float w = g.axis_x.x();
            const float h = g.axis_y.y();
            if (w <= 0.0f || h <= 0.0f) continue;
            const float tex_w = (g.uv_max.x() - g.uv_min.x()) * static_cast<float>(atlas->width);
            const float screen_range = std::max(1.0f, g.distance_range * (w / tex_w));
            const int x0 = std::max(0, static_cast<int>(std::floor(g.origin.x())));
            const int y0 = std::max(0, static_cast<int>(std::floor(g.origin.y())));
            const int x1 = std::min(image.width - 1, static_cast<int>(std::ceil(g.origin.x() + w)));
            const int y1 = std::min(image.height - 1, static_cast<int>(std::ceil(g.origin.y() + h)));
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const float u = g.uv_min.x() + (static_cast<float>(x) + 0.5f - g.origin.x()) / w * (g.uv_max.x() - g.uv_min.x());
                    const float v = g.uv_min.y() + (static_cast<float>(y) + 0.5f - g.origin.y()) / h * (g.uv_max.y() - g.uv_min.y());
                    const int tx = std::clamp(static_cast<int>(u * static_cast<float>(atlas->width)), 0, atlas->width - 1);
                    const int ty = std::clamp(static_cast<int>(v * static_cast<float>(atlas->height)), 0, atlas->height - 1);
                    const std::uint8_t* px = &atlas->pixels[static_cast<std::size_t>(ty * atlas->width + tx) * 4];
                    const float sd = median3(px[0], px[1], px[2]) / 255.0f - 0.5f;
                    const float alpha = std::clamp(sd * screen_range + 0.5f, 0.0f, 1.0f);
                    std::uint8_t& dst = image.ink[static_cast<std::size_t>(y * image.width + x)];
                    dst = static_cast<std::uint8_t>(std::max<float>(dst, alpha * 255.0f));
                }
            }
        }
    }
}

TEST_CASE("ui locale parity: the same menu in EN, AR and JA fits and mirrors") {
    const Fonts* fonts = test_fonts();
    if (!fonts) return;
    const fs::path out = fs::temp_directory_path() / "engine_ui_locale_tests";
    fs::create_directories(out);

    struct Shot {
        Rect row, first, second, label;
        std::uint64_t ink = 0;
        int ink_left = 0, ink_right = 0;
    };
    std::vector<Shot> shots;
    for (const char* locale : {"en", "ar", "ja"}) {
        render::TextRenderer text;
        const render::FontHandle latin = text.add_font(fonts->latin);
        const render::FontHandle arabic = text.add_font(fonts->arabic);
        const render::FontHandle japanese = text.add_font(fonts->japanese);
        text.set_fallbacks(latin, {arabic, japanese});
        Hosted h(menu_for(locale), &fonts->latin, {&fonts->arabic, &fonts->japanese});
        render::SdfBatcher shapes;
        h.host->draw(shapes, text, latin);

        const auto kids = children_of(h.global("T.row"));
        REQUIRE(kids.size() == 3);
        Shot s{rect_of(h.global("T.row")), rect_of(kids[0]), rect_of(kids[1]), rect_of(kids[2])};

        Image image(800, 320);
        rasterize(image, text);
        image.write_ppm(out / (std::string("menu_") + locale + ".pgm"));
        s.ink = image.ink_pixels();
        int y0 = 0, y1 = 0;
        REQUIRE(image.bbox(s.ink_left, y0, s.ink_right, y1));
        CHECK(s.ink > 400); // every locale shows its text
        // The text stays inside the row (nothing clipped, nothing spilling out).
        CHECK(static_cast<float>(s.ink_left) >= s.row.left() - 4.0f);
        CHECK(static_cast<float>(s.ink_right) <= s.row.right() + 4.0f);
        // The glyphs come from the right atlas: Arabic and Japanese leave the Latin font.
        std::vector<render::FontHandle> used;
        for (const auto& call : text.plan()) used.push_back(call.font);
        const bool uses_arabic = std::find(used.begin(), used.end(), arabic) != used.end();
        const bool uses_japanese = std::find(used.begin(), used.end(), japanese) != used.end();
        CHECK(uses_arabic == (std::string(locale) == "ar"));
        CHECK(uses_japanese == (std::string(locale) == "ja"));
        shots.push_back(s);
    }

    // Widget geometry is the same size in every locale (fixed min sizes), and the
    // Arabic row is the English one reflected.
    for (const Shot& s : shots) {
        // Fixed min sizes give the widths; line height comes from the primary font alone, so
        // a button is exactly as tall in every language.
        CHECK(s.first.size.x() == shots[0].first.size.x());
        CHECK(s.second.size.x() == shots[0].second.size.x());
        CHECK(s.label.size.x() == shots[0].label.size.x());
        CHECK(s.first.size.y() == shots[0].first.size.y());
        CHECK(s.second.size.y() == shots[0].second.size.y());
        CHECK(s.label.size.y() == shots[0].label.size.y());
        CHECK(s.row.size.y() == shots[0].row.size.y());
    }
    CHECK(shots[2].first.pos == shots[0].first.pos); // JA lays out like EN
    CHECK(shots[1].first.left() > shots[1].second.left());
    CHECK(shots[0].first.left() < shots[0].second.left());
    // The start-aligned label hugs its container's reading-start edge: left in EN/JA, right in AR.
    CHECK(shots[1].label.left() < shots[1].second.left());
}

TEST_CASE("LineEdit: caret, hit testing and composition follow bidi order in Arabic text") {
    const Fonts* fonts = test_fonts();
    if (!fonts) return;
    Hosted h(R"(
class T {
    static var ar: LineEdit
    static var en: LineEdit
    static var mixed: LineEdit
    static var ar_rtl = false
    static var en_rtl = true
    static var ar_end = -1
    static var ar_left_from_end = -1
    static var ar_right_from_end = -1
    static var en_left_from_end = -1
    static var x0 = 0.0
    static var x4 = 0.0
    static var m3 = 0.0
    static var m7 = 0.0
    static var hit_left = -1
    static var hit_right = -1
}
class Main extends Node {
    fn ready() {
        var layer = CanvasLayer()
        add_child(layer)
        var box = VBox()
        T.ar = LineEdit()
        T.ar.min_size = vec2(300.0, 40.0)
        T.ar.set_text("العب")
        T.en = LineEdit()
        T.en.min_size = vec2(300.0, 40.0)
        T.en.set_text("abcd")
        T.mixed = LineEdit()
        T.mixed.set_text("ab العب")
        box.add_child(T.ar)
        box.add_child(T.en)
        box.add_child(T.mixed)
        layer.add_child(box)
    }
    fn update(dt: float) {
        if Time.tick == 2 {
            var fs = 24.0
            T.ar_rtl = T.ar.text_is_rtl()
            T.en_rtl = T.en.text_is_rtl()
            T.ar.activate()
            T.en.activate()
            T.ar_end = T.ar.caret
            T.ar.nudge(vec2(-1.0, 0.0))
            T.ar_left_from_end = T.ar.caret
            T.ar.nudge(vec2(1.0, 0.0))
            T.ar_right_from_end = T.ar.caret
            T.en.nudge(vec2(-1.0, 0.0))
            T.en_left_from_end = T.en.caret
            T.x0 = UI.caret_x("العب", fs, 0)
            T.x4 = UI.caret_x("العب", fs, 4)
            T.m3 = UI.caret_x("ab العب", fs, 3)
            T.m7 = UI.caret_x("ab العب", fs, 7)
            var r = T.ar.position
            T.ar.pointer_down(r + vec2(T.ar.size.x - 2.0, 20.0))
            T.hit_right = T.ar.caret
            T.ar.pointer_down(r + vec2(2.0, 20.0))
            T.hit_left = T.ar.caret
        }
    }
}
)", &fonts->latin, {&fonts->arabic, &fonts->japanese});
    for (int i = 0; i < 2; ++i) h.host->tick(InputFrame{});
    CHECK(h.global("T.ar_rtl").b);
    CHECK_FALSE(h.global("T.en_rtl").b);
    // Arabic: the end of the text is at the left, so left from the end has nowhere to go and
    // right steps back into the text. English is the reverse.
    CHECK(h.global("T.ar_end").i == 4);
    CHECK(h.global("T.ar_left_from_end").i == 4);
    CHECK(h.global("T.ar_right_from_end").i == 3);
    CHECK(h.global("T.en_left_from_end").i == 3);
    // Carets run right to left: index 0 at the origin (the right edge), the end to its left.
    CHECK(h.global("T.x0").f == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(h.global("T.x4").f < -10.0f);
    // Left-to-right paragraph with an Arabic tail: the run's start is at its right edge.
    CHECK(h.global("T.m3").f > h.global("T.m7").f);
    // Clicking near the right edge of an Arabic field is before the first character.
    CHECK(h.global("T.hit_right").i == 0);
    CHECK(h.global("T.hit_left").i == 4);
}

TEST_CASE("ui locale parity: start-aligned label text anchors to the reading start") {
    const Fonts* fonts = test_fonts();
    if (!fonts) return;
    float left_gap[2] = {}, right_gap[2] = {};
    int i = 0;
    for (const char* locale : {"en", "ar"}) {
        render::TextRenderer text;
        const render::FontHandle latin = text.add_font(fonts->latin);
        const render::FontHandle arabic = text.add_font(fonts->arabic);
        text.set_fallbacks(latin, {arabic});
        Hosted h(menu_for(locale), &fonts->latin, {&fonts->arabic});
        render::SdfBatcher shapes;
        h.host->draw(shapes, text, latin);
        const Rect label = rect_of(h.global("T.label"));
        // The label's glyphs are the last ones queued; find the extent of quads inside its rect.
        float lo = 1e9f, hi = -1e9f;
        for (const render::GlyphInstance& g : text.instances()) {
            const float cx = g.origin.x() + g.axis_x.x() * 0.5f;
            if (cx >= label.left() && cx <= label.right()) {
                lo = std::min(lo, g.origin.x());
                hi = std::max(hi, g.origin.x() + g.axis_x.x());
            }
        }
        REQUIRE(hi > lo);
        left_gap[i] = lo - label.left();
        right_gap[i] = label.right() - hi;
        ++i;
    }
    CHECK(left_gap[0] < right_gap[0]);  // English hugs the left edge
    CHECK(right_gap[1] < left_gap[1]);  // Arabic hugs the right edge
}

#endif // fonts

} // namespace
