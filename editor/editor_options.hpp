#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cuervo/brand/brand.hpp"
#include "cuervo/rhi/rhi.hpp"
#include "editor/editor_controller.hpp"

// Start-up options shared by both editors (`cuervo-editor` flags, editor/README.md). Plain data,
// no UI-library types, so main.cpp, editor/rml/ and the tests include it without ImGui.
namespace cuervo::editor {

enum class SplashMode { Auto, On, Off };

struct EditorOptions {
    std::string project;          // --project <dir>; none: the welcome screen picks one
    std::string scene;            // --scene <path>: open this scene (project-relative or absolute)
    rhi::Backend backend = rhi::Backend::OpenGL;
    bool allow_fallback = true;   // no --backend: Vulkan, else GL
    bool gl_es = false;
    bool vsync = true;
    std::string script_commands;  // --script-commands <file>: apply, save, exit (headless)
    std::string screenshot;       // --screenshot <file.ppm>: capture the whole window, exit
    int screenshot_frame = 6;     // frames rendered before the capture (layout settles)
    double quit_after = 0.0;      // seconds; 0 = until closed
    std::string font_path;        // UI and scene text
    // --view <x>,<y>,<zoom>: start the scene view there instead of framing
    // the game screen (screenshots of a zoomed view).
    std::optional<vec3> view;
    int width = 1600;
    int height = 900;
    // --replay-verify <file>: play a replay back headless, compare every
    // tick's state hash, exit 1 at the first mismatch (no window).
    std::string replay_verify;
    unsigned jobs = 0;            // --jobs: job system threads for play sessions
    bool audio = true;            // --no-audio: play sessions use the null audio backend
    // --tabs <Title,Title>: bring these panels' tabs to the front at start
    // (screenshots of the 6B panels).
    std::vector<std::string> tabs;
    // --theme light|dark: the brand theme (DESIGN.md 4.16); default the OS's.
    std::optional<brand::Theme> theme;
    // --splash: show the splash until a click or key (screenshots of it);
    // --no-splash: never. Default: shown at start, except for screenshots.
    SplashMode splash = SplashMode::Auto;
    bool no_viewports = false;    // --no-viewports: keep every panel inside the window (the single-window fallback)
    // --screen welcome: show the welcome screen even when a project is given (for screenshots/tests).
    std::string screen;
    // --tool select|move|rotate|scale, --select </a,/b>, --palette [query]: start in that
    // state (screenshots of the gizmos and the palette).
    std::optional<Tool> tool;
    std::vector<std::string> select_paths;
    std::optional<std::string> palette;
    // --layout <name>: start in that saved layout.
    std::string layout;
    std::optional<float> ui_scale;    // --ui-scale <float>: UI spacing/size scale factor
    // --ui-events "move x,y; down x,y; up x,y; click x,y; rclick x,y; key Escape": RmlUi editor only. Posts
    // these as real SDL events, two frames apart, before the --screenshot is taken (golden screenshots of
    // drag, menu and dialog states). Window pixel coordinates.
    std::string ui_events;
    std::optional<float> font_scale;  // --font-scale <float>: Font size scale factor
};

} // namespace cuervo::editor
