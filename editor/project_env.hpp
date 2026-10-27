#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cuervo/input/input_map.hpp"
#include "cuervo/math/vec.hpp"
#include "cuervo/render/font.hpp"
#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"
#include "editor/class_registry.hpp"

// The project the editor works on (DESIGN.md 4.13): project.toml, its
// scripts compiled once into a ScriptHost that never ticks, the node class
// registry built from them, and the scene view's *preview*: the edited scene
// instantiated as live script objects so the engine's own renderer draws it
// (node draw() methods through Canvas onto the SDF batcher and MSDF text).
//
// The preview is not the simulation. It is rebuilt from the document after
// every edit: field initializers and init() run, ready() and update() never
// do, so authoring can't start coroutines, spawn particles or touch the
// RNG; UI roots get one layout pass so controls have their rects.
namespace cuervo::editor {

// Where a node is in the scene view, for picking and the move gizmo.
struct NodeBounds {
    bool has_position = false; // Node2D (and Control): `position`
    vec2 position{0.0f, 0.0f}; // world position (parent chain applied when the hierarchy is on)
    float parent_rotation = 0.0f;       // the parent's world rotation and scale: a world
    vec2 parent_scale{1.0f, 1.0f};      // drag delta becomes a local one through these
    bool has_rect = false;     // Control: the laid-out rect
    vec2 rect_min{0.0f, 0.0f};
    vec2 rect_size{0.0f, 0.0f};
};

class ProjectEnv {
public:
    ProjectEnv();
    ~ProjectEnv();
    ProjectEnv(const ProjectEnv&) = delete;
    ProjectEnv& operator=(const ProjectEnv&) = delete;

    // Loads `<dir>/project.toml` and compiles its scripts. `font` (optional,
    // kept by pointer) is what UI.measure lays text out with.
    bool open(const std::filesystem::path& dir, const render::FontAtlas* font, std::string& error);
    // No project: just the engine prelude's node classes (tests, scratch).
    bool open_prelude_only(std::string& error);

    const script::Project& project() const { return project_; }
    bool has_project() const { return has_project_; }
    std::filesystem::path project_dir() const { return project_.dir; }
    // A scene path as the user typed it: relative paths are project-relative.
    std::filesystem::path resolve(const std::string& path) const;
    // `path` relative to the project when it is inside it (for display/save).
    std::string relative(const std::filesystem::path& path) const;

    const ClassRegistry& classes() const { return registry_; }
    script::ScriptHost& host() { return *host_; }
    vec2 virtual_size() const;

    // Rebuilds the preview from `doc`. Errors (unknown classes, panics in
    // init()) are logged once and leave an empty preview.
    void rebuild_preview(const scene::SceneDoc& doc);
    bool preview_ok() const { return preview_ok_; }
    // Poses the preview at tick `tick` of animation `name` of the
    // AnimationPlayer at doc index `player` (the timeline's scrub, milestone
    // 9.8): play(name) then seek(tick) on the preview node. The pose stays
    // until the next rebuild_preview. false (and `error`) when it can't.
    bool preview_animation(std::size_t player, const std::string& name, float tick, std::string& error);
    // Per document node (same order as the doc), after layout.
    const std::vector<NodeBounds>& bounds() const { return bounds_; }

private:
    void release_preview();
    bool compile(const std::vector<pluma::SourceFile::Part>& files, std::string& error);

    script::Project project_;
    bool has_project_ = false;
    InputMap input_map_;
    std::unique_ptr<script::ScriptHost> host_;
    ClassRegistry registry_;
    std::vector<pluma::Value> preview_nodes_;
    std::vector<NodeBounds> bounds_;
    bool preview_ok_ = false;
    std::string last_report_;
};

} // namespace cuervo::editor
