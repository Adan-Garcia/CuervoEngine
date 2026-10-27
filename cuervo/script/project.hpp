#pragma once

#include <compare>
#include <cstdint>
#include <filesystem>
#include <memory>
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
//   [dependencies]                # packages (LANGUAGE.md 9.26, milestone 10.5b)
//   tween = "1.2.0"               # vendored in packages/tween/
//   [dependencies.fx]             # or a package in another directory
//   path = "../libs/fx"
//   version = "0.3.1"
namespace cuervo::assets {
class Vfs;
}

namespace cuervo::script {

// A package dependency (project.toml or package.toml `[dependencies]`). The
// version is pinned: the package's own `version` must equal it.
struct Dependency {
    std::string name;
    std::string version;
    std::string path; // a directory relative to the declaring manifest; empty: vendored
    int line = 0;     // in the declaring manifest
};

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
    std::vector<Dependency> dependencies; // [dependencies], in file order
};

// A semantic version, MAJOR.MINOR.PATCH.
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    auto operator<=>(const Version&) const = default;
};
// "1.2.3"; with `partial`, also "1" and "1.2" (missing parts are 0).
std::optional<Version> parse_version(std::string_view text, bool partial = false);

// A version range (milestone 10.5d, docs/PACKAGES.md "Version ranges"):
// comma-separated clauses, all of which must hold. A clause is `>=V`, `>V`,
// `<=V`, `<V`, `=V` or a bare `V` (exactly V), `^V` (compatible: the same
// major version, or the same minor while the major is 0) or `*` (any).
// Versions in a range may be partial ("1", "0.2").
struct VersionRange {
    enum class Op : std::uint8_t { Eq, Ge, Gt, Le, Lt, Caret };
    struct Clause {
        Op op = Op::Eq;
        Version v;
    };
    std::vector<Clause> clauses; // empty: any version
    std::string text = "*";      // as written ("*" when empty)
    bool contains(const Version& v) const;
};
std::optional<VersionRange> parse_version_range(std::string_view text, std::string& error);

// This engine's version (the CMake project version) and the plugin ABI it hosts.
std::string_view engine_version();
int plugin_abi_version();

// A package's `package.toml` (docs/PACKAGES.md):
//
//   name = "tween"         # a Pluma identifier: the namespace scripts import
//   version = "1.2.0"      # MAJOR.MINOR.PATCH
//   description = "Easing curves and tweens"
//   license = "MIT"        # an SPDX expression (cuervo-tools plugin verify checks it)
//   engine = ">=0.1, <0.2" # engine versions it works with (default: any)
//   [dependencies]         # as in project.toml
//   [plugin]               # native code (milestone 10.5c, docs/PLUGINS.md)
//   abi = 1                # the plugin ABI it was built for, or a range: ">=1, <3"
//   sim_safe = true        # deterministic: may run in the simulation
//   platforms = ["linux", "windows"]    # where it runs (default: from the libraries)
//   linux = "lib/libnoise.so"           # shared library per platform
//   linux_static = "lib/libnoise.a"     # linked into AOT exports (optional)
//   windows = "lib/noise.dll"
//   windows_static = "lib/noise.lib"         # AOT exports from MSVC-runtime builds
//   windows_gnu_static = "lib/libnoise_gnu.a" # ... and from MinGW-w64 builds
//   dotnet = "bin/Noise.dll"            # or a .NET assembly (C#, desktop VM only)
//   dotnet_type = "Noise.Plugin, Noise" # its entry type (default: <assembly>.Plugin)
struct PluginSpec {
    bool present = false;
    bool has_abi = false;
    VersionRange abi; // over ABI versions ("1" is 1.0.0)
    bool sim_safe = false;
    // Package-relative paths. (Not `linux`: a predefined macro in GNU modes.)
    // A static library only links with its own toolchain's C/C++ runtime, so
    // Windows has one per ABI (MSVC, MinGW-w64); the DLL is plain C ABI.
    std::string lib_linux, static_linux, lib_windows, static_windows, static_windows_gnu, dotnet, dotnet_type;
    // `platforms`, or the platforms the library fields name: "linux",
    // "windows" (a .NET plugin: both).
    std::vector<std::string> platforms;
    bool abi_compatible(int abi_version) const { return abi.contains(Version{abi_version, 0, 0}); }
};

struct PackageManifest {
    std::string name;
    std::string version;
    std::string description;
    std::string license;
    VersionRange engine; // `engine`: the engine versions it works with
    std::vector<Dependency> dependencies;
    PluginSpec plugin;
};

// A package found for a project, with the VFS and path prefix its files are
// read through ("packages/tween/" inside the project or its pack, "" for a
// directory of its own).
struct ResolvedPackage {
    PackageManifest manifest;
    std::shared_ptr<const assets::Vfs> vfs;
    std::string prefix;
    std::filesystem::path dir; // on disk; empty inside a pack
};

bool valid_package_name(std::string_view name);
bool valid_version(std::string_view version);

// Reads `<prefix>package.toml` through `vfs`.
std::optional<PackageManifest> load_package_manifest(const assets::Vfs& vfs, std::string_view prefix,
                                                     std::string& error);

// Every package the project depends on, directly or not, dependencies
// before their dependents (the order their globals initialize in). In a
// pack every package is under `packages/<name>/`; in development a
// dependency with a `path` is that directory, others are vendored under the
// project's `packages/<name>/`. A version mismatch, two versions of one
// package or a cycle is an error.
bool resolve_packages(const Project& project, const assets::Vfs& vfs, std::vector<ResolvedPackage>& out,
                      std::string& error);

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
// The project's packages come first, each file tagged with its package and
// named `packages/<name>/scripts/...` (LANGUAGE.md 9.26).
bool read_scripts(const assets::Vfs& vfs, std::vector<pluma::SourceFile::Part>& out,
                  std::vector<std::filesystem::path>* files, std::string& error);

} // namespace cuervo::script
