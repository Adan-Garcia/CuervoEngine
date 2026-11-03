#include "cuervo/script/game_session.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "cuervo/assets/manifest.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/core/profiler.hpp"
#include "cuervo/jobs/job_system.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/render/font_import.hpp"
#include "cuervo/render/text_layout.hpp"
#include "cuervo/script/plugin_host.hpp"
#include "pluma/aot.hpp"

namespace cuervo::script {

namespace fs = std::filesystem;

GameSession::GameSession() = default;

GameSession::~GameSession() {
    host_.reset(); // before the audio system and font it points at
    host_options_.plugins.reset(); // then the plugins, before the module holding linked ones
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
    // Native plugins (milestone 10.5c) before any script is compiled. A native
    // pack's script module links the plugins that have static libraries.
    if (native_scripts_) {
        auto file = vfs_->open(assets::native_scripts_entry, error);
        if (file) aot_plugins_ = pluma::AotModule::open_bytes(file->bytes(), error);
        if (!aot_plugins_) {
            LOG_ERROR("{}", error);
            return false;
        }
    }
    auto plugins = load_project_plugins(*vfs_, error, aot_plugins_.get());
    if (!plugins) {
        LOG_ERROR("{}", error);
        return false;
    }
    add_importers(plugins, *vfs_);
    host_options_.plugins = std::move(plugins);

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
    // [text] fallbacks come through the Vfs, so a pack's imported `.efont` is found
    // when the source TTF/OTF was converted at export (DESIGN.md 4.19).
    if (font_atlas_) {
        fallback_atlases_.reserve(project_->font_fallbacks.size());
        for (const std::string& path : project_->font_fallbacks) {
            std::string entry = path;
            if (!vfs_->exists(entry)) {
                const std::size_t dot = entry.rfind('.');
                entry = (dot == std::string::npos ? entry : entry.substr(0, dot)) + ".efont";
            }
            std::string font_error;
            std::optional<render::FontAtlas> atlas;
            if (vfs_->exists(entry)) {
                if (auto file = vfs_->open(entry, font_error)) {
                    atlas = render::load_font_data(file->bytes(), vfs_->describe(entry));
                }
            }
            if (atlas) {
                fallback_atlases_.push_back(std::move(*atlas));
            } else {
                LOG_WARN("[text] fallbacks: can't load '{}'", path);
            }
        }
    }
    host_options_.project_dir = project_->dir;
    host_options_.debug_hooks = options_.debug_hooks;
    host_options_.on_active_vm = options_.on_active_vm;
    host_options_.vfs = vfs_;
    host_options_.main_scene = options_.scene ? options_.scene_label : project_->main_scene;
    host_options_.main_scene_doc = options_.scene;
    host_options_.font = font_atlas_ ? &*font_atlas_ : nullptr;
    for (const render::FontAtlas& f : fallback_atlases_) host_options_.font_fallbacks.push_back(&f);
    host_options_.game_name = project_->name;
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
        compile_failed_ = true;
        error = host->error(); // the host logged the diagnostics
        return false;
    }
    host_ = std::move(host);
    return true;
}

void GameSession::set_presses(std::string_view presses) {
    presses_.clear();
    axis_presses_.clear();
    // "60-300:move=1,0" has a comma of its own: a token that is only a number
    // is the y value of the axis press before it.
    std::vector<std::string> items;
    for (std::size_t pos = 0; pos < presses.size();) {
        std::size_t end = presses.find(',', pos);
        if (end == std::string_view::npos) end = presses.size();
        std::string item(presses.substr(pos, end - pos));
        pos = end + 1;
        const bool number = !item.empty() && item.find_first_not_of("+-.0123456789") == std::string::npos;
        if (number && !items.empty() && items.back().find('=') != std::string::npos &&
            items.back().find(',') == std::string::npos) {
            items.back() += "," + item;
        } else {
            items.push_back(std::move(item));
        }
    }
    for (const std::string& item : items) {
        const std::size_t colon = item.find(':');
        const std::size_t equals = item.find('=');
        const bool is_axis = colon != std::string::npos && equals != std::string::npos && equals > colon;
        std::optional<std::size_t> action;
        std::optional<std::size_t> axis_x;
        std::optional<std::size_t> axis_y;
        if (is_axis) {
            const std::string axis = item.substr(colon + 1, equals - colon - 1);
            axis_x = input_map_.axis_index(Name(axis + ".x"));
            axis_y = input_map_.axis_index(Name(axis + ".y"));
        } else if (colon != std::string::npos) {
            action = input_map_.action_index(Name(item.substr(colon + 1)));
        }
        const std::size_t comma = is_axis ? item.find(',', equals) : std::string::npos;
        if (is_axis ? !(axis_x && axis_y && comma != std::string::npos) : !action) {
            LOG_WARN("--press: ignoring '{}' (expected range:action or range:axis=x,y with a project action or axis)", item);
            continue;
        }
        const std::uint64_t first = std::strtoull(item.c_str(), nullptr, 10);
        std::uint64_t last = first;
        if (const std::size_t dash = item.find('-'); dash != std::string::npos && dash < colon) {
            last = std::strtoull(item.c_str() + dash + 1, nullptr, 10);
        }
        if (!is_axis) {
            presses_.push_back({first, std::max(first, last), *action});
            continue;
        }
        auto quantize = [](const char* text) {
            return static_cast<std::int16_t>(std::lround(std::clamp(std::strtof(text, nullptr), -1.0f, 1.0f) * 32767.0f));
        };
        axis_presses_.push_back({first, std::max(first, last), *axis_x, *axis_y, quantize(item.c_str() + equals + 1),
                                 quantize(item.c_str() + comma + 1)});
    }
}

void GameSession::set_pointer_script(std::string_view script) {
    pointer_script_.clear();
    for (std::size_t pos = 0; pos < script.size();) {
        std::size_t end = script.find(',', pos);
        if (end == std::string_view::npos) end = script.size();
        const std::string item(script.substr(pos, end - pos));
        pos = end + 1;
        const std::size_t c1 = item.find(':');
        const std::size_t c2 = c1 == std::string::npos ? c1 : item.find(':', c1 + 1);
        if (c2 == std::string::npos) {
            LOG_WARN("--pointer: ignoring '{}' (expected tick:x:y)", item);
            continue;
        }
        pointer_script_.push_back({std::strtoull(item.c_str(), nullptr, 10),
                                   vec2{std::strtof(item.c_str() + c1 + 1, nullptr), std::strtof(item.c_str() + c2 + 1, nullptr)}});
    }
    std::stable_sort(pointer_script_.begin(), pointer_script_.end(),
                     [](const PointerStep& a, const PointerStep& b) { return a.tick < b.tick; });
}

void GameSession::set_typed(std::string_view script) {
    typed_.clear();
    for (std::size_t pos = 0; pos < script.size();) {
        std::size_t end = script.find(',', pos);
        if (end == std::string_view::npos) end = script.size();
        const std::string_view item = script.substr(pos, end - pos);
        pos = end + 1;
        const std::size_t colon = item.find(':');
        if (colon == std::string_view::npos) {
            LOG_WARN("--type: ignoring '{}' (expected tick:text)", item);
            continue;
        }
        std::uint64_t tick = std::strtoull(std::string(item.substr(0, colon)).c_str(), nullptr, 10);
        const std::string_view text = item.substr(colon + 1);
        for (std::size_t i = 0; i < text.size();) {
            std::uint32_t cp = render::decode_utf8(text, i);
            typed_.emplace_back(tick++, cp);
        }
    }
}

InputFrame GameSession::sample(DeviceState& devices, vec2 pointer) {
    InputFrame input = input_map_.sample(devices);
    const std::uint64_t tick = host_->tick_count() + 1;
    for (const Press& p : presses_) {
        if (tick >= p.first && tick <= p.last) input.held |= std::uint64_t{1} << p.action;
    }
    for (const AxisPress& p : axis_presses_) {
        if (tick >= p.first && tick <= p.last) {
            input.axes[p.axis_x] = p.x;
            input.axes[p.axis_y] = p.y;
        }
    }
    for (const PointerStep& s : pointer_script_) {
        if (s.tick <= tick) pointer = s.at;
    }
    for (const auto& [t, cp] : typed_) {
        if (t == tick) input.text = cp;
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

void GameSession::tick_players(std::span<const InputFrame> players) {
    CUERVO_PROFILE_ZONE("tick");
    if (players.empty()) return;
    host_->tick_players(players);
    if (recording_) {
        // The extra players join the replay the first tick they appear; earlier ticks ran without them.
        while (replay_.extra_players.size() + 1 < players.size()) {
            replay_.extra_players.emplace_back(replay_.frames.size());
        }
        replay_.frames.push_back(players[0]);
        for (std::size_t p = 1; p <= replay_.extra_players.size(); ++p) {
            replay_.extra_players[p - 1].push_back(p < players.size() ? players[p] : InputFrame{});
        }
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
    r.action_ids = input_map_.action_ids();
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
    // Actions added, removed or reordered since the recording are mapped by their stable IDs.
    Replay adapted = replay;
    if (!adapt_replay_input(adapted, session.input_map(), check.error)) return check;
    if (session.project().tick_rate != replay.tick_rate) {
        check.error = std::format("the project's tick rate ({}) differs from the replay's ({})",
                                  session.project().tick_rate, replay.tick_rate);
        return check;
    }
    for (std::size_t i = 0; i < adapted.frames.size(); ++i) {
        if (adapted.extra_players.empty()) {
            session.tick(adapted.frames[i]);
        } else {
            std::vector<InputFrame> players{adapted.frames[i]};
            for (const auto& extra : adapted.extra_players) players.push_back(extra[i]);
            session.tick_players(players);
        }
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
