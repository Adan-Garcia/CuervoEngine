#include "game_cli.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>

#include "cuervo/core/log.hpp"
#include "runtime/cli_flags.hpp"
#include "cuervo/jobs/job_system.hpp"
#include "cuervo/math/simd.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/script/game_runner.hpp"

namespace {

// Flags are read through the table (runtime/cli_flags.hpp), so one that isn't listed can't be handled.
bool has_flag(int argc, char** argv, std::string_view flag) { return cuervo::cli::Args(argc, argv).has(flag); }

std::string_view arg_value(int argc, char** argv, std::string_view flag) {
    return cuervo::cli::Args(argc, argv).value(flag);
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
    o.backend = rhi::default_backend();
    if (!backend.empty()) {
        if (!rhi::parse_backend(backend, o.backend)) {
            LOG_ERROR("Unknown --backend '{}' (expected vulkan, gl or webgpu)", backend);
            return EXIT_FAILURE;
        }
        o.allow_fallback = false;
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
    // The flag wins over the variable: a browser page has no environment (tools/bench.py --web).
    if (const std::string_view t = arg_value(argc, argv, "--screenshot-tick"); !t.empty()) {
        o.screenshot_tick = std::strtoull(std::string(t).c_str(), nullptr, 10);
    }
    o.presses = std::string(arg_value(argc, argv, "--press"));
    if (auto env = platform::env("CUERVO_PRESS"); o.presses.empty() && env) o.presses = *env;
    o.pointers = std::string(arg_value(argc, argv, "--pointer"));
    o.typed = std::string(arg_value(argc, argv, "--type"));
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
    if (o.font_path.empty() && o.pack.empty()) {
        o.font_path = platform::utf8(platform::find_asset("fonts/LiberationSans-Regular.ttf", CUERVO_DEFAULT_FONT));
    }
    o.hot_reload = !has_flag(argc, argv, "--no-hot-reload");
    o.record = std::string(arg_value(argc, argv, "--record"));
    o.audio = !has_flag(argc, argv, "--no-audio");
    if (const std::string_view v = arg_value(argc, argv, "--net-host"); !v.empty()) {
        char* end = nullptr;
        const long port = std::strtol(std::string(v).c_str(), &end, 10);
        if (*end != '\0' || port < 0 || port > 65535) {
            LOG_ERROR("--net-host needs a UDP port (0 picks one), not '{}'", v);
            return EXIT_FAILURE;
        }
        o.net_host = static_cast<int>(port);
    }
    o.net_join = std::string(arg_value(argc, argv, "--net-join"));
    o.net_relay = std::string(arg_value(argc, argv, "--net-relay"));
    o.net_mesh = std::string(arg_value(argc, argv, "--net-mesh"));
    if (const std::string_view v = arg_value(argc, argv, "--net-player"); !v.empty()) {
        const long n = std::strtol(std::string(v).c_str(), nullptr, 10);
        if (n < 0 || n > 3) {
            LOG_ERROR("--net-player needs 0 to 3, not '{}'", v);
            return EXIT_FAILURE;
        }
        o.net_player = static_cast<unsigned>(n);
    }
    o.net_room = std::string(arg_value(argc, argv, "--net-room"));
    o.net_mode = std::string(arg_value(argc, argv, "--net-mode"));
    if (!o.net_mode.empty() && o.net_mode != "lockstep" && o.net_mode != "rollback") {
        LOG_ERROR("Unknown --net-mode '{}' (expected lockstep or rollback)", o.net_mode);
        return EXIT_FAILURE;
    }
    if (const std::string_view v = arg_value(argc, argv, "--net-players"); !v.empty()) {
        const long n = std::strtol(std::string(v).c_str(), nullptr, 10);
        if (n < 2 || n > 8) {
            LOG_ERROR("--net-players needs 2 to 8, not '{}'", v);
            return EXIT_FAILURE;
        }
        o.net_players = static_cast<unsigned>(n);
    }
    if ((o.net_host >= 0) + !o.net_join.empty() + !o.net_relay.empty() + !o.net_mesh.empty() > 1 ||
        (!o.net_relay.empty() && o.net_room.empty())) {
        LOG_ERROR("Use one of --net-host <port>, --net-join <host:port> or --net-relay <ws-url> --net-room <name> or --net-mesh <list> --net-player <i>");
        return EXIT_FAILURE;
    }
    o.profile = has_flag(argc, argv, "--profile"); // engine zones + script profiler, summary at exit
    o.trace = std::string(arg_value(argc, argv, "--trace")); // Chrome trace of the same, written at exit
    if (!o.trace.empty()) o.profile = true;
    const std::string_view scale = arg_value(argc, argv, "--scale");
    if (scale == "integer") o.scale = render::ScaleMode::Integer;
    if (scale == "stretch") o.scale = render::ScaleMode::Stretch;
    return script::run_game(o);
}
