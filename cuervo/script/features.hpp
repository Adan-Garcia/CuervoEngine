#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cuervo::assets {
class Vfs;
}

namespace cuervo::script {

struct Project;

// The resolved feature set for an exported game or engine build.
// (Milestone 7.5, DESIGN.md 6).
struct FeatureSet {
    bool audio = true;
    bool ui = true;
    bool particles = true;
    bool collision = true;
    bool physics = true;
    bool vulkan = true;
    bool svg_import = false;
    bool font_import = false;
    bool dev = false;

    bool enabled(std::string_view name) const;
    void set(std::string_view name, bool value);

    // List of active feature names, sorted.
    std::vector<std::string> active_list() const;
    // List of stripped feature names, sorted.
    std::vector<std::string> stripped_list() const;
};

// Explicit overrides in project.toml [features]
struct FeatureOverrides {
    std::optional<bool> audio;
    std::optional<bool> ui;
    std::optional<bool> particles;
    std::optional<bool> collision;
    std::optional<bool> physics;
    std::optional<bool> vulkan;
    std::optional<bool> svg_import;
    std::optional<bool> font_import;
    std::optional<bool> dev;

    bool empty() const;
};

// Detects the features needed by a project from its assets, scene files, scripts, and project configuration.
// Overrides from project.toml take precedence.
FeatureSet detect_features(const assets::Vfs& vfs, const Project& project, const FeatureOverrides& overrides = {});

} // namespace cuervo::script
