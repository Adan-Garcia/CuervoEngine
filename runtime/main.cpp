// The Cuervo runtime: runs script projects and exported games, plus the
// engine's own demos and import tools.
//
//   cuervo --game <dir> [...]   (a script project; see game_cli.hpp)
//   cuervo --dap                (a debug adapter on stdio for VS Code; see dap_cli.hpp)
//   cuervo --pack <game.pak> [...] (an exported game; docs/EXPORT.md). With no
//   mode flags, a `game.pak` next to the executable runs (exported builds).
//   cuervo --demo vector [--zoom <z>] [--svg <file>] [--font <file>] [--screenshot <file.ppm>]
//   cuervo --demo particles [--bench <s>] [--intensity <x>] [--font <file>] [--screenshot <file.ppm>]
//   cuervo --svg <file.svg|file.evec> [--zoom <z>] [--instances <N>] [--animate-zoom] [...]
//   cuervo --svg <file.svg> --export <file.evec>   (import to the binary format, no window)
//   cuervo --font <file.ttf> --export <file.efont>  (import a font atlas, no window)
//   Shared: [--backend vulkan|gl] [--quit-after <seconds>] [--vsync on|off] [--msaa 1|2|4|8]
//           [--scale fit|integer|stretch] [--screenshot <file.ppm>] [--gl-es]
//   --jobs <N>: job system threads (default: CUERVO_JOBS, else auto; DESIGN.md 4.15)
//   --simd off: run the scalar loops instead of the SIMD kernels (same results; for comparisons)
//
// --svg shows a vector file (svg_viewer.hpp).
// --demo vector shows an SVG character with MSDF text at any zoom (vector_demo.hpp).
// --demo particles runs a bullet-hell pattern on cuervo/particles (particles_demo.hpp).
//
// The SDF shape stress test / milestone 2 benchmark ("50k SDF shapes at 60
// fps", DESIGN.md 6) is a script project: `cuervo --game examples/bench
// [--bench <seconds>]` (examples/bench/project.toml).
//
// --screenshot (or the CUERVO_SCREENSHOT=<file.ppm> environment variable)
// simulates exactly one tick per frame up to the capture tick (independent of
// timing), draws that state, writes it as a PPM and exits, so both backends
// produce directly comparable images. The capture tick defaults to 240;
// CUERVO_SCREENSHOT_TICK=<n> overrides it (0 = the very first rendered frame).

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "cuervo/core/log.hpp"
#include "cuervo/jobs/job_system.hpp"
#include "cuervo/math/simd.hpp"
#include "cuervo/platform/eula.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/platform/window.hpp"
#include "cuervo/render/camera.hpp"
#include "cuervo/render/font_import.hpp"
#include "cuervo/rhi/rhi.hpp"
#include "cuervo/vector/imported_vector.hpp"
#include "dap_cli.hpp"
#include "game_cli.hpp"
#if CUERVO_FEATURE_DEV
#include "particles_demo.hpp"
#include "svg_viewer.hpp"
#include "vector_demo.hpp"
#endif

using namespace cuervo;

namespace {

constexpr std::uint64_t default_screenshot_tick = 240;

bool has_flag(int argc, char** argv, std::string_view flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == flag) {
            return true;
        }
    }
    return false;
}

std::string_view arg_value(int argc, char** argv, std::string_view flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == flag) {
            return argv[i + 1];
        }
    }
    return {};
}

GraphicsApi graphics_api_for(rhi::Backend backend) {
    return backend == rhi::Backend::Vulkan ? GraphicsApi::Vulkan : GraphicsApi::OpenGL;
}

// Creates the window and device together, since the window must be created
// for a specific graphics API.
std::unique_ptr<rhi::Device> open_device(Window& window, rhi::Backend backend, const rhi::DeviceOptions& options,
                                         const std::string& title, bool gl_es) {
    if (!window.create({.title = title, .graphics = graphics_api_for(backend), .gl_es = gl_es})) {
        return nullptr;
    }
    auto device = rhi::create_device(backend, window, options);
    if (!device) {
        window.destroy();
    }
    return device;
}


render::ScaleMode parse_scale(std::string_view arg) {
    if (arg == "integer") return render::ScaleMode::Integer;
    if (arg == "stretch") return render::ScaleMode::Stretch;
    if (!arg.empty() && arg != "fit") LOG_WARN("Unknown --scale '{}', using fit", arg);
    return render::ScaleMode::Fit;
}

} // namespace

int main(int argc, char** argv) {
    static std::vector<std::string> game_args;
    static std::vector<char*> game_argv;
    auto prepend = [&](std::initializer_list<std::string> first) {
        game_args = first;
        for (int i = 1; i < argc; ++i) game_args.emplace_back(argv[i]);
        for (std::string& a : game_args) game_argv.push_back(a.data());
        game_argv.push_back(nullptr);
        argc = static_cast<int>(game_args.size());
        argv = game_argv.data();
    };
#if defined(__EMSCRIPTEN__)
    // The web build runs the one preloaded game (docs/WEB.md): its game and
    // font arguments come first, then whatever the page passes (shell.html
    // `?args=`). Demos use blocking loops and are desktop-only.
#if defined(CUERVO_WEB_PACK)
    prepend({"cuervo", "--pack", CUERVO_WEB_PACK, "--backend", "gl"}); // an exported pack (docs/EXPORT.md)
#else
    prepend({"cuervo", "--game", CUERVO_WEB_GAME_DIR, "--font", CUERVO_WEB_FONT, "--backend", "gl"});
#endif
#else
    // An exported game (docs/EXPORT.md): `game.pak` next to the executable
    // runs when no other mode is asked for; --pack/--game override it.
    if (!has_flag(argc, argv, "--game") && !has_flag(argc, argv, "--pack") && !has_flag(argc, argv, "--demo") &&
        !has_flag(argc, argv, "--svg") && !has_flag(argc, argv, "--export") && !has_flag(argc, argv, "--bench")) {
        const std::filesystem::path pak = platform::executable_dir() / "game.pak";
        std::error_code ec;
        if (std::filesystem::is_regular_file(pak, ec)) prepend({"cuervo", "--pack", platform::utf8(pak)});
    }
#endif
#if !defined(__EMSCRIPTEN__)
    // The EULA gate (DESIGN.md 4.16): every use of the engine but running a pack. An exported game's
    // players aren't Engine licensees, so --pack (and a game.pak next to the player) never asks.
    if (!has_flag(argc, argv, "--pack") && !cuervo::eula::require_acceptance(argc, argv, "cuervo")) return EXIT_FAILURE;
    if (has_flag(argc, argv, "--dap")) return run_dap(argc, argv); // a debug adapter: dap_cli.hpp
#endif
    if (has_flag(argc, argv, "--game") || has_flag(argc, argv, "--pack")) {
        return run_game_cli(argc, argv); // script projects and exported games: game_cli.cpp
    }
    std::optional<unsigned> jobs_arg;
    if (const std::string_view jobs = arg_value(argc, argv, "--jobs"); !jobs.empty()) {
        jobs_arg = jobs::parse_thread_count(jobs);
        if (!jobs_arg) {
            LOG_ERROR("--jobs needs a positive thread count, not '{}'", jobs);
            return EXIT_FAILURE;
        }
    }
    if (arg_value(argc, argv, "--simd") == "off") simd::set_enabled(false);
    jobs::configure(jobs::resolve_thread_count(jobs_arg, std::nullopt));
    const std::string_view quit_arg = arg_value(argc, argv, "--quit-after");
    const double quit_after = quit_arg.empty() ? 0.0 : std::atof(quit_arg.data());
    double bench_seconds = 0.0; // --demo particles --bench
    if (const std::string_view bench_arg = arg_value(argc, argv, "--bench"); !bench_arg.empty()) {
        bench_seconds = std::max(0.5, std::atof(bench_arg.data()));
    }
    std::string font_path(arg_value(argc, argv, "--font"));
    if (font_path.empty()) {
        font_path = platform::utf8(platform::find_asset("fonts/LiberationSans-Regular.ttf", CUERVO_DEFAULT_FONT));
    }
    std::string screenshot_path(arg_value(argc, argv, "--screenshot"));
    if (auto env = platform::env("CUERVO_SCREENSHOT"); screenshot_path.empty() && env) {
        screenshot_path = *env;
    }
    std::uint64_t screenshot_tick = default_screenshot_tick;
    if (auto env = platform::env("CUERVO_SCREENSHOT_TICK"); env && !env->empty()) {
        screenshot_tick = std::strtoull(env->c_str(), nullptr, 10);
    }
    const render::ScaleMode scale_mode = parse_scale(arg_value(argc, argv, "--scale"));
    rhi::DeviceOptions options;
    // Demos and benchmarks are engine tools, not projects: no tier gate.
    options.tier = rhi::Tier::Full;
    const bool gl_es = has_flag(argc, argv, "--gl-es") || platform::env_is("CUERVO_GL_ES", "1");
    options.vsync = arg_value(argc, argv, "--vsync") != "off";
    if (bench_seconds > 0.0) options.vsync = false; // bench: uncapped
    if (const std::string_view msaa = arg_value(argc, argv, "--msaa"); !msaa.empty()) {
        options.msaa_samples = static_cast<std::uint32_t>(std::max(1, std::atoi(msaa.data())));
    }

    const std::string_view demo = arg_value(argc, argv, "--demo");
    if (!demo.empty() && demo != "vector" && demo != "particles") {
        LOG_ERROR("Unknown --demo '{}' (expected vector or particles)", demo);
        return EXIT_FAILURE;
    }
    const std::string svg_path(arg_value(argc, argv, "--svg"));
    const std::string export_path(arg_value(argc, argv, "--export"));
    if (svg_path.empty() && !export_path.empty()) {
        // Font import step (DESIGN.md 4.11): TTF/OTF -> .efont MSDF atlas.
        const auto font = render::load_font_file(font_path);
        if (!font || !vector::write_file(export_path, render::serialize_font_atlas(*font))) {
            return EXIT_FAILURE;
        }
        LOG_INFO("Imported '{}' -> '{}' ({} glyphs, {} kerning pairs, {}x{} atlas)", font_path, export_path,
                 font->glyphs.size(), font->kerning.size(), font->width, font->height);
        return EXIT_SUCCESS;
    }
    if (!svg_path.empty() && !export_path.empty() && demo.empty()) {
        // Import step (DESIGN.md 4.11): SVG -> ImportedVector, no window needed.
        std::vector<std::byte> text;
        if (!vector::read_file(svg_path, text)) {
            return EXIT_FAILURE;
        }
        auto imported =
            vector::import_svg(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), svg_path);
        if (!imported || !vector::write_file(export_path, vector::serialize_imported_vector(*imported))) {
            return EXIT_FAILURE;
        }
        LOG_INFO("Imported '{}' -> '{}' ({} shapes, base mesh {} triangles)", svg_path, export_path,
                 imported->document.shapes.size(), imported->base_mesh->mesh.triangle_count());
        return EXIT_SUCCESS;
    }

    if (demo.empty() && svg_path.empty()) {
        LOG_ERROR("Nothing to run: use --game <dir>, --pack <file.pak>, --demo vector|particles or --svg <file> "
                  "(the SDF stress test is `--game examples/bench`)");
        return EXIT_FAILURE;
    }

    const std::string_view backend_arg = arg_value(argc, argv, "--backend");
    rhi::Backend backend = rhi::backend_compiled_in(rhi::Backend::Vulkan) ? rhi::Backend::Vulkan : rhi::Backend::OpenGL;
    bool allow_fallback = true;
    if (backend_arg == "vulkan" || backend_arg == "vk") {
        backend = rhi::Backend::Vulkan;
        allow_fallback = false;
    } else if (backend_arg == "gl" || backend_arg == "opengl") {
        backend = rhi::Backend::OpenGL;
        allow_fallback = false;
    } else if (!backend_arg.empty()) {
        LOG_ERROR("Unknown --backend '{}' (expected vulkan or gl)", backend_arg);
        return EXIT_FAILURE;
    }

    const std::string title =
        !demo.empty() ? "Cuervo Engine — " + std::string(demo) + " demo" : "Cuervo Engine — " + svg_path;
    Window window;
    auto device = open_device(window, backend, options, title, gl_es);
    if (!device && allow_fallback && backend == rhi::Backend::Vulkan) {
        LOG_WARN("Vulkan unavailable, falling back to OpenGL");
        device = open_device(window, rhi::Backend::OpenGL, options, title, gl_es);
    }
    if (!device) {
        return EXIT_FAILURE;
    }

#if CUERVO_FEATURE_DEV
    if (demo == "particles") {
        ParticlesDemoOptions demo_options;
        demo_options.font_path = has_flag(argc, argv, "--no-hud") ? std::string() : font_path;
        demo_options.screenshot = screenshot_path;
        demo_options.screenshot_tick = screenshot_tick;
        demo_options.quit_after = quit_after;
        demo_options.bench_seconds = bench_seconds;
        demo_options.scale = scale_mode;
        if (const std::string_view intensity = arg_value(argc, argv, "--intensity"); !intensity.empty()) {
            demo_options.intensity = std::max(0.05f, static_cast<float>(std::atof(intensity.data())));
        }
        const int result = run_particles_demo(window, *device, demo_options);
        device.reset();
        window.destroy();
        return result;
    }

    if (!demo.empty()) {
        VectorDemoOptions demo_options;
        demo_options.svg_path = svg_path.empty()
                                    ? platform::utf8(platform::find_asset("demo/robot.svg", CUERVO_DEMO_SVG))
                                    : svg_path;
        demo_options.font_path = font_path;
        demo_options.screenshot = screenshot_path;
        demo_options.quit_after = quit_after;
        demo_options.scale = scale_mode;
        const std::string_view zoom = arg_value(argc, argv, "--zoom");
        demo_options.zoom = zoom.empty() ? 0.0f : static_cast<float>(std::atof(zoom.data()));
        const int result = run_vector_demo(window, *device, demo_options);
        device.reset();
        window.destroy();
        return result;
    }

    {
        SvgViewerOptions viewer;
        viewer.path = svg_path;
        viewer.screenshot = screenshot_path;
        viewer.quit_after = quit_after;
        viewer.scale = scale_mode;
        const std::string_view zoom = arg_value(argc, argv, "--zoom");
        viewer.zoom = zoom.empty() ? 0.0f : static_cast<float>(std::atof(zoom.data()));
        const std::string_view instances = arg_value(argc, argv, "--instances");
        viewer.instances = instances.empty() ? 1 : std::max(1, std::atoi(instances.data()));
        viewer.animate_zoom = has_flag(argc, argv, "--animate-zoom");
        const int result = run_svg_viewer(window, *device, viewer);
        device.reset();
        window.destroy();
        return result;
    }

#else
    (void)scale_mode;
    (void)quit_after;
    (void)bench_seconds;
    (void)screenshot_path;
    (void)screenshot_tick;
    (void)font_path;
    device.reset();
    window.destroy();
    LOG_ERROR("Demos and dev tools are disabled in this stripped build; run a game with --game <dir> or --pack <file.pak>");
    return EXIT_FAILURE;
#endif
}
