#pragma once

#include <optional>
#include <string>

#include "cuervo/brand/brand.hpp"
#include "cuervo/rhi/rhi.hpp"

// The editor's first-launch windows (DESIGN.md 4.16 "Clarified (Milestone 8.3)"): the
// EULA agreement a fresh user must accept, then the welcome screen where a project is
// picked (recent projects, templates, open a folder). Its own small window, device and
// ImGui context, closed before the editor window opens.
namespace cuervo::editor {

struct LauncherOptions {
    rhi::Backend backend = rhi::Backend::OpenGL;
    bool allow_fallback = true;
    bool gl_es = false;
    bool vsync = true;
    std::string font_path;
    std::optional<brand::Theme> theme;
    int width = 1100;
    int height = 760;
    bool show_eula = false;      // the agreement comes first
    bool show_welcome = true;    // then the project picker
    // Screenshots: capture the first screen after this many frames and exit; nothing is stored.
    std::string screenshot;
    int screenshot_frame = 6;
    bool store_acceptance = true;
};

enum class LauncherOutcome { OpenProject, Declined, Closed, Continue, Failed };

struct LauncherResult {
    LauncherOutcome outcome = LauncherOutcome::Closed;
    std::string project; // OpenProject: the folder to open
};

// Shows the screens in `options` and returns what the user chose. Accepting the EULA
// writes eula-accepted.txt (unless `store_acceptance` is false). `Continue`: the EULA
// was accepted and there is no welcome screen to show.
LauncherResult run_launcher(const LauncherOptions& options);

} // namespace cuervo::editor
