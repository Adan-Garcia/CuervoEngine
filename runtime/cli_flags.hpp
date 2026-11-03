#pragma once

// Every flag and environment variable `cuervo` reads: the single table behind
// `cuervo --help`, docs/CLI.md and tests/cuervo/cli_table_tests.cpp. Add a flag
// to a parser (main.cpp, game_cli.cpp, the demos) and to this table together;
// the parsers read flags through cli::Args, which stops on a name missing here.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

#include "runtime/cli_table.hpp"

namespace cuervo::cli {

inline constexpr std::uint64_t default_screenshot_tick = 240;

inline constexpr Flag runtime_flag_list[] = {
    {"--game", "", "<dir>", "Run the script project in <dir> (it holds project.toml).", "Run a game"},
    {"--pack", "", "<game.pak>", "Run an exported game; a game.pak next to the executable runs by itself.", "Run a game"},
    {"--dap", "", "", "Serve the Debug Adapter Protocol on stdin/stdout (the VS Code extension starts this).", "Run a game"},
    {"--backend", "", "vulkan|gl|webgpu", "Graphics backend. Without it: the platform default, with a fallback to GL.", "Run a game"},
    {"--quit-after", "", "<seconds>", "Exit after this many seconds (0 = run until closed).", "Run a game"},
    {"--vsync", "", "on|off", "VSync (default on).", "Run a game"},
    {"--msaa", "", "1|2|4|8", "Multisample count for the demos and the SVG viewer.", "Run a game"},
    {"--scale", "", "fit|integer|stretch", "How the virtual screen fills the window (default fit).", "Run a game"},
    {"--font", "", "<file>", "Font for text (TTF/OTF or .efont); with --export, the font to import.", "Run a game"},
    {"--gl-es", "", "", "Ask the GL backend for an OpenGL ES 3.0 context (WebGL2's API).", "Run a game"},
    {"--no-audio", "", "", "Use the null audio backend: same triggers, no sound.", "Run a game"},
    {"--no-hot-reload", "", "", "Do not watch the scripts for changes.", "Run a game"},
    {"--jobs", "", "<N>", "Job system threads (default: CUERVO_JOBS, project.toml, else auto). The result is the same for every N.", "Run a game"},
    {"--simd", "", "off", "Run the scalar loops instead of the SIMD kernels (same results; for comparisons).", "Run a game"},
    {"--record", "", "<file.replay>", "Write every tick's input and state hash as a replay when the game ends.", "Run a game"},
    {"--net-host", "", "<port>", "Network play: wait for one peer on this UDP port and be player 0 (0 picks a free port and logs it).", "Run a game"},
    {"--net-join", "", "<host:port>", "Network play: join a --net-host game over UDP and be player 1.", "Run a game"},
    {"--net-mesh", "", "<host:port,...>", "Network play over UDP with 2 to 4 players and no server: every player's address in player order (the same list on every machine); needs --net-player.", "Run a game"},
    {"--net-player", "", "<i>", "Our index in the --net-mesh list (0-based).", "Run a game"},
    {"--net-relay", "", "<ws-url>", "Network play through a WebSocket relay (tools/net/relay.py); needs --net-room. Players are numbered in join order.", "Run a game"},
    {"--net-room", "", "<name>", "The relay room to join with --net-relay.", "Run a game"},
    {"--net-players", "", "<n>", "Players in a relay room (2 to 8, default 2).", "Run a game"},
    {"--net-mode", "", "lockstep|rollback", "Network play: wait for every player's input (default) or predict and roll back.", "Run a game"},
    {"--profile", "", "", "Record engine zones and script functions and log both tables at exit.", "Run a game"},
    {"--trace", "", "<file.json>", "Like --profile, and write a Chrome trace (zones, job tasks, draw calls, Pluma heap) to the file at exit; open it in ui.perfetto.dev.", "Run a game"},
    {"--bench", "", "<seconds>", "Turn VSync off, warm up, then log frame-time stats over <seconds> and exit.", "Run a game"},
    {"--bench-warmup", "", "<seconds>", "Warm-up before --bench measures (default 3).", "Run a game"},

    {"--screenshot", "", "<file.ppm>", "Simulate one tick per frame up to the capture tick, write that frame as a PPM and exit.", "Automation"},
    {"--screenshot-tick", "", "<n>", "The capture tick (default 240; 0 = the first rendered frame). Wins over CUERVO_SCREENSHOT_TICK.", "Automation"},
    {"--press", "", "<list>", "Hold actions or push axes on given ticks: \"60:ui_accept,10-20:ui_click,60-300:move=1,0\". A tick is a number or a first-last range; action = name of an action, axis = name=x,y with values in [-1, 1].", "Automation"},
    {"--pointer", "", "<tick:x:y,...>", "Put the pointer at virtual pixel (x, y) from that tick on.", "Automation"},
    {"--type", "", "<tick:text,...>", "Type text into a focused text field, one character per tick from that tick.", "Automation"},

    {"--demo", "", "vector|particles", "Run an engine demo.", "Demos and importers"},
    {"--zoom", "", "<z>", "Start zoom of the vector demo or the SVG viewer.", "Demos and importers"},
    {"--intensity", "", "<x>", "Particles demo: bullet density multiplier.", "Demos and importers"},
    {"--no-hud", "", "", "Particles demo: draw no text.", "Demos and importers"},
    {"--svg", "", "<file.svg|file.evec>", "Show a vector file in the SVG viewer.", "Demos and importers"},
    {"--instances", "", "<N>", "SVG viewer: draw N copies.", "Demos and importers"},
    {"--animate-zoom", "", "", "SVG viewer: zoom in and out continuously.", "Demos and importers"},
    {"--export", "", "<file.evec|file.efont>", "With --svg: import to the binary format; with --font: import a font atlas. No window.", "Demos and importers"},

    {"--help", "-h", "", "Print this reference and exit.", "Help"},

    {"CUERVO_SCREENSHOT", "", "<file.ppm>", "Same as --screenshot.", "Environment"},
    {"CUERVO_SCREENSHOT_TICK", "", "<n>", "Capture tick for screenshots (default 240); --screenshot-tick overrides it.", "Environment"},
    {"CUERVO_PRESS", "", "<list>", "Same as --press when the flag is absent.", "Environment"},
    {"CUERVO_GL_ES", "", "1", "Same as --gl-es.", "Environment"},
    {"CUERVO_JOBS", "", "<N>", "Same as --jobs when the flag is absent.", "Environment"},
    {"CUERVO_AUDIO", "", "null", "Use the null audio backend (also implied by the offscreen video driver).", "Environment"},
    {"SDL_VIDEO_DRIVER", "", "offscreen", "SDL's video driver; offscreen gives a headless GL run.", "Environment"},
};

inline std::span<const Flag> runtime_table() { return runtime_flag_list; }

inline constexpr std::string_view exit_codes_text =
    "0 ok, 1 any other failure (including a replay mismatch), 2 the scripts did not compile, 3 a script panicked";

inline std::string runtime_help() {
    std::string out =
        "cuervo: the Cuervo Engine runtime\n\n"
        "usage:\n"
        "  cuervo --game <dir> | --pack <game.pak> [options]\n"
        "  cuervo --demo vector|particles [options]\n"
        "  cuervo --svg <file> [options]\n"
        "  cuervo --svg <file.svg> --export <file.evec>\n"
        "  cuervo --font <file.ttf> --export <file.efont>\n"
        "  cuervo --dap\n";
    out += help_text(runtime_table());
    out += "\nScreenshots: the capture tick defaults to " + std::to_string(default_screenshot_tick) +
           " (--screenshot-tick or CUERVO_SCREENSHOT_TICK change it).\n"
           "Exit codes: " + std::string(exit_codes_text) + ".\n"
           "docs/CLI.md has the same reference.\n";
    return out;
}

// The command-line arguments, read through the table: asking for a flag the
// table doesn't list is a programming error, which stops the run.
class Args {
public:
    Args(int argc, char** argv) : argc_(argc), argv_(argv) {}

    bool has(std::string_view flag) const {
        check(flag);
        for (int i = 1; i < argc_; ++i) {
            if (std::string_view(argv_[i]) == flag) return true;
        }
        return false;
    }
    std::string_view value(std::string_view flag) const {
        check(flag);
        for (int i = 1; i + 1 < argc_; ++i) {
            if (std::string_view(argv_[i]) == flag) return argv_[i + 1];
        }
        return {};
    }
    // The first "--x" argument the table doesn't know (empty if none).
    std::string_view unknown() const {
        for (int i = 1; i < argc_; ++i) {
            const std::string_view a = argv_[i];
            if (a.starts_with("--") && find(runtime_table(), a) == nullptr) return a;
        }
        return {};
    }

private:
    static void check(std::string_view flag) {
        if (find(runtime_table(), flag) == nullptr) {
            std::fprintf(stderr, "cuervo: internal error: flag '%.*s' is missing from runtime/cli_flags.hpp\n",
                         static_cast<int>(flag.size()), flag.data());
            std::abort();
        }
    }
    int argc_;
    char** argv_;
};

} // namespace cuervo::cli
