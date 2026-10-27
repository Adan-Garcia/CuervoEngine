# Web export (WebGPU / WebGL2 / WASM)

The engine runs a script project in the browser through Emscripten: it renders
with WebGPU where the browser has it and otherwise with the GL backend, whose
GLES 3.0 path *is* WebGL2 (see "WebGPU"); the frame loop is driven by the browser,
and the project is preloaded into an in-memory file system. Only projects with
`render_tier = "compatible"` can be exported; a `full` project is refused ([STATUS.md](STATUS.md) "Render tiers").

Status: **built and verified** with the pinned toolchain below — the
shmup (`examples/shmup`) builds to WASM, loads, renders and plays in headless
Chrome (SwiftShader WebGL2), and its simulation is bit-identical to the native
build at tick 240 (same state hash).

## Pinned toolchain

| Tool | Version | Notes |
|---|---|---|
| emsdk / Emscripten | **6.0.12** (`emcc` commit `5488e0871d46d19a3209988190fa6a653126256f`) | `./emsdk install 6.0.12 && ./emsdk activate 6.0.12` |
| Node (emsdk's) | 24.19.0 | runs the smoke test (needs Node 22+ for the global `WebSocket`) |
| SDL3 | release-3.2.10 | fetched and built for Emscripten by `cmake/Dependencies.cmake` (no system SDL in the sysroot) |
| Browser tested | Google Chrome 154.0.8037.97, headless, `--use-angle=swiftshader` | any WebGL2 browser should work |

Other emsdk versions probably work; the link flags in `cmake/Web.cmake` are the
part most likely to change between releases (`-sGL_ENABLE_GET_PROC_ADDRESS`,
`-sSTACK_SIZE`).

## Building

Two builds: a native **host-tools** build (the shader cross-compiler and the
font importer run on the build machine), then the **web** build.

```sh
# 1. Host tools (native, Release, no Vulkan/tests): build/host-tools
cmake --preset host-tools
cmake --build --preset host-tools          # targets: shader_tool, runtime (the `cuervo` binary)

# 2. Web build: build/web  (EMSDK must point at the emsdk checkout)
source ~/emsdk/emsdk_env.sh                # or export EMSDK/EM_CONFIG/PATH by hand
cmake --preset web
cmake --build --preset web
# -> build/web/runtime/index.html, index.js, index.wasm, index.data
```

`cmake --preset web` uses `$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake`
as the toolchain and `CUERVO_HOST_BUILD=build/host-tools`. Without the preset:
`emcmake cmake -B build/web -G Ninja -DCMAKE_BUILD_TYPE=Release -DCUERVO_HOST_BUILD=$PWD/build/host-tools`.

Cache variables (`cmake/Web.cmake`):

| Variable | Default | Meaning |
|---|---|---|
| `CUERVO_HOST_BUILD` | `build/host-tools` (preset) | native build providing `shader_tool` and `runtime/cuervo` |
| `CUERVO_SHADER_TOOL` | `<host>/shader_tool` | explicit host shader tool instead |
| `CUERVO_WEB_GAME` | `examples/shmup` | project directory preloaded as `/game` |
| `CUERVO_WEB_FONT_SOURCE` | `assets/fonts/LiberationSans-Regular.ttf` | converted at build time to `/fonts/default.efont` (an `.efont` is used as is) |
| `CUERVO_WEB_PACK` | empty | run an exported pack instead of `CUERVO_WEB_GAME`: `fetch` (preset `web-pack`) builds a game-independent runtime that fetches `game.pak` next to the page; a `.pak` path preloads that pack (docs/EXPORT.md "Web") |

### Shipping a game

The preloaded project directory above is the development build. A release
ships a pack (docs/EXPORT.md):

```sh
cmake --preset web-pack && cmake --build --preset web-pack       # build/web-pack/runtime, once
cuervo-tools export <project> --platform web -o dist-web/ --strip  # copies it + writes game.pak
node tools/web/smoke_test.mjs --dir dist-web --args "--screenshot /tmp/w.ppm" --expect "state hash a3e57e79964421d3"
```

The page fetches `game.pak` into MEMFS before `main()` (`web/pack_loader.js`),
and the runtime runs `--pack /game.pak`; the font comes from the pack
(`.cuervo/default.efont`). For the shmup that is 1.9 MB of `game.pak` beside
the same 2.6 MB `.wasm`. Verified: state hash `a3e57e79964421d3` at tick 240 in
headless Chrome for the fetch build and for a build with the pack preloaded.

## Running it

The page must be served over HTTP (browsers refuse the `.data` preload from
`file://`):

```sh
python3 -m http.server 8000 --directory build/web/runtime
# open http://localhost:8000/   (click the canvas for keyboard focus)
```

URL parameters (from `web/shell.html`): `?args=<runtime args>` appends runtime
arguments (space separated, URL-encoded), e.g. `?args=--quit-after%2010`;
`?log=1` shows the engine log under the canvas. The log always goes to the
browser console too.

## Verifying

```sh
~/emsdk/node/24.19.0_64bit/bin/node tools/web/smoke_test.mjs --dir build/web/runtime --seconds 5 \
    --screenshot build/web-shot.png
```

The script serves the directory, starts headless Chrome/Chromium with
SwiftShader WebGL2 and remote debugging, loads `index.html?args=--quit-after 5`
and reads the console over the DevTools protocol. It passes when the engine
logs `--quit-after reached` with no `[error]` line, abort or page exception,
and saves a mid-game screenshot. `--expect-backend webgpu|gl` also requires that
device, `--no-webgpu` hides `navigator.gpu` to run the WebGL2 fallback, and
`--pull <memfs path>=<file>` copies a file the engine wrote (its `--screenshot`
PPM) out of the page for `rhi_parity compare`. Determinism against a native build:

```sh
./build/<native>/runtime/cuervo --game examples/shmup --backend gl --screenshot /tmp/n.ppm   # logs "state hash H"
node tools/web/smoke_test.mjs --args "--screenshot /tmp/w.ppm" --expect "state hash H"
```

(Verified: `a3e57e79964421d3` at tick 240 on GL and WASM.) The web build compiles the
WASM SIMD128 paths (`-msimd128`, `CUERVO_SIMD=simd128`, the web default) and
runs the job system inline on one thread; neither changes the hash. Manual check: serve as above, open in a desktop browser, play with
keyboard and gamepad, and watch the console.

## What the web build does differently

| Area | Desktop | Web | Where |
|---|---|---|---|
| Frame loop | `while (loop->frame())` | `emscripten_set_main_loop_arg`, one `frame()` per animation frame; `run_main_loop` never returns | `cuervo/platform/main_loop.hpp` |
| Graphics | Vulkan or GL 3.3 core | WebGPU when the browser has it (with `CUERVO_WEBGPU`, the default), else the GL backend on a WebGL2 context (GLES 3.0 path, GLSL ES 300); Vulkan not compiled (see "WebGPU") | `cuervo/platform/window.cpp`, `cmake/Web.cmake`, `web/webgpu_init.js` |
| Threads | yes | default: none (no `-pthread`, no SharedArrayBuffer, so no COOP/COEP headers needed), job system inline (DESIGN.md 4.14); `CUERVO_WEB_THREADS=ON`: WASM threads, a real job system (see "Threads") | `cuervo/platform/platform.hpp`, `cmake/Web.cmake` |
| Audio | miniaudio device thread | miniaudio's Web Audio backend (ScriptProcessorNode, main thread); browsers start audio after the first click/key | `cuervo/audio/` |
| Files | real file system | Emscripten MEMFS: the project at `/game` and the font at `/fonts/default.efont`, or an exported `/game.pak`; writes (e.g. `Scene.save`, which goes to SDL's pref path in release) vanish on reload | `cmake/Web.cmake` |
| Hot reload | scripts watched by mtime | off (`platform::has_file_watching = false`); F5/F6 still recompile/restart from the bundle | `cuervo/platform/file_watcher.hpp` |
| Fonts | TTF/OTF import or `.efont` | `.efont` only (no FreeType/msdfgen in the WASM) | `CUERVO_FONT_IMPORT=OFF` |
| Escape | quits (unless bound) | never quits; a tab can't close itself | `game_runner.cpp` |
| Exceptions | native | `-fwasm-exceptions` (the language compiler reports limits with exceptions) | `cmake/Web.cmake` |
| Demos (`--demo`, `--svg`) | yes | no (blocking loops); the web runtime always runs `--game` | `runtime/main.cpp` |

## Threads (experimental)

Opt-in and less widely verified than the default single-threaded build ([STATUS.md](STATUS.md)).

`cmake --preset web-threads` (raw project) and `web-pack-threads` (pack-loading,
for `cuervo-tools export --platform web --runtime build/web-pack-threads/runtime`)
build with WASM threads (`CUERVO_WEB_THREADS=ON`): everything, fetched
dependencies included, compiles with `-pthread`, and the page starts a pool of 8
workers before `main()` (`-sPTHREAD_POOL_SIZE=8`), so creating the job system's
threads never blocks the browser's main thread on a worker that can't load. The
job system then works as on desktop (`--jobs`, `CUERVO_JOBS`, auto = up to 8).

Hosting: SharedArrayBuffer needs a **cross-origin isolated** page, i.e. the
server sends `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp` (the smoke test's server always
does). Without them the threaded build fails to start; the default
single-threaded build needs neither.

Measured on headless Chrome on the RTX 3060 Ti (`smoke_test.mjs --gpu`),
`examples/bench` (50k shapes) `--bench 6`:

| Build | Job threads | FPS | sim ms/tick | state hash |
|---|---|---|---|---|
| web-pack | 1 (inline) | 226.3 | 0.242 | |
| web-pack-threads | 1 | 225.1 | 0.292 | |
| web-pack-threads | 8 | 226.7 | 0.147 | |

The shmup reaches `a3e57e79964421d3` at tick 240 with 1 and 4 threads (as
on desktop). Threads halve the simulation time; the frame rate is set by the GPU
side at this load. Emscripten warns that `-pthread` with memory growth slows
JavaScript-side memory access (WebGL calls go through JS); the render CPU time
above shows no visible cost.

## WebGPU (experimental)

Milestone 10.3 (DESIGN.md 4.4 "Clarified (Milestone 10.3)"): a web build made
with `CUERVO_WEBGPU` (the default when the host has naga-cli 29.0.4 and
spirv-opt) carries both backends in one `.wasm`. The page runs WebGPU when the
browser has it and WebGL2 otherwise; the game cannot tell (render only: the
state hash is the same).

- **Before `main`**, `web/webgpu_init.js` (linked with `--pre-js`) asks
  `navigator.gpu` for an adapter (`high-performance`, then any) and a device, and
  hands the device to emdawnwebgpu as `Module.preinitializedWebGPUDevice`. The
  engine never waits on a browser promise, so the build needs neither JSPI nor
  Asyncify and loads in any browser.
- **Choice**: `rhi::default_backend()` is WebGPU when the page got a device and GL
  (WebGL2) otherwise; if the WebGPU device still fails to open, the player falls
  back to WebGL2 like a desktop falls back from Vulkan. `?args=--backend gl`
  forces WebGL2 (the page then requests no device); `--backend webgpu` insists
  on WebGPU and fails without it.
- **Errors**: a device made by the page can't take the engine's error callback,
  so `webgpu_init.js` logs uncaptured errors and device loss to the console as
  `[error] WebGPU ...`, which the smoke test treats as a failure.
- **Threads**: works with `CUERVO_WEB_THREADS` (WebGPU calls stay on the page's
  thread; workers skip the device request).
- **Exports**: `cuervo-tools export --platform web` builds web players with
  `CUERVO_WEBGPU=ON` (docs/EXPORT.md "Web"); the prebuilt `web-pack` runtimes
  have it whenever their build found naga.

```sh
# Host tools as above (they need naga-cli 29.0.4 and spirv-opt for the WGSL:
#   cargo install naga-cli --version 29.0.4 --locked; pacman -S spirv-tools)
cmake --preset web-pack && cmake --build --preset web-pack       # CUERVO_WEBGPU defaults on
cuervo-tools pack examples/shmup --platform web -o build/web-pack/runtime/game.pak
# WebGPU on the real GPU, then the WebGL2 fallback (navigator.gpu hidden):
node tools/web/smoke_test.mjs --dir build/web-pack/runtime --gpu --expect-backend webgpu \
    --args "--screenshot /tmp/w.ppm" --expect "state hash a3e57e79964421d3"
node tools/web/smoke_test.mjs --dir build/web-pack/runtime --gpu --no-webgpu --expect-backend gl \
    --args "--screenshot /tmp/w.ppm" --expect "state hash a3e57e79964421d3"
```

Verified in headless Chrome 156 on the RTX 3060 Ti, for the `web-pack` and
`web-pack-threads` (`--jobs 4`) runtimes, on WebGPU and on the WebGL2 fallback:
`examples/shmup` reaches `a3e57e79964421d3` and `examples/lantern_cave` (with the
`10-114:move_right,60:jump,124:jump,138-240:move_right` presses of its sweep)
`2dd8beccd71bc67e` at tick 240, both their native hashes. The engine's own
captures (`--pull`) of the two backends match on the shmup up to anti-aliased
edges; `lantern_cave` differs in 0.013 % of its pixels (at most 26/255) inside
the raymarched soft shadows, where ANGLE's GL and Dawn's Vulkan round differently.

- Headless Chrome on Linux only offers SwiftShader to WebGPU unless it is told not
  to want a Vulkan surface (`--disable-vulkan-surface`, part of the smoke test's
  `--gpu`), and then it answers `high-performance` with no adapter (hence the
  second request). Its page screenshots (`--screenshot`) of a WebGPU canvas come
  out blank in that mode; the frames themselves are right (check them with `--pull`).
- `capture_frame` maps its readback asynchronously (a main-loop callback cannot
  block): the file lands a frame or so after the log line.
- The canvas is configured with `COPY_SRC` (emdawnwebgpu reports no surface usages;
  the WebGPU spec allows it) so screenshots work.
- Firefox and Safari are untested; any browser without WebGPU gets WebGL2.
- Benchmarks (milestone 10.4): `tools/bench.py record --web <runtime dir> --backends webgpu
  webgl2` ([PERFORMANCE.md](PERFORMANCE.md) "Web"). With the threaded runtime, `examples/bench`
  at 100k shapes runs at 390 FPS on WebGPU and 210 on WebGL2 (Chrome presents WebGL2 at most 240
  times a second); `--bench` (VSync off) runs web frames back to back on `setImmediate`.

## Decisions (recorded in DESIGN.md 4.4 and 4.14)

- Single-threaded WASM by default, so hosting needs no special headers; WASM
  threads are opt-in (`CUERVO_WEB_THREADS`, "Threads" above).
- One project per web build, preloaded (`--preload-file`), not fetched lazily:
  the shmup is 1.9 MB of `.data` (1.1 MB of it the font atlas) next to a 2.6 MB
  `.wasm`. That is the development build; a release ships a packed archive instead
  (DESIGN.md 4.11, "Shipping a game" above).
- The default framebuffer has no MSAA (`antialias: false`); the engine renders
  into its own 4x MSAA renderbuffer and blits, exactly like desktop GL.
- Release builds only in practice (Debug WASM works but is slow).

## Limitations

- Compressed downloads: the pack is stored uncompressed (docs/EXPORT.md "Pack
  format"); gzip/brotli is up to the web server.
- Persistent saves (IDBFS) for `Scene.save` and the planned `Save` API (milestone 11.4) are
  not implemented: writes vanish on reload.
- Audio uses the main-thread ScriptProcessorNode backend (deprecated but universal); an
  AudioWorklet backend (needs WASM threads) is planned.
- Firefox and Safari are untested (headless Chrome, SwiftShader or the real GPU with `--gpu`).
- The engine's doctest suites are not run under Node (they need the real file
  system: `-sNODERAWFS`); determinism is checked through the state hash instead.
