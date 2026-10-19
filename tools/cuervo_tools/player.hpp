#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cuervo/assets/pack.hpp"
#include "cuervo/script/features.hpp"

// Per-export players (DESIGN.md 4.11 "Clarified (Milestone 7.5)", docs/EXPORT.md
// "Stripped players"): `cuervo-tools export` configures and builds the engine
// with exactly the features the project needs (cuervo/script/features.hpp)
// and ships that player instead of the prebuilt full one. Builds live in a
// cache keyed by the feature set and the toolchain, so the second export of a
// project (or of any project with the same features) only re-runs an
// incremental build.
//
// The pure parts (feature set -> CMake definitions, cache key) are separate
// functions so they can be unit-tested without a compiler.
namespace cuervo::tools {

// `--engine auto|full|stripped`.
enum class EngineChoice {
    Auto,     // stripped when something can be stripped and a toolchain exists
    Full,     // the prebuilt player (the build tree's `cuervo`)
    Stripped, // always build a per-export player; fail if it can't
};
std::optional<EngineChoice> parse_engine_choice(std::string_view text);
std::string_view engine_choice_name(EngineChoice choice);

// The host toolchain that builds per-export players. cuervo-tools bakes in
// the values of its own configure (tools/cuervo_tools/CMakeLists.txt), so a
// player is built from the same sources with the same compiler.
struct PlayerToolchain {
    std::filesystem::path source_dir; // engine sources (top-level CMakeLists.txt)
    std::filesystem::path cmake;      // the cmake executable
    std::string generator;            // e.g. "Ninja"; empty = cmake's default
    std::filesystem::path make_program;
    std::filesystem::path c_compiler;
    std::filesystem::path cxx_compiler;
    // A configured build tree's `_deps/`: its `<name>-src` checkouts are
    // handed to FetchContent (FETCHCONTENT_SOURCE_DIR_<NAME>), so a player
    // build reuses the pinned sources instead of downloading them again.
    std::filesystem::path deps_dir;
    std::string engine_version;
    // Web players: the emsdk checkout (its Emscripten toolchain file and
    // tools) and a native host-tools build (shader_tool, the font importer;
    // docs/WEB.md "Building").
    std::filesystem::path emsdk;
    std::filesystem::path host_build;

    // Empty when the toolchain can build a player for `platform`, else why not.
    std::string missing(assets::PackPlatform platform = assets::PackPlatform::Linux) const;
};

// The environment a web player's configure and build run with: EMSDK,
// EM_CONFIG and the emsdk tools first on PATH (what `emsdk_env.sh` sets).
std::vector<std::pair<std::string, std::string>> web_build_environment(const std::filesystem::path& emsdk);

// The `-D` definitions that configure a player with exactly `features`,
// sorted by variable name: Release, `runtime` only (no tests, no editor),
// one CUERVO_FEATURE_* per subsystem, CUERVO_VULKAN off for projects the GL
// backend runs (compatible tier), and the runtime importers (SVG, TTF/OTF)
// off because a pack carries imported assets (DESIGN.md 4.11).
// Web players also set CUERVO_WEB_PACK=fetch (a runtime that loads the game.pak
// next to its page) and never have Vulkan.
std::vector<std::string> player_cmake_definitions(const script::FeatureSet& features,
                                                  assets::PackPlatform platform = assets::PackPlatform::Linux);

// Whether a per-export player drops anything the prebuilt full player has.
// The prebuilt player is configured with every option at its default (all
// subsystems, Vulkan, dev tools, importers), so this is true unless a project
// asks for all of them.
bool can_strip(const script::FeatureSet& features);

// Empty when the CMake option dependencies hold (top-level CMakeLists.txt:
// particles need collision, dev tools need particles), else the reason.
std::string check_feature_dependencies(const script::FeatureSet& features);

// The cache directory name: `<platform>_<features>_<hash>`, e.g.
// `linux_collision+particles_1f0c...`. The hash covers the definitions, the
// platform, the engine version and sources, the generator and the compilers,
// so it is stable for equal inputs (in any order of construction) and changes
// when any of them does.
std::string player_cache_key(const script::FeatureSet& features, const PlayerToolchain& toolchain,
                             assets::PackPlatform platform);

// The configure command line (argv) for `build_dir`.
std::vector<std::string> player_configure_command(const script::FeatureSet& features,
                                                  const PlayerToolchain& toolchain,
                                                  const std::filesystem::path& build_dir,
                                                  assets::PackPlatform platform = assets::PackPlatform::Linux);

// `$CUERVO_PLAYER_CACHE`, else `$XDG_CACHE_HOME/cuervo/players`, else
// `~/.cache/cuervo/players`.
std::filesystem::path default_player_cache_dir();

using Progress = std::function<void(std::string_view)>;

struct PlayerBuild {
    std::filesystem::path executable;
    std::filesystem::path build_dir;
    std::filesystem::path log;
    std::string key;
    bool configured = false; // false: an existing cache entry was reused
};

// Configures (first time only) and builds the `runtime` target into
// `<cache_root>/<key>`; the configure/build output goes to
// `<cache_root>/<key>/player-build.log`. Linux only for now.
std::optional<PlayerBuild> build_player(const script::FeatureSet& features, const PlayerToolchain& toolchain,
                                        const std::filesystem::path& cache_root, assets::PackPlatform platform,
                                        const Progress& progress, std::string& error);

// Runs a program and waits for it. stdout and stderr go to `log`; the
// result's `output` is that log's content.
struct ProcessResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string output;
};
std::optional<ProcessResult> run_process(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
                                         const std::vector<std::pair<std::string, std::string>>& env,
                                         const std::filesystem::path& log, std::chrono::seconds timeout,
                                         std::string& error);

} // namespace cuervo::tools
