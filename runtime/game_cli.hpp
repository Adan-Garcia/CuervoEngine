#pragma once

// `cuervo --game <dir>`: runs a script project (cuervo/script/game_runner.hpp).
// `cuervo --pack <game.pak>`: runs an exported game (docs/EXPORT.md); an
// exported build also finds `game.pak` next to its executable by itself.
//
//   cuervo --game <dir> | --pack <file.pak> [--backend vulkan|gl] [--quit-after <seconds>] [--vsync on|off]
//                        [--screenshot <file.ppm>] [--font <file>] [--scale fit|integer|stretch]
//                        [--no-hot-reload] [--press <tick:action,...>] [--pointer <tick:x:y,...>]
//                        [--type <tick:text,...>] [--gl-es] [--no-audio]
//                        [--record <file.replay>] [--bench <seconds> [--bench-warmup <seconds>]]
//                        [--profile]
//
// --screenshot simulates one tick per frame up to the capture tick (240, or
// CUERVO_SCREENSHOT_TICK), writes that frame and exits, so captures are
// reproducible and comparable across backends. --press (or CUERVO_PRESS)
// holds project actions on given ticks, e.g. "60:ui_accept,300:pause", so
// automated runs can reach menus ("10-20:ui_click" holds for a range of ticks).
// --pointer ("tick:x:y") puts the pointer somewhere from that tick on, and --type
// ("tick:text") types text into a focused text field, a character per tick. --gl-es (or CUERVO_GL_ES=1) asks the GL
// backend for an OpenGL ES 3.0 context, the API WebGL2 exposes, so desktop
// runs exercise the web's shaders (GLSL ES 300) and limits.
// --record writes the run as a replay (every tick's input and state hash,
// docs/REPLAYS.md) when the game ends; `cuervo-editor --replay-verify` checks it.
// --bench turns VSync off, runs the game for --bench-warmup seconds (default
// 3), then logs frame-time stats (avg / p50 / p95 / p99 / max, sim and render
// CPU time) over the given seconds and exits; examples/bench is the SDF stress
// scene made for it.
// --profile records engine zones (cuervo/core/profiler.hpp) and profiles the
// scripts (pluma/profiler.hpp), then logs both tables at exit ("profile |"
// lines); it observes only, so the state hash is unchanged (DESIGN.md 5.14).
int run_game_cli(int argc, char** argv);
