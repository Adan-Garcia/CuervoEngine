<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/brand/logo/mark-ember-dark.svg">
    <img src="assets/brand/logo/mark-ember.svg" alt="The Cuervo Mark" width="200">
  </picture>
</p>

# Cuervo Engine

A 2D, vector-first, deterministic game engine in C++20 with its own statically typed
scripting language, **Pluma** (`.plm` files). See [`docs/DESIGN.md`](docs/DESIGN.md) for the full design.

**Status: Beta (v0.1)** — the renderer has Vulkan and OpenGL/GLES/WebGL2 backends in
verified parity, with render tiers (`render_tier` in `project.toml`) and a migration report.
The engine includes the Pluma language and VM, deterministic collections and math/geometry/
random libraries, transform hierarchies and the node library, particles, collision, audio,
UI, binary scenes, the editor, exporting, and the shmup example running natively and in the
browser ([`docs/WEB.md`](docs/WEB.md)). Linux and web exports are verified; Windows is
portable by review but has not yet been built or run ([`docs/WINDOWS.md`](docs/WINDOWS.md)).

## Documentation

- [docs/GUIDE.md](docs/GUIDE.md): **start here**: install, build, and make a small game step
  by step (the finished game is [examples/tutorial](examples/tutorial)), then hot reload,
  the editor, debugging and exporting
- [docs/API.md](docs/API.md): the engine API for scripts (generated from the prelude by
  `cuervo-tools docs`)
- [docs/LANGUAGE.md](docs/LANGUAGE.md): the Pluma language
- [docs/SCENES.md](docs/SCENES.md): scene files and their text form
- [docs/REPLAYS.md](docs/REPLAYS.md): recording and verifying replays
- [docs/EXPORT.md](docs/EXPORT.md): packing and exporting games
- [docs/WEB.md](docs/WEB.md): building and serving WebGL2/WASM exports
- [docs/WINDOWS.md](docs/WINDOWS.md): Windows readiness and remaining verification
- [editor/README.md](editor/README.md): the editor
- [tools/vscode-pluma](tools/vscode-pluma/README.md): the VS Code extension (highlighting,
  language server, debugger)
- [docs/DESIGN.md](docs/DESIGN.md): architecture and design decisions

## Requirements

- CMake ≥ 3.24 and Ninja
- GCC 13+ or Clang 17+
- SDL3 (optional: if it isn't installed, CMake fetches and builds it)
  - Arch: `sudo pacman -S cmake ninja sdl3` (+ `vulkan-validation-layers` for debug builds)
  - Ubuntu 25.04+/Debian: `sudo apt install cmake ninja-build libsdl3-dev`
- A Vulkan 1.3 driver for the Vulkan backend (otherwise the runtime falls back to OpenGL)
- FreeType for font import (optional: if it isn't installed, CMake fetches and builds it;
  `-DCUERVO_FONT_IMPORT=OFF` drops font import and loads prebuilt `.efont` atlases only)
- Network access on first configure (fetches doctest, Vulkan headers, volk, VMA, SPIRV-Cross,
  libtess2, msdfgen, msdf-atlas-gen, Dear ImGui, and SDL3/FreeType if needed)

## License

Cuervo Engine is source-available under the [Cuervo Engine Fair-Code License Agreement
(EULA)](LICENSE.md): completely free during Beta for learning, modding, non-commercial forks,
and commercial games. Commercial licensing terms will be introduced upon the Version 1.0 release,
with full grandfathering for games built or released during Beta. **Read it before building: by building or using the Engine you agree to it.**
Third-party components keep their own licenses: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
and `third_party/licenses/`.

## Build and test

By building you accept the [EULA](LICENSE.md) (see License above).

```sh
cmake --preset debug          # or: release
cmake --build --preset debug
ctest --preset debug
```

The editor is enabled in desktop builds by default. Feature switches include
`CUERVO_FEATURE_AUDIO`, `CUERVO_FEATURE_UI`, `CUERVO_FEATURE_PARTICLES`,
`CUERVO_FEATURE_COLLISION`, `CUERVO_FEATURE_DEV`, `CUERVO_PROFILING`, and
`CUERVO_TRACY`; see `CMakeLists.txt` and [`docs/EXPORT.md`](docs/EXPORT.md) for stripped
players and release builds.

## Run

The first run of `cuervo`, `cuervo-editor`, `cuervo-tools`, or `plm` asks you to accept
the EULA. Use `--accept-eula` for scripted or headless runs; exported game players do not
ask.

```sh
./build/debug/runtime/cuervo --accept-eula --game examples/bench   # SDF shape stress test (Pluma); Space/Enter/Pad A cycles 1k/10k/50k/100k shapes, Esc quits
./build/debug/runtime/cuervo --game examples/bench --backend gl   # force a backend: vulkan | gl (default: Vulkan, falls back to GL)
./build/debug/runtime/cuervo --game examples/bench --quit-after 3 # exit after 3 seconds (smoke tests)
SDL_VIDEO_DRIVER=offscreen ./build/debug/runtime/cuervo --game examples/bench --quit-after 3   # headless (GL only: no headless Vulkan on NVIDIA)
./build/debug/runtime/cuervo --svg assets/test/clip.svg   # SVG viewer: wheel/+/- zoom, arrows pan, 0 resets
./build/debug/runtime/cuervo --svg in.svg --export out.evec  # import SVG to the binary ImportedVector format
./build/release/runtime/cuervo --game examples/bench --bench 10  # 50k animated SDF shapes, VSync off: logs frame-time stats (--bench works for any game)
./build/debug/runtime/cuervo --demo vector      # SVG character + MSDF text, zoom sweeps 1x..32x (--zoom Z fixes it)
./build/debug/runtime/cuervo --font in.ttf --export out.efont  # import a font to an MSDF atlas
./build/debug/runtime/cuervo --game examples/shmup  # the shmup stage, written in Pluma; edit its scripts while it runs (hot reload)
                                                 # title/pause/options/game-over menus (UI nodes + scene files); Esc/P pauses
./build/debug/runtime/cuervo --game examples/tutorial  # Starfall, the game docs/GUIDE.md builds
./build/debug/tools/cuervo_tools/cuervo-tools docs -o docs/API.md  # regenerate the API reference from the prelude
./build/debug/tools/cuervo_tools/cuervo-tools scene dump examples/shmup/scenes/title.scene  # scene file as text (docs/SCENES.md)
./build/debug/runtime/cuervo --game examples/shmup --backend gl --gl-es  # GL backend on an OpenGL ES 3.0 context (WebGL2's API)
./build/debug/runtime/cuervo --game examples/shmup --jobs 4  # job system threads (or CUERVO_JOBS / project.toml `jobs`); same result for any count
./build/debug/tools/cuervo_tools/cuervo-tools tier check examples/shmup [--to full]  # render-tier migration report
./build/debug/tools/rhi_parity/rhi_parity features --backend vulkan  # device capabilities and tiers
./build/debug/editor/cuervo-editor --accept-eula --project examples/editor_level  # the editor (editor/README.md): scene tree, view, inspector, undo
./build/debug/editor/cuervo-editor --project examples/shmup     # F5 plays the game in the editor; replays: docs/REPLAYS.md
./build/debug/runtime/cuervo --game examples/editor_level  # a level built in the editor without code (examples/editor_level/level.cmds)
./build/debug/pluma/plm --accept-eula      # Pluma REPL: type code, see the results
```

## Web build

See [`docs/WEB.md`](docs/WEB.md): `cmake --preset host-tools && cmake --build --preset host-tools`,
then with emsdk 6.0.12 active `cmake --preset web && cmake --build --preset web`, serve
`build/web/runtime/` and open `index.html`; `node tools/web/smoke_test.mjs` checks it headlessly.

## Layout

```
cmake/      compiler flags (determinism: no fast-math, no FMA contraction) and dependencies
cuervo/
  core/     logging, fixed-timestep loop, PCG32 RNG, profiling, interned names
  math/     Vec<T,N>, affine transforms, deterministic math (dmath)
  platform/ SDL3 window + event pump, frame loop (web-drivable), file watcher, platform helpers
  input/    device state, action/axis map, recordable/replayable input frames
  rhi/      render hardware interface; vulkan/ and gl/ backends
  render/   camera, SDF and mesh batchers, MSDF fonts/text, shaders
  vector/   SVG import, flattening, tessellation, stroking, ImportedVector format
  scene/    binary scenes, node hierarchy and scene editing
  particles/ deterministic CPU and visual particle systems
  collision/ collision world and spatial queries
  audio/    miniaudio mixer and sound playback
  jobs/     deterministic job-system integration
  script/   project loading, VM host, bindings and engine prelude
pluma/      Pluma language library (lexer, parser, checker, VM, AOT) + repl/ (`plm`) — never depends on cuervo/
runtime/    player executable (`cuervo`)
editor/     the editor (`cuervo-editor`, Dear ImGui on the RHI; desktop only)
tools/      cuervo-tools (pack/export, scenes, tiers, LSP, API docs), shader_tool, rhi_parity, web smoke test
tests/      doctest suites: cuervo_tests, pluma_tests, editor_tests
docs/       design documents
```
