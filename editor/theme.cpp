#include "editor/theme.hpp"

#include <SDL3/SDL.h>

#include <algorithm>

namespace cuervo::editor::theme {

namespace {

brand::Theme current_theme = brand::Theme::Dark;
float current_ui_scale = 1.0f;
float current_font_scale = 1.0f;
void (*restyle_hook)() = nullptr;

void restyle() {
    if (restyle_hook != nullptr) restyle_hook();
}

using brand::mix;

} // namespace

void set_restyle_hook(void (*hook)()) { restyle_hook = hook; }

void set_current(brand::Theme theme) {
    current_theme = theme;
}

float ui_scale() { return current_ui_scale; }

void set_ui_scale(float scale) {
    current_ui_scale = std::clamp(scale, 0.5f, 3.0f);
    restyle();
}

float font_scale() { return current_font_scale; }

void set_font_scale(float scale) {
    current_font_scale = std::clamp(scale, 0.5f, 3.0f);
    restyle();
}

brand::Theme current() { return current_theme; }

const brand::ThemeColors& colors() { return brand::colors(current_theme); }

brand::Theme system_theme() {
    return SDL_GetSystemTheme() == SDL_SYSTEM_THEME_LIGHT ? brand::Theme::Light : brand::Theme::Dark;
}

rhi::Color control_fill() { return colors().surface; }
rhi::Color control_hover() { return mix(colors().surface, colors().text, 0.07f); }
rhi::Color control_active() { return mix(colors().surface, colors().text, 0.14f); }

rhi::Color severity_color(Severity s) {
    const brand::ThemeColors& t = colors();
    switch (s) {
        case Severity::Success: return t.success;
        case Severity::Warning: return t.warning;
        case Severity::Danger: return t.danger;
        case Severity::Info: break;
    }
    return t.info;
}

rhi::Color severity_subtle(Severity s) {
    const brand::ThemeColors& t = colors();
    switch (s) {
        case Severity::Success: return t.success_subtle;
        case Severity::Warning: return t.warning_subtle;
        case Severity::Danger: return t.danger_subtle;
        case Severity::Info: break;
    }
    return t.info_subtle;
}

} // namespace cuervo::editor::theme
