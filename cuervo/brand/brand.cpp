#include "cuervo/brand/brand.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace cuervo::brand {

namespace {

constexpr std::uint8_t app_icon_bytes[] = {
#include "cuervo/brand/app_icon.inc"
};

int channel(float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

} // namespace

const ThemeColors& colors(Theme theme) { return theme == Theme::Light ? light : dark; }

std::string_view theme_name(Theme theme) { return theme == Theme::Light ? "light" : "dark"; }

bool parse_theme(std::string_view text, Theme& out) {
    if (text == "light") {
        out = Theme::Light;
    } else if (text == "dark") {
        out = Theme::Dark;
    } else {
        return false;
    }
    return true;
}

std::string to_hex(rhi::Color color) {
    return std::format("#{:02X}{:02X}{:02X}", channel(color.r), channel(color.g), channel(color.b));
}

std::span<const std::uint8_t> app_icon_png() { return app_icon_bytes; }

} // namespace cuervo::brand
