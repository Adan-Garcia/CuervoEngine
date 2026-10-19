#include "cuervo/script/game_session.hpp"

#include <algorithm>
#include <format>

#include "cuervo/assets/manifest.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/core/profiler.hpp"
#include "cuervo/jobs/job_system.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/render/font_import.hpp"

namespace cuervo::script {

namespace fs = std::filesystem;

GameSession::GameSession() = default;

GameSession::~GameSession() {
    host_.reset(); // before the audio system and font it points at
}

bool GameSession::packed() const { return vfs_ && vfs_->packed(); }

// The game's files: the project directory (development) or the pack
// (release), plus the pack's manifest checks. docs/EXPORT.md "VFS rules".
bool GameSession::open_files(std::string& error) {
    native_scripts_ = false;
    if (options_.pack.empty()) {
        vfs_ = assets::Vfs::dev(options_.dir);
        return true;
    }
    auto pack = assets::Pack::open(options_.pack, error);
    if (!pack) return false;
    vfs_ = assets::Vfs::release(pack, {});
    std::string text;
    if (!vfs_->read_text(assets::manifest_path, text, error)) {
        error = std::format("'{}' is not an exported game (no {})", platform::utf8(options_.pack),
                            assets::manifest_path);
        return false;
    }
    auto manifest = assets::parse_manifest(text, error);
    if (!manifest) return false;
    if (manifest->scripts != assets::ScriptForm::Source && manifest->scripts != assets::ScriptForm::Native) {
        error = std::format("'{}' ships its scripts as '{}', which this runtime can't load (it runs sources or native)",
                            platform::utf8(options_.pack), assets::script_form_name(manifest->scripts));
        return false;
    }
    native_scripts_ = (manifest->scripts == assets::ScriptForm::Native);
    if (pack->platform() != assets::PackPlatform::Any && pack->platform() != assets::host_platform()) {
        LOG_WARN("'{}' was packed for {}, running on {}", platform::utf8(options_.pack),
                 assets::platform_name(pack->platform()), assets::platform_name(assets::host_platform()));
    }
    LOG_INFO("Pack '{}': {} entries, {} bytes, content hash {:016x}", platform::utf8(options_.pack),
             pack->entries().size(), pack->size_bytes(), pack->content_hash());
    return true;
}

bool GameSession::init(const SessionOptions& options, std::string& error) {
    options_ = options;
    if (!open_files(error)) {
        LOG_ERROR("{}", error);
        return false;
    }
    project_ = load_project(*vfs_, error);
    if (!project_) {
        LOG_ERROR("{}", error);
        return false;
    }
    // Saves (Scene.save) never go into a pack: the per-user directory.
    if (vfs_->packed()) {
        vfs_->set_user_dir(options_.user_dir.empty() ? platform::user_data_dir(project_->name) : options_.user_dir);
    }
    if (options_.configure_jobs) {
        jobs::configure(jobs::resolve_thread_count(
            options_.jobs > 0 ? std::optional<unsigned>(options_.jobs) : std::nullopt,
            project_->jobs > 0 ? std::optional<unsigned>(project_->jobs) : std::nullopt));
    }
    if (!apply_input(*project_, input_map_, error)) {
        error = "project.toml: " + error;
        LOG_ERROR("{}", error);
        return false;
    }
    if (native_scripts_) {
        sources_.clear();
        files_.clear();
        if (!vfs_->exists(assets::native_scripts_entry)) {
            error = std::format("'{}': missing native script module '{}'", platform::utf8(options_.pack),
                                assets::native_scripts_entry);
            LOG_ERROR("{}", error);
            return false;
        }
    } else if (!read_scripts(*vfs_, sources_, &files_, error)) {
        LOG_ERROR("{}", error);
        return false;
    }

#if CUERVO_FEATURE_AUDIO
    // Audio (DESIGN.md 4.9): every file under assets/ is registered by name.
    // The null backend triggers the same sounds without a device.
    audio_ = std::make_unique<audio::AudioSystem>(audio::AudioOptions{
        .backend = options_.audio_device ? audio::Backend::Device : audio::Backend::Null,
        .tick_rate = project_->tick_rate,
        .root = project_->dir,
        .vfs = vfs_});
    const std::size_t sounds = audio_->load_directory(*vfs_, "assets");
    if (sounds > 0) LOG_INFO("Audio: {} sound(s) from '{}'", sounds, vfs_->describe("assets"));

    host_options_.audio = audio_.get();
#else
    host_options_.audio = nullptr;
#endif
    // The font is loaded before the game starts: UI layout measures text in
    // the simulation (UI.measure), with the same atlas the renderer uses.
    if (!options_.font_path.empty()) {
        font_atlas_ = render::load_font_file(options_.font_path);
        if (!font_atlas_) LOG_WARN("No font ('{}'): Canvas.text draws nothing", options_.font_path);
    } else if (vfs_->exists(assets::default_font_entry)) {
        // An exported game carries its font (imported to .efont at export).
        if (auto file = vfs_->open(assets::default_font_entry, error)) {
            font_atlas_ = render::load_font_data(file->bytes(), vfs_->describe(assets::default_font_entry));
        }
        if (!font_atlas_) LOG_WARN("No font ('{}'): Canvas.text draws nothing", assets::default_font_entry);
    }
    host_options_.project_dir = project_->dir;
    host_options_.debug_hooks = options_.debug_hooks;
    host_options_.on_active_vm = options_.on_active_vm;
    host_options_.vfs = vfs_;
    host_options_.main_scene = options_.scene ? options_.scene_label : project_->main_scene;
    host_options_.main_scene_doc = options_.scene;
    host_options_.font = font_atlas_ ? &*font_atlas_ : nullptr;
    host_options_.seed = options_.seed.value_or(project_->seed);
    host_options_.tick_rate = project_->tick_rate;
    host_options_.transform_hierarchy = project_->transform_hierarchy;
    host_options_.profile_scripts = options_.profile_scripts;
    host_options_.virtual_size = {static_cast<float>(project_->virtual_size.x()),
                                  static_cast<float>(project_->virtual_size.y())};
    if (!make_host(error)) return false;
    // Hot reload is development-only: a pack never changes under the game.
    if (!vfs_->packed()) watcher_.reset(files_, project_->dir / "scripts", ".plm");
    return true;
}

bool GameSession::make_host(std::string& error) {
    auto host = std::make_unique<ScriptHost>(host_options_, input_map_);
    if (native_scripts_) {
        auto file = vfs_->open(assets::native_scripts_entry, error);
        if (!file) {
            LOG_ERROR("{}", error);
            return false;
        }
        if (!host->load_native(file->bytes(), project_->main_class)) {
            error = host->error();
            return false;
        }
    } else if (!host->load(sources_, project_->main_class)) {
        error = host->error(); // the host logged the diagnostics
        return false;
    }
    host_ = std::move(host);
    return true;
}

void GameSession::set_presses(std::string_view presses) {
    presses_.clear();
    for (std::size_t pos = 0; pos < presses.size();) {
        std::size_t end = presses.find(',', pos);
        if (end == std::string_view::npos) end = presses.size();
        const std::string item(presses.substr(pos, end - pos));
        pos = end + 1;
        const std::size_t colon = item.find(':');
        auto action = colon == std::string::npos ? std::nullopt : input_map_.action_index(Name(item.substr(colon + 1)));
        if (!action) {
            LOG_WARN("--press: ignoring '{}' (expected tick:action with a project action)", item);
            continue;
        }
        presses_.emplace_back(std::strtoull(item.c_str(), nullptr, 10), *action);
    }
}

InputFrame GameSession::sample(DeviceState& devices, vec2 pointer) {
    InputFrame input = input_map_.sample(devices);
    for (const auto& [tick, action] : presses_) {
        if (tick == host_->tick_count() + 1) input.held |= std::uint64_t{1} << action;
    }
    input.pointer = {static_cast<std::int16_t>(std::clamp(pointer.x(), -32768.0f, 32767.0f)),
                     static_cast<std::int16_t>(std::clamp(pointer.y(), -32768.0f, 32767.0f))};
    return input;
}

void GameSession::tick(const InputFrame& frame) {
    CUERVO_PROFILE_ZONE("tick");
    host_->tick(frame);
    if (recording_) {
        replay_.frames.push_back(frame);
        replay_.hashes.push_back(host_->state_hash());
    }
}

GameSession::Reload GameSession::reload(bool poll, bool force, bool restart) {
    // FileWatcher::changed() is always false where nothing can change (web).
    if (!(force || restart || (poll && watcher_.changed()))) return Reload::None;
    std::string error;
    if (native_scripts_) {
        if (!restart) return Reload::None;
#if CUERVO_FEATURE_AUDIO
        audio_->reset();
#endif
        if (!make_host(error)) return Reload::Failed;
        LOG_INFO("Game restarted");
        if (recording_) start_recording();
        return Reload::Restarted;
    }
    if (!read_scripts(*vfs_, sources_, &files_, error)) {
        LOG_ERROR("{}", error);
        return Reload::Failed;
    }
    if (!vfs_->packed()) watcher_.reset(files_, project_->dir / "scripts", ".plm");
    if (restart) {
#if CUERVO_FEATURE_AUDIO
        audio_->reset();
#endif
        if (!make_host(error)) return Reload::Failed;
        LOG_INFO("Game restarted");
        if (recording_) start_recording(); // a restart is a new run
        return Reload::Restarted;
    }
    if (recording_) {
        LOG_WARN("Hot reload while recording: the replay will play tick {} on with the scripts as they are then",
                 host_->tick_count());
    }
    return host_->load(sources_, project_->main_class) ? Reload::Reloaded : Reload::Failed;
}

Replay GameSession::replay_header() const {
    Replay r;
    r.project = project_->name;
    r.seed = host_options_.seed;
    r.tick_rate = project_->tick_rate;
    r.actions = input_map_.action_names();
    r.axes = input_map_.axis_names();
    if (options_.scene) {
        r.start_scene = options_.scene_label.empty() ? std::string("(scene)") : options_.scene_label;
        r.scene = scene::write_binary(*options_.scene);
    }
    return r;
}

void GameSession::start_recording() {
    replay_ = replay_header();
    recording_ = true;
}

bool session_options_for(const Replay& replay, SessionOptions& options, std::string& error) {
    options.seed = replay.seed;
    options.scene.reset();
    options.scene_label.clear();
    if (!replay.scene.empty()) {
        auto doc = scene::read_binary(replay.scene, error);
        if (!doc) {
            error = "the replay's start scene: " + error;
            return false;
        }
        options.scene = std::make_shared<const scene::SceneDoc>(std::move(*doc));
        options.scene_label = replay.start_scene;
    }
    return true;
}

ReplayCheck verify_replay(const fs::path& dir, const Replay& replay, const SessionOptions& base) {
    ReplayCheck check;
    SessionOptions options = base;
    options.dir = dir;
    options.audio_device = false;
    if (!session_options_for(replay, options, check.error)) return check;
    GameSession session;
    if (!session.init(options, check.error)) return check;
    if (session.input_map().action_names() != replay.actions || session.input_map().axis_names() != replay.axes) {
        check.error = "the project's input actions or axes changed since the replay was recorded";
        return check;
    }
    if (session.project().tick_rate != replay.tick_rate) {
        check.error = std::format("the project's tick rate ({}) differs from the replay's ({})",
                                  session.project().tick_rate, replay.tick_rate);
        return check;
    }
    for (std::size_t i = 0; i < replay.frames.size(); ++i) {
        session.tick(replay.frames[i]);
        ++check.ticks;
        if (i >= replay.hashes.size()) continue;
        ++check.compared;
        const std::uint64_t h = session.host().state_hash();
        if (h != replay.hashes[i] && !check.first_mismatch) {
            check.first_mismatch = i + 1;
            check.expected = replay.hashes[i];
            check.actual = h;
            break; // everything after diverges too
        }
    }
    check.final_hash = session.host().state_hash();
    check.ok = !check.first_mismatch;
    return check;
}

} // namespace cuervo::script
