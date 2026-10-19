#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "cuervo/assets/manifest.hpp"
#include "cuervo/assets/pack.hpp"
#include "cuervo/script/features.hpp"
#include "tools/cuervo_tools/player.hpp"

// The release pipeline behind `cuervo-tools pack` and `cuervo-tools export`
// (DESIGN.md 4.11, docs/EXPORT.md). Kept out of the engine library: it runs
// on the developer's machine and pulls in the importers and the tier check.
//
// Pipeline (each step a function, so Phase 7B can add "compile the scripts
// to C and link them into the runtime" as a script stage without touching
// the others):
//   1. check     project.toml loads; the tier check passes for the target
//                (`cuervo-tools tier check`; the web needs "compatible")
//   2. collect   every project file except hidden ones (.import/, .git...),
//                and with --strip the development-only ones (replays, editor
//                command scripts)
//   3. import    sources become engine formats the target can load:
//                TTF/OTF -> .efont, SVG -> .evec; sounds ship as they are
//                (miniaudio decodes WAV/OGG/FLAC/MP3 on every target)
//   4. scripts   ScriptForm::Source today: the .plm files (no bytecode
//                serialization exists in pluma/ yet); 7B: Native
//   5. engine    .cuervo/default.efont (the game font) + .cuervo/export.toml
//   6. pack      a deterministic archive (cuervo/assets/pack.hpp)
//   7. assemble  (export only) runtime + game.pak into the output directory;
//                the runtime is a per-export player built with exactly the
//                project's features (player.hpp, milestone 7.5) or the
//                prebuilt full one, and a built player is run on the pack
//                before it ships
namespace cuervo::tools {

struct PackOptions {
    assets::PackPlatform platform = assets::host_platform();
    assets::ScriptForm scripts = assets::ScriptForm::Source;
    // Leave development-only files out (docs/EXPORT.md "Strip").
    bool strip = false;
    // The game font (TTF/OTF imported at export, or an .efont as is).
    std::string font_path;
    // Run the tier check gate (step 1); off only for tests of the format.
    bool tier_gate = true;
    std::string engine_version;
};

struct PackSummary {
    std::string game;            // project.toml name
    std::size_t files = 0;       // entries in the pack
    std::size_t imported = 0;    // converted by an importer
    std::size_t skipped = 0;     // hidden or stripped
    std::uint64_t bytes = 0;     // pack size
    std::uint64_t content_hash = 0;
    // What the game needs (detect_features + project.toml [features]); the
    // manifest records it, and export builds a player from it.
    script::FeatureSet features;
    std::vector<std::string> notes; // human-readable log of what happened
};

// Steps 1-6: the project's pack in memory. False + `error` on any failure
// (tier errors are listed in `error`).
std::optional<std::vector<std::byte>> pack_project(const std::filesystem::path& project, const PackOptions& options,
                                                   PackSummary& summary, std::string& error);

struct ExportOptions {
    PackOptions pack;
    std::filesystem::path out_dir;
    // An explicit player to ship, which wins over `engine`: Linux/Windows an
    // executable; web the directory with a pack-loading web runtime build
    // (index.html/.js/.wasm).
    std::filesystem::path runtime;
    // The launcher's name (default: the project name, lowercased, safe).
    std::string name;

    // Which player ships when `runtime` is empty (docs/EXPORT.md "Stripped
    // players"). The library default keeps the prebuilt player; the CLI
    // defaults to Auto.
    EngineChoice engine = EngineChoice::Full;
    // The prebuilt full player (`engine` Full, or Auto when nothing can be
    // stripped or no toolchain is available); same forms as `runtime`.
    std::filesystem::path full_runtime;
    // How to build stripped players, and where they are cached.
    PlayerToolchain toolchain;
    std::filesystem::path player_cache;
    // Run a freshly built player on the exported pack before shipping it
    // (off for cross builds, whose player can't run here).
    bool verify = true;
    // The engine's source root, holding THIRD_PARTY_NOTICES.md and
    // third_party/licenses/: copied into the output as `licenses/`, since
    // the player's third-party licenses (zlib, MIT, ...) ask for their notices
    // to travel with it (LICENSE.md 11.1). Empty: no notices are copied.
    std::filesystem::path notices_root;
    Progress progress;
};

// Copies `root`/THIRD_PARTY_NOTICES.md and `root`/third_party/licenses/* into
// `out_dir`/licenses/, adding the files to `written`.
bool copy_third_party_notices(const std::filesystem::path& root, const std::filesystem::path& out_dir,
                              std::vector<std::filesystem::path>& written, std::string& error);

// The player that shipped.
struct ShippedPlayer {
    enum class Kind { Explicit, Full, Stripped };
    Kind kind = Kind::Full;
    std::filesystem::path source;            // the file copied to the launcher
    std::uint64_t bytes = 0;                 // its size (web: the .wasm)
    std::optional<std::uint64_t> full_bytes; // the prebuilt full player's, when it exists
    std::string cache_key;                   // Stripped: the cache entry
    bool configured = false;                 // Stripped: a new cache entry was configured
    bool verified = false;                   // ran the pack headless (and matched the reference)
    std::optional<std::uint64_t> state_hash; // the verification run's hash at verify_tick
    std::optional<std::uint64_t> reference_hash; // what it was compared with
    std::string reference;                       // "full engine", "dev run" or empty (none)
};
std::string_view player_kind_name(ShippedPlayer::Kind kind);

struct ExportSummary {
    PackSummary pack;
    ShippedPlayer player;
    std::vector<std::filesystem::path> outputs; // files written
    std::filesystem::path launcher;             // what to run
};

// The tick the verification compares at, as `--screenshot` does by default
// (runtime/game_cli.hpp, tests/export_shmup.cmake).
inline constexpr std::uint64_t verify_tick = 240;

// Step 7's check of a built player. Runs `launcher` headless (GL, offscreen,
// no audio, `--screenshot` at verify_tick, a private user directory) with
// `args` (none: the game.pak next to it) and returns the state hash it logs.
// A run that fails, logs an error or a panic (e.g. a script naming a feature
// the player was built without), or (`from_pack`) doesn't run from a pack is
// an error.
std::optional<std::uint64_t> run_player_headless(const std::filesystem::path& launcher,
                                                 const std::vector<std::string>& args, bool from_pack,
                                                 std::string& error);

// Whether a player with `features` has the same script surface (the
// optional preludes and natives of audio/ui/particles/collision) as this
// process's engine. Two things depend on it:
//   - the state hash: ScriptHost::state_hash covers every numeric script
//     global, the optional preludes' among them, and the particle world, so
//     a player without one of these features hashes a different (equally
//     deterministic) set and is checked against its own dev run instead;
//   - AOT scripts (--aot): emit_aot_c compiles the prelude of *this* engine
//     into .cuervo/scripts.so, so a native pack only matches a player with
//     the same surface (Vulkan, dev tools and importers can still go).
bool script_surface_matches(const script::FeatureSet& features);

// The state hash of `pack` after verify_tick ticks without input, in this
// process (the reference for verify_player); nullopt + `error` when this
// engine can't run it.
std::optional<std::uint64_t> reference_state_hash(const std::filesystem::path& pack, std::string& error);

bool export_project(const std::filesystem::path& project, const ExportOptions& options, ExportSummary& summary,
                    std::string& error);

// "Shmup Deluxe!" -> "shmup-deluxe".
std::string launcher_name(std::string_view game);

} // namespace cuervo::tools
