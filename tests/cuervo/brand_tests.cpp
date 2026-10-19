// Brand integration (DESIGN.md 4.16, milestone 8.1): the generated color tokens
// match assets/brand/tokens.json and both themes are complete; the synced logo
// SVGs import with the engine's own SVG importer; the embedded icon is a PNG.

#include <doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>

#include "cuervo/brand/brand.hpp"
#include "cuervo/vector/imported_vector.hpp"
#include "cuervo/vector/svg.hpp"
#include "pluma/json.hpp"

using namespace cuervo;

namespace {

std::string brand_dir() { return std::string(CUERVO_ASSETS_DIR) + "/brand"; }

std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(static_cast<bool>(in), path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::uint32_t rgba8(rhi::Color c) {
    auto ch = [](float v) { return static_cast<std::uint32_t>(v * 255.0f + 0.5f); };
    return ch(c.r) << 24 | ch(c.g) << 16 | ch(c.b) << 8 | ch(c.a);
}

// A token's value with "{a.b.c}" references followed, as #RRGGBB -> 0xRRGGBBFF.
std::uint32_t token_rgba(const pluma::json::Value& root, std::string value) {
    for (int depth = 0; value.starts_with('{') && depth < 8; ++depth) {
        const pluma::json::Value* v = &root;
        std::string_view path(value);
        path = path.substr(1, path.size() - 2);
        while (!path.empty() && v != nullptr) {
            const std::size_t dot = path.find('.');
            v = v->get(path.substr(0, dot));
            path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
        }
        REQUIRE_MESSAGE(v != nullptr, value);
        value = std::string(v->str_or("$value"));
    }
    REQUIRE(value.size() == 7);
    REQUIRE(value[0] == '#');
    return static_cast<std::uint32_t>(std::stoul(value.substr(1), nullptr, 16)) << 8 | 0xFFu;
}

} // namespace

TEST_CASE("brand tokens: both themes have every semantic color, generated from tokens.json") {
    // The colors the editor reads (the kit's semantic tokens, tokens.css).
    constexpr std::array<std::string_view, 25> required{
        "bg", "bg-subtle", "surface", "surface-raised", "surface-inverse", "text", "text-secondary", "text-muted",
        "text-inverse", "border", "border-strong", "focus", "accent", "accent-hover", "accent-subtle", "on-accent",
        "link", "success", "success-subtle", "info", "info-subtle", "warning", "warning-subtle", "danger",
        "danger-subtle"};
    REQUIRE(brand::theme_color_count == required.size());
    for (std::size_t i = 0; i < required.size(); ++i) CHECK(brand::theme_color_names[i] == required[i]);
    // ThemeColors is exactly the listed fields, in order.
    static_assert(sizeof(brand::ThemeColors) == brand::theme_color_count * sizeof(rhi::Color));

    const std::string text = read_text(brand_dir() + "/tokens.json");
    auto root = pluma::json::Parser(text).parse();
    REQUIRE(root.has_value());
    const pluma::json::Value* semantic = root->get("semantic");
    REQUIRE(semantic != nullptr);
    for (const auto& [name, values, theme] :
         {std::tuple{"light", &brand::light_rgba, &brand::light}, std::tuple{"dark", &brand::dark_rgba, &brand::dark}}) {
        CAPTURE(name);
        const pluma::json::Value* t = semantic->get(name);
        REQUIRE(t != nullptr);
        const auto fields = brand::as_array(*theme);
        for (std::size_t i = 0; i < required.size(); ++i) {
            CAPTURE(required[i]);
            const pluma::json::Value* token = t->get(required[i]);
            REQUIRE(token != nullptr);
            const std::uint32_t expected = token_rgba(*root, std::string(token->str_or("$value")));
            CHECK((*values)[i] == expected);
            CHECK(rgba8(fields[i]) == expected);
        }
    }
}

TEST_CASE("brand tokens: the kit's named colors") {
    CHECK(rgba8(brand::palette::bone) == 0xF6F5F2FFu);
    CHECK(rgba8(brand::palette::raven) == 0x111214FFu);
    CHECK(rgba8(brand::light.bg) == 0xF6F5F2FFu); // Bone page
    CHECK(rgba8(brand::dark.bg) == 0x111214FFu);  // Raven page
    // Ember is the accent and Sage is success in both themes.
    CHECK(rgba8(brand::light.accent) == rgba8(brand::palette::ember_600));
    CHECK(rgba8(brand::dark.accent) == rgba8(brand::palette::ember_400));
    CHECK(rgba8(brand::light.success) == rgba8(brand::palette::sage_600));
    CHECK(rgba8(brand::dark.success) == rgba8(brand::palette::sage_300));
    CHECK(brand::font_display == "Outfit");
    CHECK(brand::font_body == "Figtree");
    CHECK(brand::font_mono == "JetBrains Mono");

    CHECK(brand::to_hex(brand::palette::bone) == "#F6F5F2");
    CHECK(&brand::colors(brand::Theme::Light) == &brand::light);
    CHECK(&brand::colors(brand::Theme::Dark) == &brand::dark);
    brand::Theme t = brand::Theme::Light;
    CHECK(brand::parse_theme("dark", t));
    CHECK(t == brand::Theme::Dark);
    CHECK_FALSE(brand::parse_theme("sepia", t));
}

TEST_CASE("brand: the embedded window icon is the synced 512x512 PNG") {
    const auto png = brand::app_icon_png();
    const std::string file = read_text(brand_dir() + "/png/app-icon-512.png");
    REQUIRE(png.size() == file.size());
    CHECK(std::equal(png.begin(), png.end(), file.begin(),
                     [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); }));
    constexpr std::uint8_t signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    REQUIRE(png.size() > 24);
    CHECK(std::equal(std::begin(signature), std::end(signature), png.begin()));
    auto be32 = [&](std::size_t at) {
        return std::uint32_t{png[at]} << 24 | std::uint32_t{png[at + 1]} << 16 | std::uint32_t{png[at + 2]} << 8 |
               std::uint32_t{png[at + 3]};
    };
    CHECK(be32(16) == 512); // IHDR width
    CHECK(be32(20) == 512); // IHDR height
}

TEST_CASE("brand: every synced logo SVG imports with the engine's SVG importer") {
    int files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(brand_dir() + "/logo")) {
        if (entry.path().extension() != ".svg") continue;
        ++files;
        const std::string path = entry.path().string();
        CAPTURE(path);
        const std::string text = read_text(path);
        const vector::SvgResult r = vector::parse_svg(text, path);
        REQUIRE(r.ok);
        CHECK(r.error.empty());
        if (entry.path().filename() == "favicon.svg") {
            // The browser favicon switches light/dark with a CSS <style> sheet,
            // which the importer skips (DESIGN.md 4.5); the engine never draws it.
            CHECK(r.unsupported.size() == 1);
        } else {
            CHECK(r.unsupported.empty()); // no feature skipped: what is drawn is the whole mark
        }
        CHECK(!r.document.shapes.empty());
        CHECK(r.document.size.x() > 0.0f);
        CHECK(r.document.size.y() > 0.0f);
        const auto imported = vector::import_svg(text, path);
        REQUIRE(imported.has_value());
    }
    CHECK(files >= 10);

    // The mark keeps the kit's 1.655 : 1 ratio (the splash lays the lockup out from it).
    const vector::SvgResult mark = vector::parse_svg(read_text(brand_dir() + "/logo/mark-ember.svg"));
    REQUIRE(mark.ok);
    CHECK(mark.document.size.x() / mark.document.size.y() == doctest::Approx(1.655).epsilon(0.001));
    CHECK(mark.document.shapes.size() == 2); // Raven body + Ember beak
}
