#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if CUERVO_FEATURE_AUDIO
#include "cuervo/audio/audio_system.hpp"
#else
namespace cuervo::audio {
class AudioSystem;
}
#endif
#if CUERVO_FEATURE_PHYSICS
#include "cuervo/physics/physics_world.hpp"
#endif
#include "cuervo/core/hash.hpp"
#include "cuervo/core/rng.hpp"
#include "cuervo/input/input_map.hpp"
#include "cuervo/math/affine2.hpp"
#include "cuervo/math/vec.hpp"
#if CUERVO_FEATURE_PARTICLES
#include "cuervo/particles/particle_system.hpp"
#elif CUERVO_FEATURE_COLLISION
#include "cuervo/collision/collision_world.hpp"
#endif
#include "cuervo/render/light_batcher.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/render/text_renderer.hpp"
#include "cuervo/scene/scene_file.hpp"
#include "pluma/bindings.hpp"
#include "pluma/lsp.hpp"
#include "pluma/profiler.hpp"
#include "pluma/session.hpp"
#include "pluma/source.hpp"

// The engine side of the script runtime (DESIGN.md 3 "script/": engine
// bindings for the language). Owns the language session of a game and binds
// the engine API declared in cuervo/script/prelude/*.plm: math (dmath),
// Color, Time, Screen, Random (the simulation PCG32), Input actions, the
// Canvas draw API (onto SdfBatcher / TextRenderer), the node tree, gameplay
// particles (cuervo/particles) and collision hitboxes.
//
// One fixed tick (`tick`), in this order (deterministic, DESIGN.md 4.1):
//   1. Time.* and the input frame are updated
//   2. coroutines due this tick resume, in start order
//   3. the node tree's update(dt), parents before children
//   4. the particle system ticks (simulate, spawn, collide); hits call
//      on_hit(tag, count) on hitbox owners, in hit order; then every Area
//      (prelude collision.plm) gets its overlaps and emits area_exited /
//      area_entered, in registration order
//   5. nodes queued with queue_free() leave the tree
//   6. the frame arena is freed (`new(frame)`), then the cycle collector runs
//      one step of HostOptions::gc_budget work units (DESIGN.md 5.10); it
//      runs on every tick, whatever happened before, so its timing depends
//      only on the tick count and on what scripts did
// While `Scene.paused` is set, steps 2-4 only run for nodes that process
// during a pause (`always_process`, inherited) and step 4's particles stand
// still (DESIGN.md 4.10 menus).
//
// A panic stops the game (`panicked()`); `load` with fixed scripts resumes it.
// `load` on a running game is a hot reload (DESIGN.md 5.13): state migrates
// to the new code (pluma/reload.hpp) and coroutines restart.
namespace cuervo::assets {
class Vfs;
}

namespace cuervo::script {

class VectorCache;

// What ScriptHost::draw flushes (DrawTargets::flush).
enum class DrawFlush {
    Shapes, // the SDF shapes queued so far (a vector image comes next)
    Meshes, // the vector meshes queued so far (shapes come next); clear the batcher after
    Layer,  // shapes, meshes, then text: everything drawn next covers it
};

// Where a frame's Canvas calls go (ScriptHost::draw). Shapes and text as
// before; vector images (Canvas.vector) need `vectors` (null skips them).
// Draw order is call order: the host flushes one batch before switching to
// the other, and everything at layer breaks and view changes.
struct DrawTargets {
    render::SdfBatcher* shapes = nullptr;
    render::TextRenderer* text = nullptr;
    render::FontHandle font;
    VectorCache* vectors = nullptr;
    // Draws what is queued: `view` maps world to virtual coordinates (the
    // current Camera2D, identity for the screen view) and `zoom` is its scale,
    // so world units per framebuffer pixel = 1 / (pixels per virtual unit *
    // zoom). For DrawFlush::Meshes, flush the batcher and clear() it.
    std::function<void(DrawFlush what, const Affine2& view, float zoom)> flush;
    // Framebuffer pixels per virtual unit (tessellation level of vectors).
    float pixels_per_unit = 1.0f;
    // Canvas.set_clip / clear_clip (UI scroll areas): everything flushed from
    // now on is cut to the rectangle (`pos`, `size` in virtual pixels), or
    // uncut when `on` is false. ScriptHost flushes the batchers before calling
    // it. Null (editor views): clipping is ignored.
    std::function<void(bool on, vec2 pos, vec2 size)> clip;
    // false ignores Canvas.set_view (the editor's scene view has its own
    // camera); the game draws through the scene's Camera2D.
    bool cameras = true;
    // 2D lighting (DESIGN.md 4.17, 9.7): PointLight2D and friends queue into
    // `lights` while the tree draws; Canvas.apply_lights calls `apply_lights`
    // with the current view to shade everything drawn so far. Null `lights`
    // (or a null `apply_lights`) draws the scene unlit.
    render::LightBatcher* lights = nullptr;
    std::function<void(const Affine2& view, float zoom)> apply_lights;
};

// DrawTargets drawing into the device's current viewport: `to_ndc` maps
// virtual coordinates to NDC (for the game, render::virtual_to_ndc; the
// editor's scene view passes its own camera with `cameras` off) and
// `pixels_per_unit` is framebuffer pixels per virtual unit. Flushes go to
// `shapes`, `text` and the batcher of `vectors` (optional).
// `lights` (optional) receives the scene's lights and occluders.
DrawTargets make_draw_targets(render::SdfBatcher& shapes, render::TextRenderer& text, render::FontHandle font,
                              VectorCache* vectors, const Affine2& to_ndc, float pixels_per_unit, bool cameras,
                              render::LightBatcher* lights = nullptr);

// What state_hash() folds in for a script global (numbers, bools, vectors;
// other values are skipped).
void hash_global(StateHasher& h, const pluma::Value& g);
// The hashed UI statics (prelude ui/ui.plm, in declaration order) as a game
// that never uses the UI leaves them. A player built without the UI feature
// hashes these after `unused_ui_anchor`, where the UI prelude's globals
// would be, so it reaches the full player's state hash (DESIGN.md 4.11,
// milestone 7.5 stripped players). A test checks them against the prelude.
const std::vector<pluma::Value>& unused_ui_globals();
inline constexpr std::string_view unused_ui_anchor = "Camera2D.current_camera";

class PluginSet;

struct HostOptions {
    std::uint64_t seed = 1;
    float tick_rate = 60.0f;
    vec2 virtual_size{1280.0f, 720.0f};
    // Scene.load / Scene.save paths are relative to this directory...
    std::filesystem::path project_dir;
    // ...read through this VFS when set (docs/EXPORT.md "VFS rules": the
    // pack in release); null = a development VFS over `project_dir`.
    // Scene.save writes to its user_dir() (the project in development, the
    // per-user directory in release), and Scene.load reads a saved file there
    // before the game's own.
    std::shared_ptr<const assets::Vfs> vfs;
    // The game font, for UI.measure (layout runs in the simulation, so it
    // measures with the atlas data, not the GPU font). Null: an estimate.
    const render::FontAtlas* font = nullptr;
    // Audio triggers go here (cuervo/audio); null = a private null-backend
    // system, so scripts behave identically with or without sound.
    audio::AudioSystem* audio = nullptr;
    // Native plugins (cuervo/script/plugin_host.hpp): their natives and
    // finalizers are bound beside the engine's. Kept alive by every host.
    std::shared_ptr<const PluginSet> plugins;
    // project.toml `main_scene` (project-relative): the first load builds the
    // root from this scene file instead of instantiating a main class.
    std::string main_scene;
    // Or the root is built from this scene in memory (the editor plays the
    // edited scene, unsaved changes included); `main_scene` is then only
    // its label in messages.
    std::shared_ptr<const scene::SceneDoc> main_scene_doc;
    // Node2D transforms are relative to the parent Node2D (project.toml
    // `[scene] transform_hierarchy`, DESIGN.md 4.17 "Clarified (Milestone
    // 9.1)"). false = every node behaves exactly as before the hierarchy
    // existed; a node's `top_level` also opts it out.
    bool transform_hierarchy = true;
    // Cycle collector work units per tick (pluma/heap.hpp). Part of the
    // simulation's definition: weak references into garbage cycles turn
    // null when the collector gets to them.
    std::uint64_t gc_budget = 20000;
    // A debugger (pluma/dap.hpp) or profiler: installed on every VM the host
    // creates, before its code runs (a hot reload makes a new VM, which may
    // be discarded if it fails). `on_active_vm` reports the VM that runs the
    // game from then on.
    pluma::DebugHooks* debug_hooks = nullptr;
    std::function<void(pluma::VM&)> on_active_vm;
    // Start with script profiling on (ScriptHost::set_script_profiling).
    bool profile_scripts = false;
};

class ScriptHost {
public:
    ScriptHost(HostOptions options, const InputMap& input_map);
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Compiles the game's scripts with the engine prelude. The first load
    // creates an instance of `main_class` (a Node) as the scene root; later
    // loads hot-reload. On failure the running game (if any) is kept and
    // error() has the diagnostics. With HostOptions::main_scene the first
    // load builds the root from that scene file instead. With neither (an
    // empty `main_class`) the first load only compiles: there is no root
    // until set_root (the editor's scene view).
    bool load(const std::vector<pluma::SourceFile::Part>& files, const std::string& main_class);

    // Loads an AOT-compiled native script module (`.cuervo/scripts.so`,
    // DESIGN.md 5.12, ScriptForm::Native) without `.plm` sources.
    bool load_native(const std::filesystem::path& so_path, const std::string& main_class);
    bool load_native(std::span<const std::byte> so_bytes, const std::string& main_class);

    // Replaces the scene root (retained). `enter_tree` runs ready() as the
    // game would; the editor passes false so authoring never runs game code
    // beyond field initializers and init(). Returns false on a panic.
    // Replacing a root unloads its scene: the scene arena (`new(scene)`,
    // DESIGN.md 5.10) is freed first, including objects the new tree's
    // init() put there.
    bool set_root(const pluma::Value& root, bool enter_tree);
    const pluma::Value& root() const { return root_; }
    // Forgets a panic (the editor rebuilds its preview tree after edits).
    void clear_error();

    void tick(const InputFrame& frame);
    // Calls the tree's draw(g) (Scene.draw_frame); shapes go to `shapes`,
    // text to `text`. `layer_break` flushes both, so what is drawn next
    // covers them (Canvas.new_layer); without it layers just append.
    void draw(render::SdfBatcher& shapes, render::TextRenderer& text, render::FontHandle font,
              const std::function<void()>& layer_break = {});
    // The same with every target (vector images, cameras). Everything drawn
    // is flushed before it returns, so the caller may draw on top.
    void draw(const DrawTargets& targets);

    bool running() const { return session_ != nullptr && !panicked_; }
    bool panicked() const { return panicked_; }
    const std::string& error() const { return error_; }
    // The panic that stopped the game, with its frames and locals
    // (pluma::PanicInfo::frames, DESIGN.md 5.8); empty while running.
    const std::optional<pluma::PanicInfo>& last_panic() const { return last_panic_; }
    // Notes from the last load (hot reload warnings).
    const std::vector<std::string>& notes() const { return notes_; }
    std::uint64_t tick_count() const { return tick_; }
    std::uint64_t reloads() const { return reloads_; }
    // Scene.quit() was called.
    bool quit_requested() const { return quit_requested_; }
    // A text field had the focus in the last tick (UI.wants_text): the window
    // should deliver typed text (Window::set_text_input).
    bool wants_text_input() const { return text_wanted_; }

    // Fingerprint of the simulation (particles, RNG, tick, and the script
    // globals' numbers), for replay comparison.
    std::uint64_t state_hash() const;

    pluma::Session* session() { return session_.get(); }
    // Script profiling (pluma/profiler.hpp, DESIGN.md 5.14 "Clarified (Phase
    // 8 -- Profiler)"): a ScriptProfiler on this VM and on every VM a hot
    // reload makes; its numbers survive reloads. A VM has one DebugHooks at a
    // time: a VM that already has other hooks (a debugger) isn't profiled.
    // Turning it off uninstalls it and keeps the numbers.
    void set_script_profiling(bool on);
    bool script_profiling() const { return profiling_scripts_; }
    pluma::ScriptProfiler* script_profiler() { return script_profiler_.get(); }
    const pluma::Bindings& bindings() const { return bindings_; }
#if CUERVO_FEATURE_PARTICLES
    particles::ParticleSystem& particles() { return particles_; }
    Pcg32& rng() { return particles_.rng(); }
    // ---- used by the natives
    struct EmitterData;
    void free_emitter(particles::EmitterId id);
#else
    Pcg32& rng() { return rng_; }
#endif
#if CUERVO_FEATURE_AUDIO
    audio::AudioSystem& audio() { return *audio_; }
#endif
#if CUERVO_FEATURE_PHYSICS
    physics::PhysicsWorld& physics_world() { return *physics_; }
    const physics::PhysicsWorld& physics_world() const { return *physics_; }
#endif
    float tick_seconds() const { return 1.0f / options_.tick_rate; }

private:
    void install_debug_hooks(pluma::Session& s) {
        if (options_.debug_hooks != nullptr) s.vm().set_debug_hooks(options_.debug_hooks);
    }
    void activated() {
        if (options_.on_active_vm) options_.on_active_vm(session_->vm());
    }
    void bind();
    void bind_scene_and_ui(); // script_host_scene.cpp
    void bind_animation();    // anim_bindings.cpp
    void bind_lighting();     // light_bindings.cpp
#if CUERVO_FEATURE_PHYSICS
    void bind_physics();
#endif
    bool finish_load(std::unique_ptr<pluma::Session> next, const std::string& main_class);
    // A project-relative scene file through the VFS (Scene.load, main_scene).
    std::optional<scene::SceneDoc> load_scene(std::string_view path, std::string& error, scene::LoadInfo* info) const;
    bool paused() const;
    bool frozen(const pluma::Object* owner) const;
    pluma::VmOptions vm_options();
    bool check(const pluma::RunResult& r, const char* what);
    void set_time_globals();
    void tick_scripts(const InputFrame& frame);
    void end_tick();
    pluma::Value name_value(std::string_view text);

    HostOptions options_;
    Input input_;
    pluma::Bindings bindings_;
    std::unique_ptr<pluma::Session> session_;
    pluma::Value root_;
    pluma::Value canvas_;
#if CUERVO_FEATURE_PARTICLES
    particles::ParticleSystem particles_;
    struct EmitterMeta {
        bool used = false;
        std::string tag;
    };
    std::vector<EmitterMeta> emitters_;
    std::vector<particles::EmitterId> free_emitters_;
#else
    Pcg32 rng_;
    // Unpaused ticks: what the particle system would count (state_hash()).
    std::uint64_t particle_ticks_ = 0;
#endif
#if CUERVO_FEATURE_AUDIO
    std::unique_ptr<audio::AudioSystem> own_audio_;
    audio::AudioSystem* audio_ = nullptr;
#endif

#if CUERVO_FEATURE_COLLISION
    struct Hitbox {
        pluma::Value owner; // retained for the tick
        vec2 center;
        float radius = 0.0f;
        std::uint32_t layer = 0;
        // Areas (Collision.area): a capsule from `start` to `end` when
        // `capsule`, and the layers whose areas it reports.
        bool area = false;
        bool capsule = false;
        vec2 start;
        vec2 end;
        std::uint32_t mask = 0;
    };
    std::vector<Hitbox> hitboxes_;
    void add_hitboxes(collision::CollisionWorld& world) const;
    bool resolve_areas(const collision::CollisionWorld& world);
#if !CUERVO_FEATURE_PARTICLES
    collision::CollisionWorld collision_;
#endif
#endif
#if CUERVO_FEATURE_PHYSICS
    std::unique_ptr<physics::PhysicsWorld> physics_;
#endif

    // Canvas targets while draw() runs.
    render::SdfBatcher* shapes_ = nullptr;
    render::TextRenderer* text_ = nullptr;
    render::FontHandle font_;
    const DrawTargets* targets_ = nullptr;
    // The view Canvas.set_view chose (world -> virtual) and its zoom.
    Affine2 view_{};
    float view_zoom_ = 1.0f;
    bool meshes_pending_ = false; // vector meshes queued since the last flush
    // The SDF batcher, after flushing queued vector meshes (call order).
    render::SdfBatcher* shape_target();
    void flush_draw(DrawFlush what);
    bool quit_requested_ = false;
    bool text_wanted_ = false;
    // UI.image_size results by path (layout reads them in the simulation).
    std::map<std::string, vec2, std::less<>> image_sizes_;

    std::uint64_t tick_ = 0;
    // Fold of every animated value written (Anim.set); joins the state hash once non-zero,
    // so games without animation keep their hashes.
    std::uint64_t anim_digest_ = 0;
    std::uint64_t idle_ticks_ = 0;
    std::uint64_t reloads_ = 0;
    bool panicked_ = false;
    std::string error_;
    std::optional<pluma::PanicInfo> last_panic_;
    std::vector<std::string> notes_;
    // Outlives session_ (~ScriptHost resets session_ first).
    std::unique_ptr<pluma::ScriptProfiler> script_profiler_;
    bool profiling_scripts_ = false;
};

// Returns the embedded engine prelude sources (`cuervo/script/prelude/*.plm`).
std::vector<pluma::SourceFile::Part> engine_preludes();

// Returns LSP server options pre-configured with the Cuervo engine prelude
// (`Node`, `Canvas`, `Input`, `Color`, `Audio`, `Particles`, `Scene`, `UI`, etc.).
pluma::lsp::ServerOptions default_lsp_options(std::string root_path = {}, bool check_style = true);

// Compiles the engine prelude + `files` and emits a self-contained C11/C++20
// translation unit (`pluma::emit_c`, DESIGN.md 5.12).
// A native plugin linked into an AOT module (DESIGN.md 4.18: AOT exports link
// plugins statically): its package.toml `linux_static` / `windows_static` / `windows_gnu_static`.
// The module lists them in `cuervo_aot_plugins`, which the player registers
// (PluginSet::add_aot_plugins) before binding the scripts.
struct StaticPlugin {
    std::string name;
    std::string version;
    bool sim_safe = false;
    std::filesystem::path library;
};

// `plugins` provides the natives of a project's plugins (milestone 10.5c).
std::optional<std::string> emit_aot_c(const std::vector<pluma::SourceFile::Part>& files, std::string& error,
                                      std::shared_ptr<const PluginSet> plugins = nullptr);

// Compiles the engine prelude + `files` to a native shared library image
// (`.cuervo/scripts.so`, ScriptForm::Native).
std::optional<std::vector<std::byte>> compile_aot_scripts(const std::vector<pluma::SourceFile::Part>& files,
                                                          std::string& error, int opt_level = 2,
                                                          std::shared_ptr<const PluginSet> plugins = nullptr,
                                                          const std::vector<StaticPlugin>& statics = {});

} // namespace cuervo::script
