#include "game_cli.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>

#include "cuervo/core/log.hpp"
#include "cuervo/jobs/job_system.hpp"
#include "cuervo/math/simd.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/script/game_runner.hpp"

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

} // namespace

int run_game_cli(int argc, char** argv) {
    using namespace cuervo;
    script::GameOptions o;
    o.dir = std::string(arg_value(argc, argv, "--game"));
    o.pack = std::string(arg_value(argc, argv, "--pack"));
    if (o.dir.empty() && o.pack.empty()) {
        LOG_ERROR("{}", has_flag(argc, argv, "--pack") ? "--pack needs a .pak file" : "--game needs a project directory");
        return EXIT_FAILURE;
    }
    if (!o.dir.empty() && !o.pack.empty()) {
        LOG_ERROR("--game and --pack are exclusive (a project directory or an exported pack)");
        return EXIT_FAILURE;
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
    if (const std::string_view q = arg_value(argc, argv, "--quit-after"); !q.empty()) o.quit_after = std::atof(q.data());
    o.vsync = arg_value(argc, argv, "--vsync") != "off";
    if (const std::string_view b = arg_value(argc, argv, "--bench"); !b.empty()) {
        o.bench = std::max(0.5, std::atof(b.data()));
        o.vsync = false; // uncapped: measure the engine, not the display
    }
    if (const std::string_view w = arg_value(argc, argv, "--bench-warmup"); !w.empty()) {
        o.bench_warmup = std::max(0.0, std::atof(w.data()));
    }
    o.screenshot = std::string(arg_value(argc, argv, "--screenshot"));
    if (auto env = platform::env("CUERVO_SCREENSHOT"); o.screenshot.empty() && env) o.screenshot = *env;
    if (auto env = platform::env("CUERVO_SCREENSHOT_TICK"); env && !env->empty()) {
        o.screenshot_tick = std::strtoull(env->c_str(), nullptr, 10);
    }
    o.presses = std::string(arg_value(argc, argv, "--press"));
    if (auto env = platform::env("CUERVO_PRESS"); o.presses.empty() && env) o.presses = *env;
    o.gl_es = has_flag(argc, argv, "--gl-es") || platform::env_is("CUERVO_GL_ES", "1");
    if (arg_value(argc, argv, "--simd") == "off") simd::set_enabled(false); // scalar loops, same results
    if (const std::string_view jobs = arg_value(argc, argv, "--jobs"); !jobs.empty()) {
        const auto n = jobs::parse_thread_count(jobs);
        if (!n) {
            LOG_ERROR("--jobs needs a positive thread count, not '{}'", jobs);
            return EXIT_FAILURE;
        }
        o.jobs = *n;
    }
    o.font_path = std::string(arg_value(argc, argv, "--font"));
    // A pack carries its font (.cuervo/default.efont); a project uses the
    // engine's default font unless --font says otherwise.
    if (o.font_path.empty() && o.pack.empty()) o.font_path = CUERVO_DEFAULT_FONT;
    o.hot_reload = !has_flag(argc, argv, "--no-hot-reload");
    o.record = std::string(arg_value(argc, argv, "--record"));
    o.audio = !has_flag(argc, argv, "--no-audio");
    o.profile = has_flag(argc, argv, "--profile"); // engine zones + script profiler, summary at exit
    const std::string_view scale = arg_value(argc, argv, "--scale");
    if (scale == "integer") o.scale = render::ScaleMode::Integer;
    if (scale == "stretch") o.scale = render::ScaleMode::Stretch;
    return script::run_game(o);
}
