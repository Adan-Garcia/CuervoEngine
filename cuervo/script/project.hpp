#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cuervo/input/input_map.hpp"
#include "cuervo/math/vec.hpp"
#include "cuervo/rhi/rhi.hpp"
#include "cuervo/script/features.hpp"
#include "pluma/source.hpp"

// A script project (DESIGN.md 7.2 "Project layout"): `project.toml` plus
// `scripts/**/*.plm`. project.toml is read with a small TOML subset:
// `key = value` lines (strings, integers, floats, booleans, one-line arrays),
// `[section]` headers and `#` comments.
//
//   name = "Shmup"
//   main = "Game"                 # the root node's class
//   main_scene = "scenes/level.scene"  # or: the root is this scene file
//   virtual_size = [1280, 720]
//   tick_rate = 60
//   seed = 12345
//   clear_color = "0b0e1a"
//   render_tier = "compatible"   # or "full" (DESIGN.md 4.4); default compatible
//   jobs = 4                      # job system threads (DESIGN.md 4.15); 0/absent = auto
//   [scene]
//   transform_hierarchy = true    # false: Node2D transforms are world transforms
//   [features]                    # optional explicit overrides (Milestone 7.5)
//   audio = false
//   particles = true
//   [input.actions]               # DESIGN.md 4.8
//   shoot = ["Key.Z", "Key.Space", "Pad.South"]
//   [input.axes]                  # 2D axes: Input.axis(#move) -> vec2
//   move = ["Keys.Arrows", "Keys.WASD", "Pad.LeftStick"]
namespace cuervo::assets {
class Vfs;
}

namespace cuervo::script {

struct Project {
    std::filesystem::path dir;
    std::string name = "Game";
    std::string main_class = "Main";
    // Optional: the root is this scene file (project-relative) instead of an
    // instance of `main_class`; levels built in the editor run without a
    // main script (DESIGN.md 4.13, "Clarified (Phase 6A)").
    std::string main_scene;
    ivec2 virtual_size{1280, 720};
    float tick_rate = 60.0f;
    std::uint64_t seed = 1;
    rhi::Color clear_color{0.05f, 0.06f, 0.10f, 1.0f};
    rhi::Tier render_tier = rhi::Tier::Compatible;
    int render_tier_line = 0; // line of `render_tier` in project.toml (0 = not set)
    unsigned jobs = 0;        // job system threads; 0 = auto (--jobs and CUERVO_JOBS override)
    // [scene] transform_hierarchy: Node2D transforms are parent-relative
    // (DESIGN.md 4.17, milestone 9.1). Defaults to true; false keeps the
    // world-space behavior of earlier versions.
    bool transform_hierarchy = true;
    FeatureOverrides features; // [features] in project.toml
    std::vector<std::pair<std::string, std::vector<std::string>>> actions;
    std::vector<std::pair<std::string, std::vector<std::string>>> axes;
};

// Reads `<dir>/project.toml`. On failure returns nullopt and sets `error`.
std::optional<Project> load_project(const std::filesystem::path& dir, std::string& error);
// The same through a VFS (a project directory in development, the game's pack
// in release; cuervo/assets/vfs.hpp). `dir` is the VFS's directory mount, or
// empty for a pack.
std::optional<Project> load_project(const assets::Vfs& vfs, std::string& error);

// Registers the project's actions and axes. A 2D axis `move` becomes the
// InputMap axes `move.x` and `move.y` (y down, DESIGN.md 7.1).
bool apply_input(const Project& project, InputMap& map, std::string& error);

// Binding spellings in project.toml: `Key.Z`, `Pad.South`, `Pad.LeftStickUp`
// (a stick or trigger as a button), `Mouse.Left`, `Wheel.Up`. binding_name
// is the inverse (empty for a code with no spelling).
std::optional<Binding> parse_binding(std::string_view text);
std::string binding_name(Binding binding);
// 2D axis sources ([input.axes]): `Keys.Arrows`, `Keys.WASD`,
// `Pad.LeftStick`, `Pad.RightStick`, `Pad.Dpad`.
bool parse_axis2(std::string_view text, AxisSource& x, AxisSource& y);
std::span<const std::string_view> axis2_source_names();

// Every `.plm` file under `<dir>/scripts`, sorted by path (deterministic
// unit order). `files` receives the paths.
bool read_scripts(const Project& project, std::vector<pluma::SourceFile::Part>& out,
                  std::vector<std::filesystem::path>* files, std::string& error);
// Every `scripts/**/*.plm` through a VFS; `files` receives the real paths of
// those that have one (directory mounts: watched for hot reload). Having
// no scripts is not an error (a main_scene of prelude nodes needs none).
bool read_scripts(const assets::Vfs& vfs, std::vector<pluma::SourceFile::Part>& out,
                  std::vector<std::filesystem::path>* files, std::string& error);

} // namespace cuervo::script
