#pragma once

#include <optional>
#include <string>

#include "cuervo/brand/brand.hpp"
#include "cuervo/rhi/rhi.hpp"

// The editor's welcome window: pick a project (recent projects, templates, open a folder).
// Its own small window, device and ImGui context, closed before the editor window opens.
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
    bool show_welcome = true; // the project picker
    // Screenshots: capture the first screen after this many frames and exit.
    std::string screenshot;
    int screenshot_frame = 6;
};

enum class LauncherOutcome { OpenProject, Closed, Continue, Failed };

struct LauncherResult {
    LauncherOutcome outcome = LauncherOutcome::Closed;
    std::string project; // OpenProject: the folder to open
};

// Shows the launcher and returns what the user chose.
LauncherResult run_launcher(const LauncherOptions& options);

} // namespace cuervo::editor
