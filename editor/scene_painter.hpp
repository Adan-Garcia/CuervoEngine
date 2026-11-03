#pragma once
// The scene view's renderer, independent of any window or UI toolkit (DESIGN.md 4.13). The ImGui
// editor (EditorApp::render_scene_view) and the RmlUi shell both call paint_scene_view() with the
// pixel rectangle the scene region occupies; the rest of the frame is the caller's business.

#include <optional>

#include "cuervo/brand/brand.hpp"
#include "cuervo/render/font.hpp"
#include "cuervo/render/light_batcher.hpp"
#include "cuervo/render/mesh_batcher.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/render/sprite_batcher.hpp"
#include "cuervo/render/sprite_library.hpp"
#include "cuervo/render/text_renderer.hpp"
#include "cuervo/rhi/rhi.hpp"
#include "cuervo/script/vector_cache.hpp"
#include "editor/view_camera.hpp"

namespace cuervo::script { class ScriptHost; class GameSession; }

namespace cuervo::editor {

class SceneInput;

// Non-owning bundle of the batchers a scene paint needs (EditorApp keeps its own; the RmlUi shell owns a
// SceneResources).
struct SceneTargets {
    rhi::Device* device = nullptr;
    render::SdfBatcher* shapes = nullptr;
    render::LightBatcher* lights = nullptr;
    render::TextRenderer* text = nullptr;
    render::MeshBatcher* meshes = nullptr;
    script::VectorCache* vectors = nullptr;
    render::SpriteBatcher* sprites = nullptr;
    render::SpriteLibrary* library = nullptr;
    render::FontHandle font{};
};

// Owns the batchers (for callers that have no EditorApp).
struct SceneResources {
    render::SdfBatcher shapes;
    render::LightBatcher lights;
    render::TextRenderer text;
    render::MeshBatcher meshes;
    script::VectorCache vectors{meshes};
    render::SpriteBatcher sprites;
    render::SpriteLibrary library;
    render::FontHandle font{};
    bool init(rhi::Device& device, const render::FontAtlas* atlas);
    SceneTargets targets(rhi::Device& device);
};

struct ScenePaintParams {
    vec2 virtual_size{1280.0f, 720.0f};    // the game's screen
    rhi::Color clear_color{0.0f, 0.0f, 0.0f, 1.0f};
    script::ScriptHost* drawn = nullptr;   // null: only the grid and the frame
    bool collision_shapes = true;
    bool lighting = true;
    ViewCamera camera;
    const brand::ThemeColors* colors = nullptr;
    const SceneInput* overlay = nullptr;   // selection outline and gizmo; null: caller draws its own
};

// `view` is the scene region in framebuffer pixels. Rects are clamped to `target` (the framebuffer),
// so a region partly or wholly outside it never reaches the device.
rhi::Rect clamp_rect(rhi::Rect r, int target_w, int target_h);
void paint_scene_view(const SceneTargets& t, const ScenePaintParams& p, rhi::Rect view);
// The running game (play mode), letterboxed like the runtime's `fit` scale mode inside `view`.
void paint_game_view(const SceneTargets& t, script::GameSession& session, rhi::Rect view);

} // namespace cuervo::editor
