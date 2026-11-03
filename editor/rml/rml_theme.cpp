#include "editor/rml/rml_theme.hpp"

#include <cmath>
#include <format>

namespace cuervo::editor::rml_theme {

namespace {

constexpr rhi::Color hex(unsigned v) {
    return {static_cast<float>((v >> 16) & 0xff) / 255.0f, static_cast<float>((v >> 8) & 0xff) / 255.0f, static_cast<float>(v & 0xff) / 255.0f, 1.0f};
}

std::string css(rhi::Color c) {
    return std::format("rgba({}, {}, {}, {})", static_cast<int>(std::lround(c.r * 255.0f)), static_cast<int>(std::lround(c.g * 255.0f)),
                       static_cast<int>(std::lround(c.b * 255.0f)), static_cast<int>(std::lround(c.a * 255.0f)));
}

} // namespace

Surfaces surfaces(brand::Theme theme) {
    if (theme == brand::Theme::Light) return {hex(0xe6e6e9), hex(0xf5f5f6), hex(0xffffff), hex(0xffffff), hex(0xd8d8dd), hex(0xbfbfc7), hex(0xffffff), hex(0xdfe5f2)};
    return {hex(0x141415), hex(0x1c1c1e), hex(0x262628), hex(0x2a2a2d), hex(0x2f2f33), hex(0x3d3d42), hex(0x161617), hex(0x0b1020)};
}

std::string export_tokens(brand::Theme theme) {
    const brand::ThemeColors& t = brand::colors(theme);
    const Surfaces s = surfaces(theme);
    const bool dark = theme == brand::Theme::Dark;
    std::string out = "body {\n";
    const auto add = [&](const char* name, rhi::Color c) { out += std::format("  --{}: {};\n", name, css(c)); };
    add("page", s.page);
    add("layer", s.layer);
    add("card", s.card);
    add("overlay", s.overlay);
    add("edge", s.edge);
    add("border", s.border);
    add("field", s.field);
    add("scene", s.scene);
    add("field-hover", brand::mix(s.field, t.text, 0.10f));
    add("hover", brand::mix(s.layer, t.text, dark ? 0.08f : 0.07f));
    add("press", brand::mix(s.layer, t.text, dark ? 0.14f : 0.13f));
    add("text", t.text);
    add("text-2", t.text_secondary);
    add("muted", t.text_muted);
    add("border-strong", t.border_strong);
    add("accent", t.accent);
    add("accent-hover", t.accent_hover);
    add("accent-subtle", t.accent_subtle);
    add("on-accent", t.on_accent);
    add("focus", t.focus);
    add("danger", t.danger);
    add("warning", t.warning);
    add("info", t.info);
    add("success", t.success);
    // Pluma syntax colors (script editor), from the mockup palette.
    add("syn-kw", hex(dark ? 0xee7b4d : 0xb0430f));
    add("syn-ty", hex(dark ? 0x7fb3d5 : 0x1f6a99));
    add("syn-str", hex(dark ? 0x9fcf8a : 0x2d7d46));
    add("syn-num", hex(dark ? 0xd9b36c : 0x8f6400));
    add("syn-cm", t.text_muted);
    out += "}\n";
    return out;
}

} // namespace cuervo::editor::rml_theme
