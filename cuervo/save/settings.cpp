#include "cuervo/save/settings.hpp"

#include <algorithm>
#include <string>

namespace cuervo::save {

namespace {
constexpr std::string_view binding_prefix = "input.";
float clamp01(double v) { return static_cast<float>(std::clamp(v, 0.0, 1.0)); }
} // namespace

SlotData to_slot(const Settings& s) {
    SlotData d;
    d.set("graphics.fullscreen", s.fullscreen);
    d.set("graphics.vsync", s.vsync);
    d.set("graphics.ui_scale", static_cast<double>(s.ui_scale));
    for (std::size_t i = 0; i < settings_bus_count; ++i) {
        d.set(std::string("audio.") + settings_bus_names[i], static_cast<double>(s.volume[i]));
    }
    d.set("audio.muted", s.muted);
    for (const auto& [action, binding] : s.bindings) d.set(std::string(binding_prefix) + action, binding);
    return d;
}

Settings from_slot(const SlotData& d) {
    Settings s;
    s.fullscreen = d.get_bool("graphics.fullscreen", s.fullscreen);
    s.vsync = d.get_bool("graphics.vsync", s.vsync);
    s.ui_scale = static_cast<float>(std::clamp(d.get_float("graphics.ui_scale", s.ui_scale), 0.5, 3.0));
    for (std::size_t i = 0; i < settings_bus_count; ++i) {
        s.volume[i] = clamp01(d.get_float(std::string("audio.") + settings_bus_names[i], s.volume[i]));
    }
    s.muted = d.get_bool("audio.muted", s.muted);
    for (const auto& [key, value] : d.fields) {
        if (!key.starts_with(binding_prefix) || !std::holds_alternative<std::string>(value)) continue;
        s.bindings[key.substr(binding_prefix.size())] = std::get<std::string>(value);
    }
    return s;
}

void declare_settings(SaveStore& store) { store.declare(settings_slot, 1); }

bool save_settings(SaveStore& store, const Settings& s, std::string* error) {
    declare_settings(store);
    return store.save(settings_slot, to_slot(s), error);
}

Settings load_settings(SaveStore& store, std::string* error) {
    declare_settings(store);
    std::string why;
    if (const auto data = store.load(settings_slot, why)) return from_slot(*data);
    if (error != nullptr) *error = why;
    return Settings{};
}

int apply_settings(const Settings& s, SettingsTarget& target) {
    target.set_fullscreen(s.fullscreen);
    target.set_vsync(s.vsync);
    target.set_ui_scale(s.ui_scale);
    for (std::size_t i = 0; i < settings_bus_count; ++i) target.set_bus_volume(i, s.volume[i]);
    target.set_muted(s.muted);
    int refused = 0;
    for (const auto& [action, binding] : s.bindings) {
        if (!target.set_binding(action, binding)) ++refused;
    }
    return refused;
}

} // namespace cuervo::save
