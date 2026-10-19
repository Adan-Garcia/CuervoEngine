#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Editor panels (DESIGN.md 4.13, layout 7.2 "Godot-like: scene tree left,
// viewport center, inspector right, assets + console bottom"). Each panel
// names the dock it lives in; panels sharing a dock become tabs. Phase 6B
// adds its panels (asset browser, input map, replay/debugger, tier report)
// by registering them here, without touching the layout code.
namespace cuervo::editor {

class EditorApp;

enum class Dock { Left, Center, Right, Bottom };

class Panel {
public:
    virtual ~Panel() = default;
    virtual const char* title() const = 0;
    virtual Dock dock() const = 0; // where the default layout docks it (it can be moved)
    // false: no window padding or scrollbars (the scene view fills its window).
    virtual bool padded() const { return true; }
    // Draws the panel's contents inside its dock (the tab is already open).
    virtual void draw(EditorApp& app) = 0;
    bool visible = true;
    // Bring this panel's tab to the front on the next frame (e.g. the
    // debugger when the game panics).
    bool select = false;
};

class PanelRegistry {
public:
    Panel& add(std::unique_ptr<Panel> panel);
    std::vector<Panel*> in(Dock dock) const;
    Panel* find(std::string_view title) const;
    const std::vector<std::unique_ptr<Panel>>& all() const { return panels_; }

private:
    std::vector<std::unique_ptr<Panel>> panels_;
};

// The 6A panels.
std::unique_ptr<Panel> make_scene_tree_panel();
std::unique_ptr<Panel> make_scene_view_panel();
std::unique_ptr<Panel> make_inspector_panel();
std::unique_ptr<Panel> make_console_panel();
// Phase 6B.
std::unique_ptr<Panel> make_asset_panel();
std::unique_ptr<Panel> make_input_map_panel();
std::unique_ptr<Panel> make_debugger_panel();
std::unique_ptr<Panel> make_replay_panel();
std::unique_ptr<Panel> make_tier_panel();
// Phase 8.
std::unique_ptr<Panel> make_profiler_panel();
// Milestone 8.3.
std::unique_ptr<Panel> make_history_panel();

// Drag-and-drop payload of the asset browser: a project-relative path
// (NUL-terminated). Inspector text fields and the scene view accept it.
inline constexpr const char* asset_payload = "CUERVO_ASSET";
} // namespace cuervo::editor
