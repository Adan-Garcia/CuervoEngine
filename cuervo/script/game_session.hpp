#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cuervo/assets/vfs.hpp"
#if CUERVO_FEATURE_AUDIO
#include "cuervo/audio/audio_system.hpp"
#endif
#include "cuervo/input/input_map.hpp"
#include "cuervo/input/replay.hpp"
#include "cuervo/platform/file_watcher.hpp"
#include "cuervo/render/font.hpp"
#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"

// One running game without a window: the simulation half of `cuervo --game`
// (game_runner.cpp), shared with the editor's play-in-editor and the replay
// tools so that all of them run a project through exactly the same code
// (DESIGN.md 4.13, "Clarified (Phase 6B)"). It loads project.toml, the input
// map, the scripts, the sounds and the font UI layout measures with, builds
// the ScriptHost (main_scene / main class, or a given scene), and then:
//   - sample(): the tick's InputFrame from devices + pointer + --press presses
//   - tick(): one fixed tick, recorded into a Replay when recording
//   - reload(): hot reload / restart (DESIGN.md 5.13)
// Rendering and the frame loop stay with the caller.
namespace cuervo::script {

struct SessionOptions {
    // Development: the project directory. Release: `pack`, the exported
    // game's archive (docs/EXPORT.md); `dir` is then ignored. Every game file
    // is read through the resulting VFS (cuervo/assets/vfs.hpp).
    std::filesystem::path dir;
    std::filesystem::path pack;
    // Release: where saves go; empty = platform::user_data_dir(<game name>).
    std::filesystem::path user_dir;
    // Job system threads: configured process-wide unless `configure_jobs` is
    // off; 0 = CUERVO_JOBS, project.toml `jobs`, auto (jobs::resolve_thread_count).
    unsigned jobs = 0;
    bool configure_jobs = true;
    // The game font (TTF/OTF or .efont); UI.measure lays text out with it.
    // Empty: the pack's `.cuervo/default.efont` when running a pack, else
    // none (a fixed estimate; layouts then differ from a run with one).
    std::string font_path;
    // Play sound on a device; otherwise the null backend (same triggers).
    bool audio_device = false;
    // Instead of the project's main: the root is this scene (the editor
    // plays the scene being edited, unsaved changes included). `scene_label`
    // names it in messages and replays.
    std::shared_ptr<const scene::SceneDoc> scene;
    std::string scene_label;
    // Overrides project.toml `seed` (replays carry their own).
    std::optional<std::uint64_t> seed;
    // A debugger or profiler for the script VMs (HostOptions::debug_hooks).
    pluma::DebugHooks* debug_hooks = nullptr;
    std::function<void(pluma::VM&)> on_active_vm;
    // Script profiling from the first line run, restarts included
    // (ScriptHost::set_script_profiling).
    bool profile_scripts = false;
};

class GameSession {
public:
    GameSession();
    ~GameSession();
    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;

    // Loads and starts the game. False + `error` if the project can't be
    // read or its scripts don't compile (the diagnostics are logged too); a
    // panic while starting is not a failure: host().panicked() tells.
    bool init(const SessionOptions& options, std::string& error);

    const Project& project() const { return *project_; }
    const InputMap& input_map() const { return input_map_; }
    ScriptHost& host() { return *host_; }
    const ScriptHost& host() const { return *host_; }
#if CUERVO_FEATURE_AUDIO
    audio::AudioSystem& audio() { return *audio_; }
#endif
    const render::FontAtlas* font_atlas() const { return font_atlas_ ? &*font_atlas_ : nullptr; }
    const SessionOptions& options() const { return options_; }
    bool started() const { return host_ != nullptr; }
    const std::vector<std::filesystem::path>& script_files() const { return files_; }
    // Script units loaded (script_files() lists only those with a real file).
    std::size_t script_count() const { return sources_.size(); }
    bool native_scripts() const { return native_scripts_; }
    // Where the game's files come from: the project directory or the pack.
    const assets::Vfs& vfs() const { return *vfs_; }
    bool packed() const;

    // Scripted input for automated runs (`--press`): "tick:action,..." holds
    // each action during that tick. Unknown actions are ignored with a warning.
    // A tick may be a range ("10-20:ui_click" holds it for ticks 10 to 20).
    void set_presses(std::string_view presses);
    // `--pointer`: "tick:x:y,..." puts the pointer at (x, y) virtual pixels
    // from that tick on (until the next entry), over the mouse.
    void set_pointer_script(std::string_view script);
    // `--type`: "tick:text,..." types the text, one character per tick from
    // that tick (a text field must have the focus). Text can't contain commas.
    void set_typed(std::string_view script);
    // The input of the next tick: the devices through the input map, the
    // pointer (virtual pixels, clamped to i16) and any scripted presses.
    InputFrame sample(DeviceState& devices, vec2 pointer);
    // One fixed tick with `frame` (recorded when recording).
    void tick(const InputFrame& frame);

    // Hot reload (DESIGN.md 5.13). `poll`: reload if a watched script
    // changed; `force`: reload now; `restart`: a fresh game from scratch.
    enum class Reload { None, Reloaded, Restarted, Failed };
    Reload reload(bool poll, bool force, bool restart);
    // True when a watched script changed since the last reload.
    bool scripts_changed() const { return watcher_.changed(); }

    // Recording (DESIGN.md 4.8: a replay is seed + input frames): every tick
    // from now on is appended with the state hash after it. Starting clears.
    void start_recording();
    void stop_recording() { recording_ = false; }
    bool recording() const { return recording_; }
    const Replay& recorded() const { return replay_; }
    // The replay header for this session (no frames).
    Replay replay_header() const;

private:
    bool make_host(std::string& error);

    bool open_files(std::string& error);

    SessionOptions options_;
    std::shared_ptr<assets::Vfs> vfs_;
    std::optional<Project> project_;
    InputMap input_map_;
    std::vector<pluma::SourceFile::Part> sources_;
    std::vector<std::filesystem::path> files_;
    bool native_scripts_ = false;
#if CUERVO_FEATURE_AUDIO
    std::unique_ptr<audio::AudioSystem> audio_;
#endif
    std::optional<render::FontAtlas> font_atlas_;
    HostOptions host_options_;
    std::unique_ptr<ScriptHost> host_;
    platform::FileWatcher watcher_;
    struct Press { std::uint64_t first, last; std::size_t action; };
    std::vector<Press> presses_;
    struct PointerStep { std::uint64_t tick; vec2 at; };
    std::vector<PointerStep> pointer_script_;       // ascending by tick
    std::vector<std::pair<std::uint64_t, std::uint32_t>> typed_; // tick -> code point
    bool recording_ = false;
    Replay replay_;
};

// Plays `replay` back headless and compares the state hash after every
// recorded tick. `base` supplies the font and job settings; the project,
// seed and start scene come from `dir` and the replay.
struct ReplayCheck {
    bool ok = false;
    std::string error;                // could not run at all (or the input map moved)
    std::uint64_t ticks = 0;          // ticks simulated
    std::uint64_t compared = 0;       // hashes compared
    std::optional<std::uint64_t> first_mismatch; // tick number (1-based)
    std::uint64_t expected = 0;       // at the first mismatch
    std::uint64_t actual = 0;
    std::uint64_t final_hash = 0;     // after the last tick
};
ReplayCheck verify_replay(const std::filesystem::path& dir, const Replay& replay, const SessionOptions& base);

// The options for a replay's session: its seed and (embedded) start scene.
// False + `error` when the replay's start scene can't be decoded.
bool session_options_for(const Replay& replay, SessionOptions& options, std::string& error);

} // namespace cuervo::script
