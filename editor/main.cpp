// cuervo-editor: the Cuervo Engine editor (DESIGN.md 4.13, editor/README.md).
//
//   cuervo-editor [--project <dir>] [--scene <path>] [--backend gl|vulkan] [--gl-es]
//                 [--script-commands <file>] [--screenshot <file.ppm>]
//                 [--quit-after <seconds>] [--size <w>x<h>] [--font <file.ttf>] [--view <x>,<y>,<zoom>]
//                 [--jobs <n>] [--no-audio] [--tabs <Panel,...>] [--profile]
//                 [--theme light|dark] [--splash|--no-splash] [--accept-eula] [--layout <name>]
//                 [--screen eula|welcome] [--tool select|move|rotate|scale] [--select </a,/b>] [--palette [query]]
//   cuervo-editor --project <dir> --replay-verify <file.replay> [--jobs <n>]
//   cuervo-editor --new-project <dir> [--template empty|walker] [--name <name>]
//
// --new-project makes a project the way the welcome screen's "New project" does and exits
// (headless, so it follows the command-line EULA rules); --template defaults to empty and
// --name to the folder's name.
// --script-commands applies a file of editor operations (headless: no
// window), saves and exits; with --screenshot it then opens the window and
// captures it. --replay-verify plays a replay back headless and exits 1 at
// the first tick whose state hash differs from the recorded one.

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include "cuervo/core/log.hpp"
#include "cuervo/core/profiler.hpp"
#include "cuervo/jobs/job_system.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/platform/eula.hpp"
#include "editor/app.hpp"
#include "editor/projects.hpp"

namespace {

std::string_view arg_value(int argc, char** argv, std::string_view flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == flag) return argv[i + 1];
    }
    return {};
}

bool has_flag(int argc, char** argv, std::string_view flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == flag) return true;
    }
    return false;
}

int usage() {
    LOG_ERROR("usage: cuervo-editor --project <dir> [--scene <path>] [--backend gl|vulkan] [--gl-es] "
              "[--script-commands <file>] [--screenshot <file.ppm>] [--quit-after <seconds>] [--size <w>x<h>] "
              "[--font <file.ttf>] [--view <x>,<y>,<zoom>] [--jobs <n>] [--no-audio] [--tabs <Panel,...>] [--replay-verify <file.replay>] [--profile] "
              "[--theme light|dark] [--splash|--no-splash] [--accept-eula] [--layout <name>] [--screen eula|welcome] "
              "[--tool select|move|rotate|scale] [--select </a,/b>] [--palette [query]]\n"
              "       cuervo-editor --new-project <dir> [--template empty|walker] [--name <name>]");
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char** argv) {
    using namespace cuervo;
    editor::EditorOptions o;
    o.project = std::string(arg_value(argc, argv, "--project"));
    if (has_flag(argc, argv, "--help")) return usage();
    o.scene = std::string(arg_value(argc, argv, "--scene"));
    o.script_commands = std::string(arg_value(argc, argv, "--script-commands"));
    o.screenshot = std::string(arg_value(argc, argv, "--screenshot"));
    o.replay_verify = std::string(arg_value(argc, argv, "--replay-verify"));
    o.audio = !has_flag(argc, argv, "--no-audio");
    o.accept_eula = has_flag(argc, argv, "--accept-eula");
    o.screen = std::string(arg_value(argc, argv, "--screen"));
    o.layout = std::string(arg_value(argc, argv, "--layout"));
    if (o.screen != "" && o.screen != "eula" && o.screen != "welcome") {
        LOG_ERROR("Unknown --screen '{}' (expected eula or welcome)", o.screen);
        return EXIT_FAILURE;
    }
    if (has_flag(argc, argv, "--new-project")) {
        const std::string_view dir = arg_value(argc, argv, "--new-project");
        if (dir.empty()) {
            LOG_ERROR("--new-project needs the folder to create. Nothing was created. "
                      "Example: cuervo-editor --new-project mygame --template walker");
            return EXIT_FAILURE;
        }
        if (!eula::require_acceptance(argc, argv, "cuervo-editor")) return EXIT_FAILURE;
        const std::filesystem::path folder = std::filesystem::absolute(std::filesystem::path(std::string(dir)));
        std::string_view name = arg_value(argc, argv, "--name");
        const std::string fallback = folder.filename().string();
        if (name.empty()) name = fallback;
        std::string_view tmpl = arg_value(argc, argv, "--template");
        if (tmpl.empty()) tmpl = "empty";
        std::string error;
        if (!editor::create_project(tmpl, folder, name, error)) {
            LOG_ERROR("{}", error);
            return EXIT_FAILURE;
        }
        LOG_INFO("Created project '{}' ({} template) in {}", name, tmpl, folder.string());
        return EXIT_SUCCESS;
    }
    // Headless runs (no window to show the agreement in) follow the command-line rules.
    if (((!o.script_commands.empty() && o.screenshot.empty()) || !o.replay_verify.empty()) &&
        !eula::require_acceptance(argc, argv, "cuervo-editor")) {
        return EXIT_FAILURE;
    }
    // The Profiler panel starts recording (DESIGN.md 5.14).
    if (has_flag(argc, argv, "--profile")) profiler::set_enabled(true);
    if (const std::string_view jobs = arg_value(argc, argv, "--jobs"); !jobs.empty()) {
        const auto n = jobs::parse_thread_count(jobs);
        if (!n) {
            LOG_ERROR("--jobs needs a positive thread count, not '{}'", jobs);
            return EXIT_FAILURE;
        }
        o.jobs = *n;
    }
    const std::string_view backend = arg_value(argc, argv, "--backend");
    o.backend = rhi::backend_compiled_in(rhi::Backend::Vulkan) ? rhi::Backend::Vulkan : rhi::Backend::OpenGL;
    if (backend == "vulkan" || backend == "vk") {
        o.backend = rhi::Backend::Vulkan;
        o.allow_fallback = false;
    } else if (backend == "gl" || backend == "opengl") {
        o.backend = rhi::Backend::OpenGL;
        o.allow_fallback = false;
    } else if (!backend.empty()) {
        LOG_ERROR("Unknown --backend '{}' (expected vulkan or gl)", backend);
        return EXIT_FAILURE;
    }
    // Headless GL runs (CI) have no Vulkan surface; follow the runtime's habit.
    if (backend.empty() && platform::env_is("SDL_VIDEO_DRIVER", "offscreen")) o.backend = rhi::Backend::OpenGL;
    o.gl_es = has_flag(argc, argv, "--gl-es") || platform::env_is("CUERVO_GL_ES", "1");
    o.vsync = arg_value(argc, argv, "--vsync") != "off";
    if (const std::string_view q = arg_value(argc, argv, "--quit-after"); !q.empty()) o.quit_after = std::atof(q.data());
    if (const std::string_view s = arg_value(argc, argv, "--size"); !s.empty()) {
        const std::size_t x = s.find('x');
        int w = 0;
        int h = 0;
        if (x == std::string_view::npos || std::from_chars(s.data(), s.data() + x, w).ec != std::errc() ||
            std::from_chars(s.data() + x + 1, s.data() + s.size(), h).ec != std::errc() || w < 320 || h < 240) {
            LOG_ERROR("--size expects <width>x<height>, e.g. 1600x900");
            return EXIT_FAILURE;
        }
        o.width = w;
        o.height = h;
    }
    if (const std::string_view v = arg_value(argc, argv, "--view"); !v.empty()) {
        float xyz[3] = {0.0f, 0.0f, 1.0f};
        std::size_t pos = 0;
        for (int i = 0; i < 3; ++i) {
            std::size_t end = v.find(',', pos);
            if (end == std::string_view::npos) end = v.size();
            if (std::from_chars(v.data() + pos, v.data() + end, xyz[i]).ec != std::errc() || (i < 2 && end == v.size())) {
                LOG_ERROR("--view expects <x>,<y>,<zoom>, e.g. 640,150,4");
                return EXIT_FAILURE;
            }
            pos = end + 1;
        }
        o.view = vec3{xyz[0], xyz[1], xyz[2]};
    }
    for (std::string_view tabs = arg_value(argc, argv, "--tabs"); !tabs.empty();) {
        const std::size_t comma = tabs.find(',');
        o.tabs.emplace_back(tabs.substr(0, comma));
        tabs = comma == std::string_view::npos ? std::string_view{} : tabs.substr(comma + 1);
    }
    if (const std::string_view t = arg_value(argc, argv, "--theme"); !t.empty()) {
        brand::Theme theme{};
        if (!brand::parse_theme(t, theme)) {
            LOG_ERROR("Unknown --theme '{}' (expected light or dark)", t);
            return EXIT_FAILURE;
        }
        o.theme = theme;
    }
    if (const std::string_view tool = arg_value(argc, argv, "--tool"); !tool.empty()) {
        if (tool == "select") o.tool = editor::Tool::Select;
        else if (tool == "move") o.tool = editor::Tool::Move;
        else if (tool == "rotate") o.tool = editor::Tool::Rotate;
        else if (tool == "scale") o.tool = editor::Tool::Scale;
        else {
            LOG_ERROR("Unknown --tool '{}' (expected select, move, rotate or scale)", tool);
            return EXIT_FAILURE;
        }
    }
    for (std::string_view paths = arg_value(argc, argv, "--select"); !paths.empty();) {
        const std::size_t comma = paths.find(',');
        o.select_paths.emplace_back(paths.substr(0, comma));
        paths = comma == std::string_view::npos ? std::string_view{} : paths.substr(comma + 1);
    }
    // --palette [query]: the query is optional.
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--palette") {
            o.palette = (i + 1 < argc && std::string_view(argv[i + 1]).substr(0, 2) != "--") ? argv[i + 1] : "";
        }
    }
    if (has_flag(argc, argv, "--splash")) o.splash = editor::SplashMode::On;
    if (has_flag(argc, argv, "--no-splash")) o.splash = editor::SplashMode::Off;
    o.font_path = std::string(arg_value(argc, argv, "--font"));
    if (o.font_path.empty()) o.font_path = CUERVO_DEFAULT_FONT;
    return editor::run_editor(o);
}
