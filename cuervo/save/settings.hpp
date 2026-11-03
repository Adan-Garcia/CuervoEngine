#pragma once

// Player settings (DESIGN.md 7.2 "Save data"): graphics, audio and input
// choices kept in the reserved save slot "settings" (version 1). They never
// enter the simulation: a Settings value is applied to the engine through a
// SettingsTarget, and the sim only sees what the target changes in the
// presentation layers (window, mixer, input map bindings).

#include <array>
#include <map>
#include <string>

#include "cuervo/save/save_store.hpp"

namespace cuervo::save {

inline constexpr const char* settings_slot = "settings";
inline constexpr std::size_t settings_bus_count = 5; // master, music, sfx, ui, voice (audio::Bus order)
inline constexpr std::array<const char*, settings_bus_count> settings_bus_names = {"master", "music", "sfx", "ui",
                                                                                    "voice"};

struct Settings {
    // graphics
    bool fullscreen = false;
    bool vsync = true;
    float ui_scale = 1.0f; // 0.5 - 3
    // audio: linear 0..1 per bus
    std::array<float, settings_bus_count> volume = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    bool muted = false; // master mute
    // input: action name -> replacement binding text (opaque to this layer)
    std::map<std::string, std::string> bindings;

    friend bool operator==(const Settings&, const Settings&) = default;
};

SlotData to_slot(const Settings& s);
// Missing or out-of-range fields fall back to defaults / are clamped.
Settings from_slot(const SlotData& data);

// Declares the settings slot on `store` (call once; harmless to repeat).
void declare_settings(SaveStore& store);
bool save_settings(SaveStore& store, const Settings& s, std::string* error = nullptr);
// Defaults when nothing is saved or the file is unreadable (`error` says why).
Settings load_settings(SaveStore& store, std::string* error = nullptr);

// What applying settings changes. Every hook may be absent: a headless run or
// a test target simply ignores the parts it does not have.
class SettingsTarget {
public:
    virtual ~SettingsTarget() = default;
    virtual void set_fullscreen(bool) {}
    virtual void set_vsync(bool) {}
    virtual void set_ui_scale(float) {}
    virtual void set_bus_volume(std::size_t /*bus, audio::Bus order*/, float /*volume*/) {}
    virtual void set_muted(bool) {}
    // Returns false when the action or binding text is unknown.
    virtual bool set_binding(const std::string& /*action*/, const std::string& /*binding*/) { return true; }
};

// Pushes every setting into `target`; returns how many bindings it refused.
int apply_settings(const Settings& s, SettingsTarget& target);

} // namespace cuervo::save
