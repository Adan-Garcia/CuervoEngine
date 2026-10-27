# Cuervo Engine & Pluma Design

> Status: Beta (v0.1). The engine is **Cuervo Engine**, its scripting language
> is **Pluma**, and script files use `.plm` (see 7.1 "Names"). The
> text below often says just "the engine" and "the language".
>
> **How to read this document.** It is the architecture and decision record, not the
> support statement. What is shipped, experimental, planned or unsupported is in
> [STATUS.md](STATUS.md). In sections 4 and 5, the opening paragraphs state the design
> and the **Clarified (...)** notes record what was built and any deviation from it; where
> they differ, the Clarified note and STATUS.md are current. Section 6 lists every
> milestone and marks the finished ones "done".

## 1. Vision

A general-purpose **2D game engine**, written in C++, with its own **statically typed
scripting language**. It is meant to be used by people other than its author, so the
editor, error messages, documentation and stability are first-class concerns.

Design pillars, in priority order:

1. **Performance first.** A bullet hell with 10k–100k live bullets must hold frame rate,
   including on the web. No per-object overhead "for later".
2. **Deterministic simulation.** The same seed + the same input stream = the same game,
   every time. This gives replays, reproducible bug reports and future rollback netcode.
3. **Vector-first graphics.** SVG and procedural shapes are the primary art pipeline;
   raster art is supported but secondary.
4. **Pleasant scripting.** Static types with inference, coroutines, good errors,
   hot reload.

Non-goals for now: engine-level modding, live in-game code execution in release builds.

## 2. Decision summary

| Area | Decision |
|---|---|
| Engine language | C++20, CMake |
| Platforms | Linux and Web (WASM) are verified; Windows is being validated (7.4; the Clang debug build passes all tests and matches the Linux hashes, docs/WINDOWS.md). macOS, mobile and drop-in console SDKs in Phase 13 |
| Dimensionality | 2D primary. Generic `Vec<T,N>` (vec1…vecN) for extra axes; zero-bloat 3D in Phase 14 |
| Simulation | Deterministic, fixed timestep, seeded RNG, strict float rules |
| Scene model | Node tree (script objects) for authoring/UI; hot systems keep contiguous SoA pools; Hybrid model (queries exposed to scripts) in 12.4 |
| Graphics backends | Vulkan (desktop), OpenGL 3.3 / GLES 3.0 / WebGL2 (web + legacy), behind an RHI; both implement every feature |
| Render tiers | `compatible` (exports to all backends) or `full` (Vulkan-only features) |
| Vector rendering | SDF primitives + tessellated SVG + MSDF text + raster fallback |
| SVG | Practical subset |
| Vector animation | Property, method and event tracks, `Tween` and an `AnimationTree` state machine (9.4); cutout, skeletal and morph animation are planned |
| Runtime drawing | Canvas-style script API |
| Resolution | Virtual coordinate space + scale mode; vectors render at native resolution |
| Particles | General particle system: gameplay (CPU, deterministic, collidable) and visual (GPU) modes, native modules + script `foreach`. No dedicated bullet system: bullets are built on particles |
| Collision | Circles + capsules in a spatial hash (AABB/OBB planned); general physics is separate and optional (implemented, 9.6) |
| Input | Named actions/axes, rebindable, recorded per tick |
| Audio | miniaudio + mixer buses + voice limiting (default); optional FMOD/Wwise via C ABI |
| Game UI | Retained UI nodes in the scene tree |
| Assets | Import pipeline (atlases, hot reload) + packed archive for release |
| Scene files | Human-readable text in project (Git 3-way mergeable, 11.2c) + packed binary archive for release |
| Editor | Standalone Dear ImGui application |
| Threading | Scripts single-threaded; engine uses a job system (1 thread on web) |
| Netcode | None yet; stay rollback-ready |
| Modding | None at engine level |
| Script execution | Bytecode VM in development + AOT-to-C for native (host-platform) release exports |
| Typing | Static with inference |
| Syntax | Braces, `//` and `/* */` comments, indentation inside braces by style guide |
| Memory | Refcount + incremental cycle collector; opt-in checked manual memory, `unsafe` escape |
| OOP | Single inheritance + traits |
| Errors | `Result<T, E>` + `?`; panics for bugs |
| Null | All reference types nullable; null use panics with a clear message |
| Numbers | `int` = i32, `float` = f32, sized types available |
| Strings | Immutable UTF-8 `str` + interned `#name` |
| Modules | Global class names (no imports) |
| Hot reload | Keep object state, migrate fields by name |
| Colors | `Color.hex("ff4060")` |
| License | MIT License ([LICENSE.md](../LICENSE.md)) |
| Names | Cuervo Engine, Pluma language, `.plm` files (7.1) |

## 3. Architecture

```
cuervo/
  core/       fixed-timestep loop, logging, allocators, file system, events, RNG
  math/       Vec<T,N>, Mat, transforms, deterministic trig
  platform/   SDL3 window + input devices, timing, file watching
  rhi/        render hardware interface
    vulkan/   desktop backend
    gl/       OpenGL 3.3 / GLES 3.0 / WebGL2 backend
  render/     SDF batcher, mesh batcher, text (MSDF), camera
  vector/     SVG parser (subset), tessellator (import + runtime), stroker
  scene/      binary scene files and migrations (the node tree itself is Pluma, in script/prelude/)
  particles/  emitters, SoA pools, native modules (visual mode runs on the CPU; GPU compute planned)
  collision/  shapes (circle, capsule), spatial hash, queries
  physics/    optional general physics (implemented, 9.6)
  audio/      miniaudio decoding + the engine's own mixer, buses, voice limiting
  input/      action maps, input frames, recording/playback, replay files
  assets/     virtual file system, pack format, manifest
  brand/      generated brand tokens and the embedded app icon
  jobs/       job system
  script/     engine bindings, project loading, game session, and the engine prelude
              (`prelude/*.plm`: nodes, UI controls, collision, particles, audio, physics)
pluma/        standalone: lexer, parser, type checker, bytecode compiler, VM, AOT-to-C, formatter,
              LSP, DAP debugger, profiler, REPL (`plm`), and the math library (`std/math.plm`)
editor/       Dear ImGui editor (`cuervo-editor`), hosts the engine; desktop only
runtime/      the player executable (`cuervo`), also the exported game's player
tools/        `cuervo-tools` (pack, export, scenes, tiers, formatter, LSP, API docs), the
              docs-site generator, shader_tool, rhi_parity, the web smoke test, the VS Code extension
web/          the web shell page and pack loader
docs/
tests/        cuervo/, pluma/ and editor/ doctest suites plus the end-to-end ctest scripts
              (internal; users get no test framework)
```

Not in the tree yet (planned): a general ECS exposed to scripts (decision
in 12.4), `ui/` as native code (the UI is Pluma nodes, 4.10), networking (11.1).

**Rule:** `pluma/` does not depend on `cuervo/`. The engine registers its API with the
language through `script/`. This keeps the language testable on its own and gives it a REPL.

### Third-party libraries

| Purpose | Library | License | Status |
|---|---|---|---|
| Window, input, platform | SDL3 | zlib | used |
| Vulkan helpers | Vulkan-Headers + volk + VMA | Apache-2.0 / MIT | used |
| Shaders | `glslangValidator` (build tool) → SPIR-V, SPIRV-Cross → GLSL 330 / ES 300 | Apache-2.0 | used at build time |
| Audio | miniaudio | MIT-0 / public domain | used |
| SVG parsing | in-house subset parser | n/a | used |
| Path tessellation | libtess2 | SGI Free Software License B 2.0 | used |
| MSDF fonts | msdfgen + msdf-atlas-gen | MIT | used |
| Font loading (for msdfgen) | FreeType | FTL (BSD-style) | used (optional) |
| Editor UI | Dear ImGui (docking branch) | MIT | used |
| Profiler bridge | Tracy | BSD-3-Clause | optional, off by default |
| Internal tests | doctest | MIT | used |
| General physics | Box2D v3 | MIT | used (optional, 9.6) |
| Text shaping | HarfBuzz | MIT | planned (11.3) |
| Raster images | stb_image | public domain | planned (11.2a) |

The full list with versions and license texts is [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).
Third-party components keep their own permissive licenses; the engine's own license is
described in 4.18.

## 4. Engine systems

### 4.1 Main loop and determinism
- The simulation runs at a fixed tick (default 60 Hz, configurable, e.g. 120). Rendering
  interpolates between the last two sim states.
- One RNG (PCG32) per simulation, seeded at scene start. No hidden randomness.
- Compiler flags: no fast-math, `-ffp-contract=off`, consistent SIMD paths.
- `sin`, `cos`, `atan2`, `sqrt` and similar come from the engine's own math library, not libm,
  so results match across compilers and between native and WASM.
- Iteration order of every container that the simulation touches is defined (no unordered
  hash-map iteration in sim code).
- Audio, rendering and the cycle collector's timing never feed back into the simulation.
- Guarantee: bit-identical on the same build and platform. Goal: identical across platforms,
  checked by comparing state hashes (the shmup reaches the same hash natively on Linux and in
  the web build).

### 4.2 Math: `Vec<T,N>`
```cpp
template<typename T, std::size_t N>
struct Vec { T v[N]; /* +, -, *, /, dot, length, normalize, swizzles for N<=4 */ };
using vec2 = Vec<float,2>;  // SIMD-specialized for N = 2..4
```
Transforms are 2D by default: `vec2` position, `float` rotation, `vec2` scale, `int` layer.
Components that need more axes use larger vectors explicitly.

### 4.3 Scene model: ECS + node tree
- **Runtime:** the hot systems (particles, collision, rendering) iterate SoA arrays directly.
  There is no general-purpose ECS in the tree: gameplay objects are nodes, and whether to expose
  component queries to scripts or drop the ECS from the design is milestone 12.4.
- **Authoring:** a node tree (parent/child, names, scripts). Users see nodes; systems see their
  own arrays.
- Native objects are referenced by **generational handles** (`index + generation`), never raw
  pointers.

**Clarified (Phase 3, `cuervo/script/`):** the node tree is written in the language itself
(`cuervo/script/prelude/scene.plm`: `Node`, `Node2D`, `Scene`), so scripts extend it like
any class and hot reload migrates it like any object. The hot data (particles, collision) lives
in SoA systems that nodes drive through bindings; nodes have no entity handles.
One fixed tick of a script project runs, in this order: Time/input update, coroutines due
this tick (start order), the tree's `update(dt)` (parents before children, children in
insertion order), the particle system tick (simulate, spawn, collide), `on_hit(tag, count)`
on hitbox owners (hit order), then nodes queued with `queue_free()` leave the tree
(during a pause, §4.10, only `always_process` nodes and their coroutines run and the
particle step is skipped)
(`exit()` children first; their coroutines are cancelled). `draw(g)` runs every rendered
frame in tree order; it draws the latest tick (render interpolation of script state is
deferred). See `cuervo/script/script_host.hpp`.

### 4.4 Rendering
**RHI.** The engine never calls Vulkan or GL directly. The RHI exposes buffers, textures,
pipelines, render passes and instanced draws. Every feature is tagged with a capability
tier.

**Render tiers** (project setting):
```toml
render_tier = "compatible"   # GL-level features; exports to Vulkan desktop + WebGL web
# render_tier = "full"       # + compute, bindless, etc.; desktop only
```
Changing tier is a "migration". The editor lists every asset, shader or API use that the
new tier doesn't support before applying the change.

**Clarified (Phase 5, `cuervo/rhi/README.md`, `cuervo/script/tier_check.hpp`):**
- The key is `render_tier` (as above), default `"compatible"`; anything else is a
  project.toml error, and a stray `tier = ...` gets a "did you mean render_tier" error.
- *Capabilities:* `rhi::Feature` enumerates every RHI feature with its tier and whether
  it is required or optional (MSAA, frame capture degrade). `DeviceInfo` reports the
  hardware's features and limits and the features usable after the **tier gate**:
  a device is created for `min(project tier, backend max)`; gated to Compatible it
  refuses Full features (logs once, no-op) and clamps limits, so a Compatible game
  tested on Vulkan can't use what WebGL2 lacks. GL's max tier is always Compatible.
- *Compatible limits* (what the tier promises): textures 4096 px, 4x MSAA, 16 KB per
  uniform range, 16 vertex attributes, 256-byte uniform offset alignment. These are
  WebGL2 reality rather than the GLES 3.0 spec minimums (2048 px textures): virtually
  every WebGL2 device offers 4096, and 2048 would forbid ordinary atlases.
- *Runtime:* a Full project on a device that can't serve Full (GL, web, or missing a
  required Full feature) is **refused** with an error naming `--backend vulkan` and the
  migration command; it never silently falls back from Vulkan to GL. A Compatible
  project on a device missing something only **warns** and runs.
- *First Full feature:* base-vertex indexed draws (`draw_indexed_base_vertex`, GLES 3.0
  has none). Compute, storage buffers, bindless and GPU visual particles join the
  Full tier when they are built; their shader constructs are already flagged by the
  migration report.
- *Migration report:* `cuervo-tools tier check <project> [--to compatible|full]` scans
  project.toml, `shaders/` (stages and GLSL constructs GLES 3.0 can't express), `scripts/`
  (Full-only script API from a rule table, plus web caveats such as `Scene.save` not
  persisting), `scenes/` (rule table over node classes and enum values; unreadable
  scenes) and `assets/` (textures over the Compatible limit, formats the engine can't
  load, TTF/OTF needing `.efont` on the web, web download size). Each item has a
  severity (error blocks the tier, warning, note) and a file:line:column.
  `cuervo-tools tier set <project> <tier> [--force]` applies the change only when
  nothing blocks it. The editor shows the same report in its Tier Report panel (4.13).
- *Parity:* every RHI entry point is implemented identically on both backends
  (audit table in `cuervo/rhi/README.md`); `tools/rhi_parity` renders one scene per
  feature through `capture_frame`, and the `rhi_parity` test compares GL 3.3 vs GLES 3.0
  (identical) and GL vs Vulkan (identical except anti-aliased edges: Vulkan's y-flipped
  viewport mirrors the MSAA sample pattern and half-pixel rounding).
- *GLES on desktop:* `--gl-es` / `CUERVO_GL_ES=1` asks the GL backend for an OpenGL ES
  3.0 context, so the web's shader path (GLSL ES 300) is testable without a browser.

**Clarified (frame pacing and stream buffers, `cuervo/rhi/gl/README.md`, `docs/PERFORMANCE.md`):**
- *GL bounds the frames it queues.* The GL backend puts a fence after each presented frame and
  waits on the one from two frames ago (`rhi::FenceRing`), matching Vulkan's two frames in
  flight; the web build keeps none (WebGL2 cannot block, the browser paces frames). GL
  frame times and peak memory therefore measure GPU completion, not how far the driver let the CPU run ahead.
- *Stream buffers are a small ring on both backends.* GL rotates three buffers; Vulkan
  renames only when it must. A second update in a frame beside the first (the batchers'
  per-layer flush) writes in place on both. Uploads past 1 MB are sliced on GL, and
  Vulkan puts large streams in system memory when their versions would crowd the CPU-writable
  VRAM window. **Decision:** a batcher's instance buffer grows by a quarter, not double, so a
  1.9M-instance frame does not allocate 168 MB x 3.

**Clarified (Milestone 8.8, `docs/support.json`, `docs/STATUS.md`, `cuervo/script/support.hpp`):**
- *One source for support.* `docs/support.json` lists every feature with its status
  (stable, experimental, planned, unsupported), the platforms it works on (Linux, Windows,
  Web), the desktop-only and web-compatible flags, the render tier it needs and a note.
  `tools/support_matrix.py` generates the tables in `docs/STATUS.md` and the summary in
  `README.md` from it (the `support_matrix_up_to_date` test fails when they are stale, and
  the docs site build refuses a stale page); CMake compiles the same file into the tools
  (`cmake/SupportData.cmake`), so the checks below read the data the documents show.
- *The tiers as a contract.* `compatible` runs on Vulkan, GL 3.3, GLES 3.0 and WebGL2 on
  every platform; the device is clamped to the limits in 4.4 above, shader stages and
  constructs GLES 3.0 cannot express are tier errors, and Full-only RHI calls on a gated
  device are ignored with one log line. `full` is Vulkan on the desktop only, has no numeric
  minimums yet, and its Full-only RHI features are required. The documented RHI feature
  table and the compatible limits are compared with `rhi::feature_tier`, `feature_required`
  and `compatible_limits()` in code by the `support.json` tests, so the documents cannot drift.
- *Claims are checked against the target.* `tier check <project> [--to TIER] [--platform
  linux|windows|web]` and `export` call `support::check_claims`: the project's render tier
  (and AOT-compiled scripts, which also need host == target) must be listed for the target platform and shipped; otherwise
  it is an error (a `full` project cannot be exported with `--platform web`, which used to be
  silently checked as `compatible`), and an experimental target or feature is a warning.
  The runtime already refuses a Full project on a device that cannot serve Full. The export
  gate no longer retargets the check to `compatible` for the web: migrate with `tier set`.

**Clarified (Milestone 10.1, `cuervo/rhi/webgpu/`, `cmake/Shaders.cmake`, docs/WEB.md "WebGPU"):**
- *A third, experimental backend.* `rhi::Backend::WebGPU` serves the Compatible tier only
  (Full stays Vulkan-only). **Decision:** native WebGPU is **wgpu-native** (v29.0.1.1,
  prebuilt release archives pinned by hash, linked statically), the same wgpu/naga release
  line as the pinned naga-cli 29.0.4, so the WGSL the build writes is parsed by the naga
  it came from; the browser uses Emscripten's **emdawnwebgpu** port, the same `webgpu.h`.
  `CUERVO_WEBGPU` defaults on for native Linux x86_64 builds that find both host tools,
  and is off for the web and for exported players until 10.3.
- *One shader source.* Each GLSL 450 file is compiled a second time with `CUERVO_WEBGPU`
  defined (WGSL has no point size, so `gl_PointSize` is skipped), `spirv-opt
  --split-combined-image-sampler` splits the samplers, `shader_tool --webgpu-bindings`
  renumbers set 1 (texture binding N -> texture 2N, sampler 2N + 1) and `naga
  --keep-coordinate-space` writes the WGSL into the same generated header. The sources
  already put NDC y = +1 at the top, WebGPU's convention, so nothing flips.
- *GL semantics on WebGPU.* Queue writes land before the frame's commands run, so a buffer an
  unsubmitted draw reads is renamed (refilled from a CPU shadow; retired buffers are reused
  once their submission is out); stream buffers write disjoint ranges in place, as on Vulkan.
  A full-target `clear` becomes the next render pass's load op, a scissored one draws a
  triangle. Resources bind through two per-draw bind groups (uniforms with dynamic offsets;
  textures), and blocks or textures a shader declares but the caller left unbound read a
  zeroed dummy. Frames in flight are bounded at two natively, like GL's fence ring.
- *Headless and the web.* Without a window surface (SDL's offscreen driver) the device
  renders into an offscreen target, so `rhi_parity` compares WebGPU with GL in every
  ctest run. In the browser, adapter and device requests block through JSPI (`-sJSPI`;
  they run under `main`), and `capture_frame` maps its readback asynchronously because a
  main-loop callback cannot suspend.
- *Result.* Every `rhi_parity` scene matches GL with no interior differences and Vulkan
  exactly except `points_lines` (WebGPU points are 1 px; 10.2 decides that scene); the
  shmup and `lantern_cave` frames are within one level of GL with their usual state
  hashes, and the shmup runs on WebGPU in headless Chrome to the native hash
  `a3e57e79964421d3`.

**Clarified (Milestone 10.2, `cuervo/rhi/README.md`):**
- *Points.* **Decision:** Compatible-tier points are 1 px on every backend (what
  `Feature::PointsAndLines` already promised); shaders that must write `gl_PointSize` for
  Vulkan write 1.0. The engine draws no point lists (particles are instanced quads), so
  nothing visible changes, and WebGPU needs no point-sprite emulation.
- *Parity.* The audit table has a WebGPU column; every scene renders on WebGPU equal to
  Vulkan's with tolerance 0 (wgpu-native on Vulkan shares its rasterization) and within the
  Vulkan edge tolerance of GL. `rhi_parity frames` also checks WebGPU's two-frame bound, and
  a unit test checks that every engine shader carries generated WGSL with the bindings its
  reflection tables name.

**Clarified (Milestone 10.3, `web/webgpu_init.js`, docs/WEB.md "WebGPU"):**
- *One web build, two backends.* **Decision:** a web build has both WebGPU and GL in one
  `.wasm` (`CUERVO_WEBGPU` now defaults on for the web when naga is found; exported web
  players always have it) and picks at load: `rhi::default_backend()` is WebGPU when the page
  got a device, else GL (WebGL2), and a WebGPU device that still fails to open falls back to
  WebGL2 like Vulkan does to GL on the desktop. Two builds (a JSPI one and a plain one) were
  rejected: double the download and the export, for a choice the page can make.
- *No JSPI.* The page requests the adapter and device before `main` (a run dependency of a
  `--pre-js`) and hands the device over as `Module.preinitializedWebGPUDevice`; the engine
  never waits on a browser promise. 10.1's JSPI link flag is gone, so the build loads in
  browsers without JSPI, which then get WebGL2.
- *Errors* of a device the page made are logged by the page (`[error] WebGPU ...`), since the
  engine's error callback can only be set when it creates the device itself.
- *Result.* `examples/shmup` and `examples/lantern_cave` reach their native state hashes in
  headless Chrome on WebGPU (the real GPU) and on the WebGL2 fallback, with and without
  `CUERVO_WEB_THREADS`; `export --platform web` ships and verifies the dual-backend player.

**Shaders** are written once in GLSL 450, compiled to SPIR-V for Vulkan and
cross-compiled to GLSL 330 / ES 300 for GL. Shader variants are built at import time, not
at runtime.

**Vector pipeline:**

| Content | Technique |
|---|---|
| Particles, UI panels, primitives | Analytic SDF shapes (circle, capsule, rounded rect, polygon, star, ring), instanced; one draw call per material |
| SVG art | Tessellated to triangle meshes at import; anti-aliased with MSAA where available, otherwise with feathered edge geometry |
| Text | MSDF font atlases |
| SVG filters, blurs | Planned: rasterized to an atlas at the needed scale, cached |
| Canvas API, shape morphing | Runtime tessellator; meshes cached until the path changes |

**Resolution.** A project defines a virtual coordinate space (e.g. 1280×720) and a scale
mode: `fit` (default; renders vectors at native resolution), `integer` (pixel art),
`stretch`. Filtering (`linear`/`nearest`) applies to raster assets only.

### 4.5 Vector assets and animation
**SVG subset:** paths, basic shapes, fills (nonzero/evenodd), strokes (width, joins, caps,
miter limit, dashes), linear and radial gradients, transforms, groups, opacity, clip paths.
Unsupported features (filters, masks, CSS, embedded text) are skipped with an import warning;
the rasterized fallback is planned (11.2a).

**Animation** (implemented in milestone 9.4, §4.17):
- *Implemented:* `AnimationPlayer` with property tracks over any `@export` field, method
  tracks and event tracks, with step, linear and eased interpolation; `Tween`, a
  chainable script-driven animation; and `AnimationTree`, a state machine with parameter
  conditions and cross-fades between states. Animations are saved in scene files.
- *Planned:* these are not implemented, and the track system is built to carry them.
  - *Cutout/transform:* SVG groups become parts animated on a timeline (position, rotation,
    scale, opacity, draw order).
  - *Skeletal + mesh deformation:* bones with weighted vertices of the tessellated mesh.
  - *Shape morphing:* keyframed path points, interpolated and re-tessellated. Paths must have
    compatible topology; the importer resamples or reports an error if they don't.
- *Not implemented in 9.4:* Bezier handles on keys, blend spaces and layered blending.

### 4.6 Particle system
There is **no dedicated bullet system**. The engine provides a general particle system;
bullets (and any other projectile) are built on top of it by games or a library.

**Two simulation modes**, chosen per emitter:

| Mode | Runs on | Deterministic | Collision | Use for |
|---|---|---|---|---|
| `gameplay` | CPU (SoA arrays, job system) | Yes | Opt-in | Bullets, pickups, anything that affects game state |
| `visual` | CPU today on every tier; GPU compute on the Full tier is planned | No | No | Sparks, smoke, dust, ambient effects |

Visual particles never feed back into the simulation, so they can't break replays.

**Data layout.** Each emitter owns a pool of particles stored as SoA arrays
(`pos[]`, `vel[]`, `age[]`, `life[]`, `size[]`, `color[]`, `rot[]`, plus per-module columns).
Particles are not ECS entities. Dead particles are swap-removed, with a deterministic order.

**Emitters** combine a spawn rule, a stack of modules and a render shape:
```
emitter {
    mode:    gameplay
    spawn:   ring(count = 36), every = 0.1
    modules: [
        Velocity(speed = 200.0),
        Accelerate(0.98),
        SizeOverLife(curve = ease_out),
        Collide(layer = #enemy_shots, shape = circle(4.0)),
    ]
    render:  Sdf.circle(4.0)
}
```
- **Spawn rules:** point, burst, ring, arc/spread, line, area/shape (from an SVG path),
  aimed at a target, and sequences over time. Driven from scripts (`emitter.burst(36)`) or by
  rate.
- **Native modules** are stackable and run in tight loops: velocity, acceleration, gravity,
  drag, orbit, homing, sine/wobble, follow-path, delayed change, color/size/speed/rotation
  over life (curves), kill-on-condition, collide.
- **Script hooks:** a `foreach` over an emitter's pool runs custom behavior after the native
  modules each tick (see LANGUAGE.md §5). `on_spawn` / `on_death` / `on_hit` signals cover
  events.
- **Rendering**, per emitter:
  - SDF shapes (default; instanced via the SDF batcher);
  - vector meshes (any imported SVG, instanced);
  - sprites, including flipbook animations;
  - trails/ribbons: each particle leaves a stroked path, using the runtime tessellator.
- **Collision** (gameplay mode only) is opt-in per emitter: particles join a collision
  layer as circles or capsules in the spatial hash (§4.7). Hits emit
  `on_hit(particle, target)` in a deterministic order.
- **Budget targets:** gameplay mode 50k particles at 60 Hz on a mid-range desktop and 10k
  on the web; visual mode 1M+ on the Full tier once GPU compute exists (planned).

**Clarified (Phase 4A, see `cuervo/particles/README.md`):**
- *Tick order* of a `ParticleSystem`: simulate every emitter in id order (integrate,
  native modules in stack order, lifetime, `foreach` hook, swap-remove dead), then run
  every emitter's spawners in id order, then rebuild the collision world (particles in
  emitter/index order, then the game's shapes) and resolve hits. Simulation is
  per-emitter independent (the job-system split point); spawning was serial because
  gameplay emitters shared the one simulation RNG (superseded in milestone 5.5: every
  emitter has its own child stream, §4.15).
- *Visual mode* runs on the CPU (GPU compute on the Full tier is planned), uses its own
  seeded RNG and never collides (collision is forced off).
- The `foreach` hook may kill but not spawn; scripts spawn with `emit` from `update()`.
- Events (`on_spawn` opt-in, `on_death`, `on_hit`) are kept per emitter until its next
  tick, in the order they happened; hits are sorted by (emitter, particle proxy,
  target proxy).
- Collision is discrete (positions after the move); fast shots should use capsules.
  Capsule colliders point along the velocity by default (no trig), or along `rotation`.
- Particle collision proxies carry a `user` value with bit 63 set (emitter id + pool
  index); game shapes leave bit 63 clear.

### 4.7 Collision
- Shapes: circle, capsule (lasers), AABB, OBB for hitboxes.
- Broadphase: uniform spatial hash, rebuilt every tick (cheap with SoA, no persistent tree).
- Typical queries: gameplay particles vs. hitboxes (e.g. shots vs. the player), overlap
  queries, proximity radius (e.g. graze).
- Results come back in a deterministic order.
- **Clarified (Phase 4A):** circles and capsules are implemented; AABB/OBB hitboxes are
  planned. 32 layers, each proxy on one layer, queries take a layer mask. Touching shapes
  overlap. The hash is a stable counting sort into power-of-two buckets (no unordered
  containers); results are sorted by proxy id (insertion order), and `pairs(A, B)`
  reports a pair whose proxies are both in A and B once, as (lower id, higher id).
  Proxies spanning more than 1024 cells go to a list every query scans (replaced in
  milestone 5.5 by a coarse secondary hash, §4.15).
- General rigid-body physics is a separate, optional module (implemented, 9.6).

### 4.8 Input
```
action shoot = [Key.Z, Pad.A]
action focus = [Key.Shift, Pad.RB]
axis   move  = [Keys.Arrows, Pad.LeftStick]
```
- Scripts read actions (`Input.held(#focus)`), never raw devices.
- Each tick's input is packed into an **input frame**. A replay is `seed + input frames`.
  **Clarified (Phase 4):** the frame also carries the mouse pointer in virtual pixels
  (for UI); project bindings gained `Pad.LeftStickUp/Down/Left/Right` (stick as button).
  **Clarified (Milestone 9.3):** the frame also carries the character typed that tick
  (`InputFrame::text`, one code point; replay format version 2, docs/REPLAYS.md).
- Bindings are rebindable at runtime and saved per player.
- **Clarified (Phase 6B):** replay files (seed + frames + per-tick state hashes) are
  `cuervo/input/replay.hpp`, documented in docs/REPLAYS.md.

### 4.9 Audio
- miniaudio, with buses (master, music, sfx, ui, voice), per-bus volume and simple effects.
- Voice limiting per sound (e.g. at most 8 instances of `hit.wav`; the oldest is stolen).
- Music is streamed.
- Sounds are triggered from the simulation by tick, so replays sound identical.

**Clarified (Phase 4, `cuervo/audio/`, `cuervo/script/prelude/audio.plm`):**
- miniaudio (pinned 0.11.25) is used for **decoding and the output device only**; the
  engine mixes itself (`audio/mixer.hpp`), so mixing rules are ours and unit-tested.
  Mixer format: stereo f32 at 48 kHz; the device converts. Vorbis via miniaudio's bundled
  stb_vorbis.
- **Decoded vs streamed:** SFX decode fully at load (converted to the mixer format).
  "Streamed" music keeps the *encoded* file in memory and decodes incrementally on the
  audio thread: no file I/O in the callback, memory = compressed size. Files under a
  project's `assets/music/` stream, loop and use the music bus; every audio file under
  `assets/` is registered by file stem (`assets/sfx/hit.wav` is `#hit`); scripts can
  register more with `Audio.load(path, id=, bus=, volume=, limit=, priority=, stream=)`.
- **Trigger path:** sim calls only record commands; `end_tick` hands the tick's commands to
  the audio thread as one lock-free batch (SPSC ring), so a tick's sounds start together.
  The sample offset at which a batch starts depends on the device callback timing; that
  jitter (≤ one callback) is accepted for the device backend.
- **No feedback:** nothing the audio thread knows (is a voice playing, its position) is
  readable from the sim. Voice ids are numbered by the sim in trigger order and returned
  even for unknown sounds; `Audio.volume/muted` return the sim-side settings. So a game
  makes identical decisions with or without a device or sound files (tested: same state
  hash). Consequently the "audio completion" awaitable of 5.9 is **not** provided; await
  `wait(seconds)` instead. Script pitch variation must not use `Random` (it would consume
  the sim RNG only because of audio); derive it from `Time.tick`.
- **Null backend:** no device and no thread; `end_tick` applies the batch and renders
  exactly `sample_rate / tick_rate` frames on the calling thread. Its output is a pure
  function of the trigger sequence (tested by hashing a shmup run). Used by tests, when no
  device opens, with `--no-audio`, `CUERVO_AUDIO=null`, or `SDL_VIDEO_DRIVER=offscreen`.
- **Mixer math:** gain = fade × voice volume × sound volume × bus × master. Pan is
  constant-power, normalized to unity at center (`√2·cos/sin((pan+1)·π/4)`, via dmath).
  Pitch = playback rate (0.125–4), linear interpolation. Output hard-clamped to [-1, 1].
  Stops and volume/bus changes without a fade still ramp over ~5 ms (declick).
- **Voice limiting:** global pool (default 32 voices). A sound at its `max_instances`
  (default 8, 0 = none) steals its own oldest voice; otherwise a free voice; otherwise the
  weakest voice is stolen — fading-out first, then lowest priority, then oldest — unless it
  outranks the new sound, which is then dropped. Stolen voices cut immediately. Music gets
  a very high priority.
- **Music:** one track at a time; `Audio.music(#t, fade=)` on the playing track is a no-op,
  another track crossfades (`Audio.crossfade(#t, seconds)`).
- **Deferred:** per-bus effects (filters, reverb), 3D/positional attenuation, an
  `.import/` audio pipeline (files load directly for now), and sample-accurate
  tick-aligned scheduling on the device backend.

### 4.10 Game UI
Retained UI nodes in the scene tree: `Panel`, `Label`, `Button`, `Image`, `Slider`, and
layout containers (`HBox`, `VBox`, `Grid`, `Margin`). Anchors, gamepad focus navigation,
themes. Rendered with SDF primitives and MSDF text.

**Clarified (Phase 4, `cuervo/script/prelude/ui.plm`):**
- UI nodes are written in the language like the rest of the node tree (§4.3): `Control`
  (extends `Node2D`: anchors, offsets, `min_size`, `expand`, `disabled`, a computed
  rect), containers `VBox`, `HBox`, `Margin`, `Panel` (a themed Margin), `Center`, and
  widgets `Label`, `Button`, `Toggle`, `Slider`. `Image` and the grid container were added in 9.3 (4.17).
- **Layout** runs in the simulation, every tick, before the UI root updates (so focus
  and hit tests see this tick's rects): a `Control` whose parent isn't a Control is a
  UI root placed in the virtual screen. Anchors/offsets are Godot-style; a control never
  shrinks below its combined minimum size and grows around a coinciding anchor.
  Containers ignore their children's anchors. Text is measured from the font atlas
  (`UI.measure`, CPU layout), not the GPU, so layout is deterministic; the runner loads
  the font before the game starts (headless hosts without a font use a fixed estimate).
- **Focus** (one global `UI.focused`): actions `ui_up/down/left/right` (with key repeat
  after 24 ticks, every 6), `ui_accept`, `ui_cancel` and `ui_click`, bound by the project.
  Navigation is geometric within the focused control's UI root (nearest in the
  direction, perpendicular distance weighted ×2, ties by tree order), wrapping to the far
  end. Each input is consumed by the first root that uses it in a tick; `UI.modal`
  restricts input to one subtree; `UI.consume_input()` drops the tick's input when a
  menu opens. Sliders take left/right while focused.
- **Pointer:** the mouse position is part of the input frame (§4.8, `InputFrame::pointer`,
  whole virtual pixels), so pointer use replays. Hover focuses (only when the pointer
  moved), click activates, sliders follow drags.
- **Themes** are one small `Theme` class (colors, three font sizes, padding, separation,
  radius, border): the nearest `theme` up the parents, else `UI.theme`. Labels pick a
  semantic `style` (`#normal`, `#title`, `#small`, `#dim`, `#accent`, `#danger`, `#gold`).
- **Draw order / layers:** `CanvasLayer` nodes are drawn after the rest of the tree, in
  `layer` order, and `Canvas.new_layer()` flushes shapes *and* text between layers (the
  SDF batcher and text renderer flush incrementally), so a menu covers HUD text.
- **Pause:** `Scene.paused` stops `update()` for nodes that don't have
  `always_process` (inherited from ancestors), freezes their coroutines (their `wait`
  deadlines move with the pause; `VM::tick_coroutines(frozen)`), and stands gameplay
  particles still. Free-function coroutines count as gameplay and freeze too.
- `Scene.quit()` asks the runner to close; the runner's Escape-quits shortcut is off when
  the project binds `Key.Escape` to an action.

### 4.11 Assets
- Source files (SVG, PNG, WAV, OGG, fonts) are imported into engine formats in `.import/`.
- Importers produce tessellated meshes, MSDF atlases, raster atlases and audio in the
  formats each target needs.
- Hot reload: a file watcher re-imports changed sources and swaps them in by handle.
- A release export packs everything into a single archive per platform.

**Clarified (Phase 7C, `cuervo/assets/`, `tools/cuervo_tools/export.hpp`, docs/EXPORT.md):**
- The archive is `game.pak`: a 64-byte header (magic `CPAK`, format version,
  platform tag, FNV-1a 64 content hash), a path table sorted bytewise and
  16-byte-aligned stored data. Packing is deterministic (same inputs, same
  bytes; no timestamps). The runtime refuses a pack whose hash, ranges or
  version don't check out. It is memory-mapped on POSIX and read into memory elsewhere.
- Entries are **stored uncompressed**: no compression library is pinned, the
  payload is mostly compact engine formats, and web servers compress downloads.
  A per-entry compression code is reserved for later.
- Every game file goes through one VFS (`assets::Vfs`). In development it is the
  project directory. In release it is the pack. Saves are not game data:
  `Scene.save` writes to the project in development and to the per-user directory
  (`SDL_GetPrefPath("Cuervo", <game>)`, 7.2 "Save data"; `CUERVO_USER_DIR`
  overrides) in release, where a saved scene shadows the packed one. Hot reload
  is development-only.
- `cuervo-tools pack` / `export` run the release pipeline: tier check gate →
  collect (hidden files never; `--strip` drops replays and editor command
  scripts) → import (TTF/OTF → `.efont`, SVG → `.evec`; audio ships as is because
  miniaudio decodes it everywhere) → scripts → engine entries
  (`.cuervo/default.efont`, `.cuervo/export.toml`) → pack → assemble. The
  `.import/` cache is never packed: export re-imports.
- Scripts ship as **sources** (`scripts = "source"` in the manifest) or AOT-compiled C
  (`native`, 7B, 5.12). `pluma/` has no bytecode serialization, so there is no
  precompiled-bytecode form (`--scripts bytecode` is refused).
- Linux: the player is copied under the game's name and finds `game.pak` next to
  itself; `--pack <file>` overrides. Web: a game-independent runtime
  (`CUERVO_WEB_PACK=fetch`, preset `web-pack`) fetches `game.pak` into MEMFS
  before `main()`. Windows export is refused as untested unless given a Windows
  player.

**Clarified (Milestone 7.5, `cuervo/script/features.hpp`, CMake `CUERVO_FEATURE_*`):**
- Engine subsystems are build options: `CUERVO_FEATURE_AUDIO`, `_UI`, `_PARTICLES`,
  `_COLLISION`, `_DEV` (demos and dev tools in the runtime), all ON by default. A
  disabled feature drops its sources, its script prelude (`cuervo/<feature>.plm`) and its
  natives, and the dependency it alone needs (miniaudio). Particles require collision; the
  dev tools require particles; the editor (`CUERVO_BUILD_EDITOR`) requires every feature
  and is hidden, not overwritten, while one is off. All builds link with
  `--gc-sections` (`-dead_strip`, `/OPT:REF`) over per-function sections.
- `detect_features` decides what an exported project needs: a feature is used when a
  script names a top-level declaration of its prelude (lexed identifiers, so comments
  and string text don't count; `{...}` interpolations do), a scene's node or field type
  is one, or (audio) an audio file is under `assets/`. `Collision` alone means collision
  without particles. Every optional prelude is embedded for this, so a stripped engine
  still reports what a project needs. `[features]` in `project.toml` overrides detection. Export writes
  the result to the manifest (`features`, `stripped_features`).
- Linux exports ship a **per-export player** (`tools/cuervo_tools/player.hpp`, docs/EXPORT.md
  "Stripped players"): `cuervo-tools export --engine auto|full|stripped` configures the
  engine sources with the detected features, `CUERVO_FEATURE_DEV`, the importers and (for
  compatible-tier games) Vulkan off, builds `runtime` in Release into a cache keyed by the
  feature set and toolchain, and runs it headless on the exported pack before shipping it.
  Measured (EXPORT.md "Sizes"): tictak -40%, bench -37%, shmup -20% against the full
  Release player. A player's state hash doesn't depend on its features: one built without
  particles or the UI hashes what those hold in a game that doesn't use them (an idle
  particle system's RNG fingerprint and tick count, the UI statics' initial values, at the
  UI prelude's place among the globals; `ScriptHost::state_hash`), so every stripped player,
  native or web, is checked against the full engine. AOT packs need the full script surface. Web exports build their player the same way with emscripten
  (`CUERVO_WEB_PACK=fetch`, verified in headless Chrome); Windows players
  build on a Windows host since 10.5a (docs/EXPORT.md).

  **Clarified (Milestone 8.9, `tests/workflow_smoke.cmake`):** the exporter's naming and
  launch contract is covered end to end by the workflow test: a project named `Workflow
  Game` ships as `workflow-game` (lowercased, spaces as `-`), the shipped directory alone
  runs from any working directory with a private user directory, exits
  0 after `--quit-after` or at the screenshot tick, and reaches the editor-play state hash.
  The state hash covers the tick, particles and numeric script globals, not node fields, so
  a workflow check that must notice a scene or script change needs a script global that
  depends on it.

### 4.12 Scene and project files
- Binary format: a header (magic, format version, schema hash), then tables.
- Every schema change bumps the version and adds a **migration**. Old projects always load.
- `tools scene dump file.scene` prints a readable text view. It is registered as a git
  `textconv` driver so `git diff` shows meaningful changes:
  ```
  # .gitattributes
  *.scene diff=scene
  # git config diff.scene.textconv "cuervo-tools scene dump"
  ```
- Project settings (`project.toml`) stay text.

**Clarified (Phase 4, `cuervo/scene/scene_file.hpp`, `cuervo/script/scene_bridge.hpp`,
docs/SCENES.md):**
- A scene file stores one node tree in pre-order: per node its class, its `name`, its
  parent index and its `@export` fields (LANGUAGE.md 9.13) as (name, declared type name,
  self-describing value). Values: bool, every number kind, str, name, vecN, plain enums
  (by variant name), structs (fields by name) and lists of these. Strings live in one
  string table. The schema hash fingerprints the (class, field, type) triples used.
- Versions: 1 = inline strings, unnamed nodes; 2 (current) = string table and node names.
  Old versions are decoded by their own reader and then pass through `migrations()`
  (v1→v2 names nodes after their class, numbering same-class siblings "Shape", "Shape2"); `cuervo-tools scene upgrade` rewrites a file.
- Script-side schema drift is handled like hot reload (§5.13): fields match by name;
  a field that no longer exists, is no longer exported or changed declared type keeps its
  default, with a warning. Unknown classes / non-Node classes are errors (a panic).
- `Scene.load(path)` builds the tree (field initializers, then `init()` if it takes no
  arguments, then saved fields, then `add_child` to the parent) and returns the root
  *outside* the tree; `ready()` runs when the caller adds it. `Scene.save(node, path)`
  skips `transient` nodes (children a script creates itself). Paths are relative to the
  project and may not leave it. Loading is part of the simulation and deterministic.
- `cuervo-tools scene dump | build | upgrade` (tools/cuervo_tools). `dump` is the
  textconv view: nested `node Class "name" { field: type = value }` blocks, floats in
  shortest round-trip form, so equal scenes dump to identical text; `build` parses that
  text back (the text authoring path; the editor is the main one).
- Hot reload (§5.13) does not re-read scene files; instances loaded earlier keep their
  (migrated) state. F6 (restart) picks up edited scenes.

  **Clarified (Milestone 8.7, `tests/examples_sweep.cmake`, `tests/editor/editor_87_tests.cpp`):**
  - *The sweep.* Every example runs the whole release path in one ctest each
    (`examples_sweep_<name>`): a headless run to a screenshot tick on 1 and 4 job threads
    (same state hash), every scene through `scene dump | build` and `scene upgrade` (same
    bytes), a recorded run verified by `cuervo-editor --replay-verify` on 1 and 4 threads
    (a drifted project refused), a scripted edit plus forced hot reload while playing and
    recording, and bytecode and `--aot` exports launched from another directory with the same
    input and the same hash (`-DCUERVO_SLOW_TESTS=ON` adds the stripped player).
  - *Migration names.* Version 1 nodes had no names; the migration now numbers siblings of
    one class ("Shape", "Shape2") the way the editor numbers new nodes, so every migrated
    node has a unique path.
  - *Every node class.* A test adds every built-in node class to one scene with every field
    changed, saves it, reopens it, goes through the text view and runs and draws it.

### 4.13 Editor
A standalone Dear ImGui application built on the engine:
- Scene view with vector-aware zoom.
- Node tree, inspector, asset browser, input-map editor.
- Play-in-editor with hot reload.
- Replay viewer and debugger panel.
- Tier migration report.

**Clarified (Phase 6A, `editor/`, editor/README.md):** Phase 6 is split: 6A is the editor
shell (this note), 6B adds the asset browser, input-map editor, play-in-editor with hot
reload, the replay viewer/debugger and the tier report as panels in the same registry.
- `cuervo-editor` (CMake target `editor`) is desktop-only; the web build never
  configures `editor/`. Dear ImGui was first pinned to the plain release v1.92.9b with
  the 7.2 layout as a fixed arrangement of docks (`PanelRegistry`, panels in the same dock
  become tabs); milestone 8.3 switched it to the docking branch, `v1.92.9b-docking` (§4.16).
  1.93.0 was two days old with a reworked draw-list renderer.
- ImGui renders through the RHI (`editor/imgui_rhi.cpp`), shaders through the shader
  pipeline, input through `imgui_impl_sdl3`. It needed nothing new from the RHI:
  16-bit indexed draws, per-draw scissor and RGBA8 texture updates already existed on
  both backends, and `ImDrawCmd::VtxOffset` is replaced by vertex-buffer offsets.
- **Scene view without render-to-texture.** The engine renders the scene straight into
  the window inside the view's rectangle (viewport + scissor) from an ImDrawList
  callback, between ImGui's draws, so overlays drawn after it sit on top. RTT was not
  added: on Vulkan the swapchain pass resolves MSAA with its samples discarded, so an
  offscreen pass mid-frame would need a second MSAA target and resolve per backend, and
  the sub-rectangle gives the same pixels (the scene keeps the device's MSAA).
- **The preview is not the simulation.** The view instantiates the edited document as
  live script nodes in a `ScriptHost` that never ticks (`load` with no main class only
  compiles; `set_root` without `ready()`): field initializers and a zero-argument
  `init()` run, then one layout pass for UI roots; `ready()`, `update()`, coroutines,
  particles and the RNG never do. It is rebuilt from the document after each edit.
- **Reflection** is the scene system's: node classes are the compiled classes extending
  `Node`, fields their `@export`s (`RecordRT::field_exported`), defaults what
  `save_tree` records for a node built from an empty file entry. The editor stores the
  fields the file had plus those edited (a new node stores none, so it follows its
  class's defaults), which keeps "open + save" byte-identical.
- **Undo** is a command stack over a node-id tree (`editor/commands.hpp`); continuous
  edits merge into one step until released. Moving a UI control shifts its offsets.
- **Headless:** `--script-commands` applies editor operations from a text file (format
  in editor/README.md) and saves; `--screenshot` captures the whole window.
- **project.toml `main_scene`** (new key): the root is that scene file instead of an
  instance of `main` (`HostOptions::main_scene`), so a level built in the editor runs
  without a main script: `examples/editor_level` (its `level.cmds` is the level as
  editor operations; the `editor_level` test rebuilds it with `cuervo-editor` and runs
  it with `cuervo`). `main_scene` must stay inside the project.
- Found on the way: `Canvas.rect` passed `rotation` as the corner radius and read its
  rotation past the end of the call; fixed (rounded UI panels now really are rounded).

**Clarified (Phase 6B, `editor/`, editor/README.md, docs/REPLAYS.md):**
- *Play-in-editor runs the runtime's code.* The simulation half of `cuervo --game` moved
  out of the runner into `script::GameSession` (`cuervo/script/game_session.hpp`: project,
  input map, scripts, sounds, the font UI layout measures with, the `ScriptHost`, input
  sampling with `--press`, hot reload, recording). The runtime's frame loop, the editor's
  `PlayController` and the replay tools all drive it, so for the same input the editor's
  state hash is the runtime's (shmup at tick 240: `a3e57e79964421d3` idle; the
  `editor_replay` test compares a scripted run). The game gets its own VM and host: Stop
  drops them and the edited document, its undo history and the preview are untouched.
  F5 plays the project's main like the runtime; F6 plays the open scene as edited
  (`HostOptions::main_scene_doc`, unsaved changes included). The game is drawn in the
  scene view letterboxed; it reads the devices only while the view is focused.
- *Replays* are the 4.8 definition made a file (`cuervo/input/replay.hpp`): seed,
  action/axis names, start (project main, or the played scene embedded), per-tick input
  frames, and the state hash after every tick, so playback verifies itself and reports
  the first mismatching tick. `cuervo --game --record <file>` and the editor record them;
  `cuervo-editor --replay-verify <file> [--jobs N]` checks one headless. There are no VM
  snapshots, so the viewer seeks backwards by re-simulating from the start. A replay
  refuses a project whose actions/axes moved (bits would change meaning); script changes
  show as hash mismatches.
- *Panics in the editor* (5.8): the game pauses and the Debugger shows the stack trace
  with each frame's locals. The VM gained the minimal API for it: `Proto::locals` (named
  locals by register and pc range, emitted by the compiler) and `PanicInfo::frames`
  (function, file:line, live locals rendered by `VM::describe`, which never runs script
  code), captured before unwinding; `ScriptHost::last_panic()` keeps it. A hot reload that
  fixes the script lets Play resume.
- *Scene instancing* had no representation in the scene format; instead of a format
  change, the prelude gained `SceneInstance` (a `Node2D` with `@export var scene`): when it
  enters the tree it loads that scene and adds its root as a transient child, moved by the
  instance's `position` when it is a `Node2D` (nodes drew in world coordinates before 9.1;
  with the transform hierarchy on, the root is an ordinary child, 4.17). Saving keeps the reference, not a copy; the editor's view expands
  instances; a scene that instances itself panics. Dropping a `.scene` into the view
  creates one.
- *Input map editing* keeps project.toml as written: only the changed `key = [...]` lines
  of `[input.actions]`/`[input.axes]` are rewritten (trailing comments kept), new entries
  go after their section's last entry; everything else stays byte for byte
  (`cuervo/script/project_edit.hpp`). Binding spellings gained `Mouse.Middle/X1/X2`,
  `Wheel.Up/Down`, `Pad.RightStick<Dir>` and `Pad.Left/RightTrigger` (all already in
  `Binding`). The input map has its own undo history in its panel.
- *Asset browser* polls the project folder once a second (like the script watcher, no
  inotify) and lists hidden-free trees; a changed script recompiles the class registry
  when not playing. The tier report panel shows `check_project_tier` with entries linked
  to their files.

  **Clarified (Milestone 8.7, `editor/command_script.cpp`, `cuervo/script/prelude/ui.plm`):**
  - *Hot reload from a command script.* `append-file <path> <text>` edits a project file and
    `reload` forces the playing game to hot-reload, so reload is testable headless. A reload
    re-runs field initializers of migrated objects, so a project with coroutines or emitter
    fields is not bit-exact across it; a replay recorded across a reload plays on with the
    scripts as they are then (§5.13), and only reload-free recordings are verified.
  - *Scene view.* The preview builds scenes without `ready()`; a `TabContainer` has no tab
    bar there and now lays its pages out instead of panicking.

  **Clarified (Milestone 8.9, `editor/main.cpp`, `editor/command_script.cpp`):**
  - *Project creation without a window.* `cuervo-editor --new-project <dir> [--template
    empty|walker] [--name <name>]` runs `create_project()`, the code behind the welcome
    screen's New project, then exits. It is headless,
    and it refuses a folder that already has files.
  - *Hash from a script.* `print-hash` logs `at tick N (state hash H)`, the runtime's wording,
    so a test hands the editor-play hash to the exported game's run.
  - The fresh-checkout path (new project, edit and save a scene, play, add a script, hot
    reload, export, launch outside the project) is `workflow_smoke`.

### 4.14 Threading
- Scripts run on the main thread, in a fixed order.
- A job system runs particle simulation, collision broadphase, animation, tessellation and
  render preparation. Jobs write to disjoint ranges and merge in a fixed order, so
  results stay deterministic.
- On the web the job system runs inline on one thread by default. WASM threads are opt-in
  (`CUERVO_WEB_THREADS`, presets `web-threads` and `web-pack-threads`, docs/WEB.md "Threads").

**Clarified (Phase 5, `cuervo/platform/`, docs/WEB.md):** the frame loop is callback-driven
(`platform::FrameLoop` / `run_main_loop`): natively a `while`, on the web
`emscripten_set_main_loop_arg`, so all per-run state lives in a heap object. The web
build has no threads by default (no `-pthread`: no SharedArrayBuffer/COOP/COEP; miniaudio
mixes on the main thread via Web Audio; the opt-in threaded build has a real job system), no file watching (the project is a read-only
preloaded MEMFS bundle) and no runtime font import. Code asks `platform::has_threads`
/ `has_file_watching` instead of testing `__EMSCRIPTEN__`. The WASM simulation matched
the native one bit for bit on the shmup (state hash at tick 240), consistent with 4.1.

### 4.15 Performance core (milestone 5.5)
Performance core requirements. Each item must keep the determinism rules (§3):
results are bit-identical regardless of thread count or whether a SIMD path is taken.
- **Activating the job system:** a real worker pool (`cuervo/jobs/`, fixed worker count,
  1 thread / inline on web). First users are the split points already chosen: per-emitter
  particle stepping (§4.6), collision hash build and query batches (§4.7), tessellation and
  render preparation. Jobs write disjoint ranges; merges happen in a fixed order.
- **Explicit SIMD (AVX2 / WASM SIMD128):** hand-written paths for `Vec<T,N>` (N = 2..4)
  and the hot SoA loops (particle integration and modules, hash cell computation, SDF
  instance packing), selected at build time (`-mavx2` builds, `-msimd128` on web) with a
  scalar fallback. No FMA and no approximate instructions (`rsqrt`, `rcp`), so SIMD and
  scalar results stay identical; tests compare them.
- **Sub-seeding the RNG:** the one simulation PCG32 (§3) derives independent child
  streams by a stable key (system, emitter id, job index) instead of sharing one stream.
  This lets parallel jobs draw random numbers without a serial order, and keeps one
  system's draws from shifting another's (e.g. adding a visual emitter must not change
  gameplay). Child seeds depend only on the parent seed and the key.
- **Secondary spatial hash:** a second, coarse-cell hash for large shapes (bosses, lasers,
  screen-wide hitboxes), replacing the "oversized proxies are scanned by every query" list
  (§4.7). Queries consult both hashes and merge results in the same deterministic order.

**Clarified (milestone 5.5):**
- *RNG streams.* `derive_stream(parent seed, StreamKey{system, id, index})`
  (`cuervo/core/rng.hpp`) mixes the seed and key with SplitMix64's finalizer into a PCG32
  state *and* stream selector. System keys are an append-only registry
  (`rng_stream::particles = 1`, `rng_stream::jobs = 2`). Every particle emitter, gameplay
  and visual alike, spawns from `derive_stream(system seed, {particles, emitter id,
  generation})`; `generation` counts `ParticleSystem::replace_emitter` calls so a reused
  id (scripts recycle freed emitters) doesn't replay its predecessor. The simulation
  stream itself (`ParticleSystem::rng()`) is now only the script-visible `Random`.
  Emitter ids are part of the key, so "adding an emitter doesn't shift others" holds for
  appended emitters; inserting one *before* others renumbers them. The old per-emitter
  `visual_seed` became `EmitterConfig::seed`, used only by standalone emitters.
  Each emitter's stream state is part of `state_hash()`.
- *Replay hash change (once).* Because gameplay emitters stopped drawing from the
  shared stream and the hash now includes the emitter streams, the shmup's state hash
  changed once with this milestone (tick 240: `13cd5d06434bf435` -> `a3e57e79964421d3`).
  The particle demo's gameplay was unchanged (same bullets, hits and grazes at tick 900:
  its gameplay patterns draw no random numbers), only its hash format.
- *Job system* (`cuervo/jobs/job_system.hpp`). Fork-join batches on a fixed pool:
  `thread_count` counts the calling thread, which works too (N threads = N - 1
  workers); tasks are claimed from an atomic counter, idle workers poll briefly then
  sleep on a condition variable. One batch at a time, started from the main thread; a
  batch started inside a task runs inline. Builds without threads (web) are always 1.
  Thread count: `--jobs N` > `CUERVO_JOBS` > project.toml `jobs = N` > auto
  (hardware threads, capped at 8). The demos read `--jobs` / `CUERVO_JOBS` only.
  A process-wide `jobs::default_jobs()` pool (1 thread until configured) is what
  `ParticleSystem` and `CollisionWorld` use unless given another (`set_jobs`, `nullptr`
  = inline); tests build their own pools to compare 1 vs N threads.
- *Split points wired:* per-particle work inside `ParticleSystem::simulate` (integrate,
  modules, lifetime) in ranges of 2048 particles across all emitters (finer than
  "per emitter": one big emitter would otherwise cap the speed-up; every module is
  per-particle, so any split gives the same bits); the finish phase (removal) per emitter,
  except that when any emitter has a `foreach` hook the whole finish runs serially in id
  order on the main thread (hooks are script code, and keep their old view of other
  emitters); spawning per emitter (own streams); collider registration in particle
  ranges into a pre-sized proxy block (`CollisionWorld::add_uninitialized` + `set_*`,
  same ids as before); the hash build as a parallel counting sort (per-chunk histograms,
  bucket-range prefix sums, per-chunk scatter, identical to the serial sort); hit queries
  per emitter (`pairs` into per-emitter vectors, recorded in emitter order); large
  `pairs()` queries split over driver ranges, concatenated and sorted; and particle SDF
  instance packing (`particles::draw_emitters`). Tessellation stays serial (no hot
  runtime tessellation yet); it gets jobs when morphing/canvas paths land.
- `CollisionWorld` columns only grow (`clear()` resets a count), so refilling 50k
  proxies a tick doesn't re-zero megabytes serially.
- *Secondary hash.* A proxy whose AABB spans more than 64 fine cells (> 8x8, e.g.
  > 256 px at the demo's 32 px cells) goes to a coarse hash with cells 8x larger, built
  serially after the fine one (few proxies) with the same counting sort; queries visit
  the fine hash, then the coarse one, with the same "report from the first shared
  cell" de-duplication, and results stay sorted by proxy id, so every query answers
  exactly as before. A shape spanning more than 1024 *coarse* cells (wider than ~8000
  fine cells, i.e. absurd sizes) still lands in a fallback scan list, so no input can
  blow up either hash; it is empty in practice. Stats: `large_count()`,
  `huge_count()`, `coarse_entry_count()` (the old `oversized_count()` is gone).
- *SIMD selection.* CMake `CUERVO_SIMD` = `sse2` (default on x86-64: every x86-64 CPU
  has it, so distributable), `avx2` (opt-in; `-mavx2` is checked with
  `check_cxx_compiler_flag` and falls back to the default with a warning), `simd128`
  (the web default, `-msimd128`) or `none`. The flag applies to every first-party target,
  since inline `Vec` code must see one ISA. `-mfma` is never passed. `cuervo/math/simd.hpp`
  wraps the ISA (8 lanes AVX2, 4 SSE2/WASM); SSE2 has no `roundps`/`pmulld`, so floor and
  the 32-bit multiply are emulated exactly. `simd::set_enabled` (`--simd off`) switches
  the SoA kernels back to the scalar loops at run time, for tests and comparisons.
- *What is SIMD:* `Vec<float, 2..4>` arithmetic, negation, min/max and dot (128-bit,
  constant evaluation stays scalar); particle integration, the lifetime check and every
  native module (`cuervo/particles/simd_kernels.cpp`, with SIMD `dmath` sin/cos/atan2 in
  `cuervo/math/dmath_simd.hpp` mirroring `dmath.cpp` step by step); hash cell ranges
  in `CollisionWorld::build`; SDF instance packing. Branches become selects of the same
  values; `std::min/max/clamp` are written as the selects the standard defines, so NaN
  and signed zero agree. `tests/cuervo/simd_tests.cpp` compares raw bits of SIMD and
  scalar results per kernel, and the replay tests compare whole-scene hashes.
- *Verified:* the shmup (ticks 240 and 1800, idle and playing) and the
  particle demo (~53k bullets, tick 900) give the same state hash at `--jobs` 1, 3 and 8,
  with SIMD on and off, in sse2, avx2 and none builds, and the shmup on WASM. Particle
  demo, ~52k bullets: 2.42 ms/tick scalar single-threaded -> 1.80 (sse2) / 1.71 (avx2)
  with SIMD -> 0.50 / 0.47 at 8 threads (`cuervo/particles/README.md` has the table).


**Clarified (Milestone 8.6, `tools/bench.py`, `docs/PERFORMANCE.md`, `docs/perf/`):**
- *Baselines.* `tools/bench.py record` runs `examples/bench` at 1k/10k/50k/100k per backend and
  build, three runs each, and stores the median frame-time statistics, the simulation time per tick
  (mean and p95), peak RSS, the state hash at tick 600 (with the default job count and with
  `--jobs 1`) and the export sizes in `docs/perf/baseline-<machine-id>.json` (schema in
  PERFORMANCE.md). The `--bench` log line gained the p95/max tick time and the tick count.
- *Regressions.* `tools/bench.py compare` exits 1 on a changed state hash (or a hash that differs
  between backends), simulation tick time +15% (mean) / +25% (p95), frame p95 +25%, peak RSS +10%,
  export size +5%, capacity -15% (CPU limit) / -25% (frame limit). The thresholds live in one table
  in the tool; timings are only enforced on the same machine id. **Decision:** CPU-only checks are
  the strict ones; anything that includes the GPU or the driver is looser.
- *Capacity.* `tools/bench.py capacity` searches for the largest particle count whose p95 frame
  time (and, separately, p95 sim tick time) fits 16.67 ms with VSync off, on a dense scene
  (`examples/bench`) and a low-overdraw one (`examples/bench_spread`), reported as a range with the
  binding limit named. The figures are facts about the measured machine, not engine promises.
- *Measured.* On the reference machine (Ryzen 7 5800X3D, RTX 3060 Ti) the 50k and 100k workloads
  hold 60 fps in debug and release on GL and Vulkan with wide margins, and the release build holds
  about 1.5 million (GL) / 1.8 million (Vulkan) particles at 60 fps (after the GL frame limit and stream-buffer fixes, PERFORMANCE.md "Findings"). Web (WebGL2) is measured by hand
  (PERFORMANCE.md "Web"); no web baseline is committed (superseded by 10.4).

**Clarified (Milestone 10.4, `tools/bench.py --web`, PERFORMANCE.md "Web"):**
- *Browser runs.* `tools/bench.py record|capacity --web <runtime dir> --backends webgpu webgl2`
  runs `examples/bench` in headless Chrome on the real GPU through `tools/web/smoke_test.mjs`
  and stores it under `builds.web` of the machine's baseline; `compare` treats it like any build.
  The state-hash runs use the new `--screenshot-tick` flag (a page has no environment).
- *Cliffs.* **Decision:** "stable" means no frame-time cliff: the runtime logs each second's mean
  frame time (`bench windows |`), `record` keeps the worst run's slowest window over its median
  window, and `compare` fails any record above 1.5 (an absolute limit, not a delta).
- *Fixes it needed.* Uncapped web frames run on `setImmediate` (WebGPU was held to the 60 Hz
  animation frame, WebGL2 to the 4 ms `setTimeout` clamp); the web frame clock is
  `performance.now()` (SDL's advanced in whole milliseconds there); and the WebGPU backend keys
  its pass state and bind groups on buffer serials and resource ids instead of object pointers,
  which a released buffer hands to the next one (a grown stream buffer stayed bound at its old
  size: a validation error natively and in the browser, now in the `rhi_parity` test).
- *Measured.* On the reference machine the threaded web runtime runs 100k shapes at 390 FPS on
  WebGPU (p95 3.9 ms) and 210 on WebGL2, every workload reaches its native state hash, the worst
  cliff ratio is 1.21, and a second record compares clean against the committed one.

### 4.16 Polish milestones (8.1-8.4)
The brand, a node library, an editor overhaul and
real documentation, as four milestones in that order (each later one uses the earlier).
- **8.1 Brand integration.** The logo and design style come from the Cuervo design kit at
  `/mnt/storage/CuervoSpine/design/` (its own git repo): the Cuervo Mark and wordmark
  (`logo/*.svg`, `logo/png/`), the palette (Graphite neutrals with "Bone" `#F6F5F2` and
  "Raven" `#111214`, Ember as the single accent, Sage for success), and the type
  (Outfit display, Figtree UI, JetBrains Mono code). `tokens/tokens.css` is the kit's
  source of truth and `tokens.json` is generated from it. Only the visual identity applies;
  the kit's product copy and CuervoOS flows are for another product. Approach:
  - The kit lives outside the repo, and exports and the web build need the files in-tree,
    so a sync script copies the logo SVG/PNGs and `tokens.json` into `assets/brand/` and
    records the kit's source commit. The copy is regenerated by the script, never edited.
  - Colors reach C++ as a generated header (from `tokens.json`), with light (Bone) and dark
    (Raven) themes, so the editor never hard-codes a hex value.
  - Fonts are SIL OFL. The kit ships only `.woff2`, which Dear ImGui's stb_truetype can't
    read, so the TTFs are fetched in `cmake/Dependencies.cmake`, pinned to exact releases.
  - The mark is drawn with the engine's own SVG importer (§4.5) wherever it is shown in a
    scene, rather than as a raster. The PNG/ICO exports are the window and app icons.
  - Uses: the editor's window icon, title bar and welcome screen, the `cuervo` runtime's
    window icon, the README, and the docs site (8.4).

  **Clarified (Milestone 8.1, `tools/sync_brand.py`, `cmake/Brand.cmake`, `cuervo/brand/`,
  `editor/theme.*`, `editor/splash.*`):**
  - *Sync.* `python3 -I tools/sync_brand.py` copies 12 logo SVGs, 9 PNG exports and
    `tokens.json` from the kit (default `/mnt/storage/CuervoSpine/design`, or `--kit`,
    `$CUERVO_DESIGN_KIT`) into `assets/brand/`, writes its README, and records the kit commit
    and each file's SHA-256 in `SOURCE.txt`; it refuses to copy files the kit has uncommitted
    changes to, and removes anything else in the folder. `--check` compares with the kit;
    `--verify` (the `brand_assets` ctest) needs no kit and fails when a file no longer
    matches `SOURCE.txt`, so a hand edit is caught. The kit's previews, `tokens.css` and
    build scripts are not copied.
  - *Tokens are generated at configure time, not checked in.* `cmake/Brand.cmake` reads
    `assets/brand/tokens.json` with CMake's `string(JSON)` (no Python in the build) and writes
    `cuervo/brand/brand_tokens.hpp` into the build tree: the whole palette as
    `brand::palette::<ramp>_<step>` (plus `bone`, `raven`), the 25 semantic colors of the
    kit's `tokens.css` as `ThemeColors brand::light` / `brand::dark` (`rhi::Color`, sRGB
    straight alpha, 7.2 "Color"; `as_array()` and `0xRRGGBBAA` arrays for tests), and the three
    type families. `{color.x.y}` references are resolved; a theme missing one of the 25 colors
    fails the configure. tokens.json stays the only copy, so nothing can drift from it.
  - *Window icon.* `app-icon-512.png` is embedded in the engine (`cuervo/brand/app_icon.inc`,
    25 KB, so packs and exported players need no file) and `Window::create` sets it for every
    window (`WindowDesc::brand_icon`, `Window::set_icon_png`) with SDL 3.4's `SDL_LoadPNG_IO`:
    `cuervo` and `cuervo-editor` both. With an SDL older than 3.4 (the fetched 3.2.10
    fallback) there is no PNG loader and no icon. A game-chosen icon is not supported yet.
  - *Editor theme.* `--theme light|dark` (default: the OS preference, `SDL_GetSystemTheme`)
    and *View > Theme*. `editor/theme.cpp` maps the semantic tokens onto every ImGui color
    (Ember only for selection, checks, sliders and the active tab's overline; 4 px radii);
    the scene view's surround, grid and screen frame and the panels' status colors (console
    levels, tier report, replay, debugger, inspector, asset kinds) use tokens too. The move
    gizmo keeps its red/green axis convention (8.3 redoes gizmos). A minimal pass: the full
    token-driven style is 8.3.
  - *Fonts.* Outfit 1.1, Figtree 2.0.3 and JetBrains Mono 2.304, SIL OFL, fetched only with
    the editor (`cmake/Dependencies.cmake`, `URL_HASH` pinned, licenses in
    `third_party/licenses/`). ImGui's UI font is Figtree 15 px, the console JetBrains Mono
    14 px; `--font` now only sets the scene view's game font.
  - *Splash.* The "splash" of the done-when is the editor's (this section lists no runtime
    splash; a game's own splash belongs to the game). At start the editor covers the window
    with the theme's page color and the kit's horizontal lockup (mark + wordmark at 92% height,
    0.32 heights apart) for 1.4 s, then fades out; a click or key skips it, `--splash` holds
    it (screenshots), `--no-splash` disables it, `--screenshot` omits it unless `--splash`.
    The art is `mark-ember.svg` / `mark-ember-dark.svg` and `wordmark.svg` imported by the
    engine's SVG importer (4.5) and drawn by `MeshBatcher` on both backends, the wordmark's
    `currentColor` filled with the mark body's color; the caption is MSDF text in Outfit.
    Every logo SVG imports with nothing unsupported except `favicon.svg`'s CSS `<style>`
    (a browser favicon the engine never draws). The welcome screen proper is 8.3.
  - *Title bar and README.* Windows are titled "… — Cuervo Engine Editor"; README.md opens
    with the two-tone mark (light/dark via `<picture>`).
- **8.2 Node library.** Before this milestone the prelude had `Node`, `Node2D`, `SceneInstance`,
  `CanvasLayer` and the UI controls (§4.10); everything else was built in scripts. The new nodes
  are prelude classes extending `Node2D` that wrap existing systems, so the editor and
  scene files pick them up through the usual reflection (`@export` fields, §4.12, §4.13).
  The nodes: `Shape` (SDF primitive: circle, rect, rounded rect, capsule, with fill and
  stroke), `Path` (polyline/bezier stroke), `Sprite` (an imported SVG), `Text` (MSDF text
  in the world), `Camera2D`, `Timer`, `Area` (collision shape with layers and signals,
  §4.7), `Emitter2D` (a particle emitter as a node, §4.6) and `AudioPlayer` (§4.9). Each
  one is deterministic in sim paths (§3) and has doctest coverage plus a scene-file
  round-trip test. Animation nodes are covered by milestone 9.4.

  **Clarified (Milestone 8.2, `cuervo/script/prelude/nodes.plm`, `collision.plm`,
  `particles.plm`, `audio.plm`, `cuervo/script/vector_cache.hpp`, docs/API.md):**
  - *Where they live.* Each node is in the prelude of its feature (§4.11 "Milestone
    7.5"): `Shape`, `Path`, `Sprite`, `Text`, `Camera2D` and `Timer` in the new core
    prelude `nodes.plm`; `Area` in a new `collision.plm` (compiled in with
    `CUERVO_FEATURE_COLLISION`), which `Collision` moved into from `particles.plm`,
    so a collision-only build now really has hitboxes; `Emitter2D` in
    `particles.plm`; `AudioPlayer` in `audio.plm`. Feature detection follows: an
    `Area` (or `Collision`) means collision, an `Emitter2D` particles. All extend
    `Node2D` and are configured only by `@export` fields (plain enums such as
    `ShapeKind`, `PathCurve`, `AreaShape`, `EmitPattern` for the choices), so
    scene files and the editor need no code for them.
  - *Hierarchy* (superseded by 9.1, §4.17): with `[scene] transform_hierarchy` on (the
    default) a node draws at its global position; with it off, at its own world `position`.
    The new drawing nodes apply their own `rotation` and `scale`. Nodes with nothing to show in
    the game (Camera2D, Timer, AudioPlayer, an Emitter2D before `ready()`, an
    Area without `debug_draw`) draw a faint marker while `ready_done` is false,
    i.e. only in the editor's preview, which never runs `ready()` (§4.13).
  - *Engine work runs before the node's own `update()`:* Timer, Area and
    Emitter2D override `update_tree` rather than `update`, so a script
    subclass that overrides `update(dt)` without calling super still works.
    Initialization that needs saved fields (Emitter2D's `Emitter`, `autostart`,
    `autoplay`, the current camera) is in `ready()`.
  - *Timer and Emitter2D count ticks:* periods are `max(1, round(seconds / dt))`
    ticks, so they fire on the same ticks in every run. `start` is a keyword, so
    the methods are `Timer.restart(time)` and `Emitter2D.restart()`/`burst()`.
  - *Area* registers its shape each tick (`Collision.area`, a circle or a capsule
    along `rotation`; the hitbox list gained capsules). After the particle step
    the host queries each area's shape on its `mask` in the built world (sorted
    results, §4.7), calls `area_touch(other)` per overlap and then
    `area_end_tick()`, which emits `area_exited` then `area_entered` in
    registration order. Detection is one-sided (an area sees the areas on its
    mask). Areas are hitboxes too: gameplay particles hitting their layer call
    `on_hit`, which emits `hit(tag, count)`. AABB/OBB shapes still wait for the
    collision world (§4.7).
  - *Camera2D* is chosen in the simulation (`current` on `ready`,
    `make_current()`, cleared on `exit`) as `Camera2D.current_camera`;
    `Scene.draw_frame` passes it to the new render-only `Canvas.set_view`, and
    `CanvasLayer` subtrees draw in screen coordinates (`Canvas.reset_view`), so
    HUDs and menus don't move. `ScriptHost::draw(DrawTargets)` replaced the
    runner's fixed screen `world_to_ndc`: the host flushes each batch with the
    view it was drawn in (`virtual_to_ndc * view`, AA width divided by zoom).
    The editor's scene view keeps its own camera (`DrawTargets::cameras` off);
    play-in-editor uses the game's.
  - *Sprite* draws imported vector art through the new `Canvas.vector` and a
    `VectorCache` (one `VectorAsset` per project path, loaded through the VFS;
    `x.svg` falls back to the `x.evec` an export packs; tessellated at the zoom
    it is drawn at). The cache lives with the runner's/editor's `MeshBatcher`,
    not the host, because its mesh handles belong to the batcher. Shapes and
    meshes keep call order: the host flushes the SDF batch before a vector and
    the mesh batch before the next shape. Instances of one asset drawn at
    different zoom levels in one frame share one mesh (the last level wins).
  - *Path* strokes polylines and SVG-style cubic Béziers (`segments` pieces per
    curve) with SDF capsules: round joins for free, but a translucent color
    shows the overlaps at the joins. Canvas also gained `capsule`, `ring`, and
    `text(max_width =)` for `Shape` and `Text`.
  - A project needs no scripts any more: `read_scripts` accepts an empty
    `scripts/` (a `main_scene` of prelude nodes is a whole game).
  - *Verified:* `examples/node_level` uses every new node, is built from
    `level.cmds` by `cuervo-editor` (byte-identical to the checked-in scene), and
    the `node_level` test runs it headless to tick 240 (state hash
    `115a6d76bb125635`), round-trips it through `scene dump`/`scene build` to the
    same bytes and hash, and plays the scene loaded back from its file in the
    editor to the same hash. The hash depends on the nodes (moving the goal
    Area off the fountain's layer changes it). `tests/cuervo/node_tests.cpp`
    covers each node's behavior, determinism and a scene-file round trip.
- **8.3 Editor UI overhaul.** Rebuild the editor's look and workflow on 8.1's theme:
  token-driven ImGui style in light and dark, compact density as the kit's product UI
  guidelines prescribe, brand fonts, an icon per node class, a toolbar and command
  palette, transform gizmos (move/rotate/scale) in the scene view, multi-select, an undo
  history panel, a welcome/project picker, and saved layouts. Error messages follow the
  kit's formula: what happened, whether work is lost, what to do, with raw details one
  click away. **Decision: the editor moves to ImGui's docking
  branch** (`v1.92.9b-docking`), reopening §4.13's Phase 6A choice of the plain release:
  panels float, undock and redock inside the main window, the default layout is built
  with the DockBuilder API, and saved layouts keep ImGui's dock text. Multi-viewport (OS
  windows) is not needed.
  - **Welcome screen.** On first launch when no `--project` is given, the editor opens a
    welcome screen (`editor/launcher.*`): recent projects, opening a folder, and new
    project from templates. The license and third-party notices are viewable via *Help > License*
    (`editor/license_view.*`).

  **Clarified (Milestone 8.3, `editor/`):**
  - *Docking.* `cmake/Dependencies.cmake` pins
    `v1.92.9b-docking` with its SHA-256; `ImGuiConfigFlags_DockingEnable` is on and
    multi-viewport is off. Every panel is a window docked into one dock space under the
    toolbar. The default layout is built with `DockBuilder*` (Scene tree and Assets left,
    Scene View center, Inspector and Debugger right, Console, Replay, Input Map, Tier
    Report, Profiler and History bottom) from three size fractions, so the built-in
    layouts (Default, Debugging, Minimal) are only fractions plus hidden panels. A saved
    layout also stores ImGui's ini text (`SaveIniSettingsToMemory`: dock tree and window
    placement) in `editor-layouts.txt`, one escaped line, and applying it calls
    `LoadIniSettingsFromMemory` between frames. ImGui's own `imgui.ini` is disabled. The
    window's last arrangement is restored at start; screenshot and script runs write
    nothing per user.
  - *Style.* `editor/theme.cpp` sets every `ImGuiCol` and the style metrics from the brand
    tokens (14 px compact density, 4/8/16 px spacing, 2/4/8 px radii, docks square with
    hairline separators); `tests/editor/editor_ui_tests.cpp` fails if any color equals
    ImGui's dark, light or classic default. Icons are vector paths drawn with the draw list,
    not an icon font, so they stay sharp at any scale and need no extra third-party asset.
  - *Workflow.* Tools Q/W/E/R with move, rotate and scale gizmos acting on the whole
    selection as one undo step (flat `GroupCommand`s of `SetFieldCommand`s, merged while
    dragging; pivot is the centroid; rotation uses `dmath` sin/cos and positions round to
    1/1000), multi-select, the History panel, the action registry behind the menus,
    toolbar and the Ctrl+Shift+P palette (fuzzy search), and messages in the kit's formula.
    The command script has verbs for all of it, and `examples/editor_level/level.cmds` was
    redone with the palette and gizmo verbs; its scene is byte-identical to before, so the
    `editor_level` test's checked-in scene and state hash are unchanged.
- **8.4 Documentation.** This makes Phase 8's "docs" concrete: a user manual (editor,
  projects, export), a Pluma guide and tutorial built from `docs/LANGUAGE.md`, a node and
  engine API reference generated from the prelude's doc comments (`cuervo-tools docs`,
  reusing the LSP's symbol index, §5), and a "first game" tutorial that is checked by
  building it in CI. The output is a static site styled with the 8.1 brand, and a test
  fails on any public prelude symbol that has no documentation.
  **Clarified (Milestone 8.4, `tools/build_site.py`, `tools/site/`, `tools/cuervo_tools/api_docs.*`,
  `docs/PLUMA_GUIDE.md`):**
  - **The site generator is Python (stdlib only), not C++:** `python3 -I tools/build_site.py -o <dir>
    --font role=ttf ...` (CMake: `--target docs_site`, output `<build>/site`). The inputs are Markdown, JSON
    and fonts, nothing that needs the engine, and a C++ Markdown renderer would add code to a binary
    whose job is project files. `tools/site/mdlite.py` is the Markdown subset the docs use,
    `highlight.py` reads its Pluma keywords from `tools/vscode-pluma/syntaxes/pluma.tmLanguage.json`.
    Colors come from `assets/brand/tokens.json` (light, and dark under `prefers-color-scheme` or the Theme
    button), type from the three brand fonts (Outfit, Figtree, JetBrains Mono; the TTFs CMake already pins
    are copied into the site with their OFL texts), the mark and wordmark from `assets/brand`.
  - **Pages:** Manual (GUIDE sections 1-3, 6-9, editor, scenes, replays, export, web, Windows, VS Code), Pluma
    (five minutes, the new `docs/PLUMA_GUIDE.md` in chapters, LANGUAGE.md as the reference split by section),
    Tutorial (GUIDE section 5, one page per step, plus the finished files), API (docs/API.md split into one page per
    prelude file and per type, with an A-Z index and anchors per member), Design (DESIGN.md), About (the
    license, third-party notices, every license text). Relative links between documents are rewritten to site
    pages; a link that points at nothing is a build error. Search is a JSON index plus vanilla JS.
  - **Docs coverage:** `cuervo-tools docs --check-coverage` fails on any public (non-`_`) prelude type, constant,
    function, variant, signal, field or method with no leading or trailing doc comment
    (`api_docs_coverage`; `api_docs_tests.cpp`). Every public symbol is documented.
  - **Tests:** `docs_site` builds the site and checks every internal link and fragment in the output;
    `docs_site_unit` tests the renderer and runs every self-contained example of the Pluma guide through `plm`;
    `tutorial_tests` fails when a line of `examples/tutorial` is missing from GUIDE.md's code blocks (the
    guide lacked the `[scene]` table); `run_tutorial` and `export_tutorial` (source and AOT, shipped alone, hash
    `519ec4a631d8254d` at tick 240) cover the tutorial's build and export in CI.
- **8.5 Documentation reconciliation.** **Clarified (Milestone 8.5, `docs/STATUS.md`):**
  - *One place for support.* `docs/STATUS.md` states what is Stable, Experimental, Planned or
    Unsupported, by platform, rendering backend and system. The other pages describe how things
    work and link to it. It is maintained by hand until 8.8 turns it into a checked contract.
  - *How the documents relate.* The opening paragraphs of §4 and §5 are the design; the
    **Clarified** notes record what was built and win where they differ; §6 marks finished
    milestones "done". User-facing pages (README, guide, WEB, WINDOWS, EXPORT, editor, module
    READMEs, LANGUAGE) describe the current state and do not narrate milestones. `LICENSE.md` is
    the source of truth for license summaries.
  - *Style.* Status lines carry no dates; decisions are written **Decision:**; measurements may name
    machines and versions.

### 4.17 Advanced local systems (9.1-9.8)
Phase 9 holds what a Godot comparison found missing (transform hierarchy, more nodes, a
bigger Pluma library, more UI, animation, tilemaps, navigation, scene-tree physics) on top
of the original Box2D and lighting plans. The target games are shmups, platformers,
top-down RPGs, UI-heavy and puzzle games, and general 2D. 3D stays in Phase 13. The
sub-milestones build on each other in order, and each keeps replay hashes identical at 1
and N job threads (§3, §4.15). Nodes from 8.2 (`Shape`, `Path`, `Sprite`,
`Text`, `Camera2D`, `Timer`, `Area`, `Emitter2D`, `AudioPlayer`) are not repeated here.
- **9.1 Transform hierarchy and node gaps.** Before 9.1 nodes drew in world coordinates
  (scene.plm). `Node2D` gained parent-relative transforms (`global_position`,
  `global_rotation`, `global_scale`, `to_local`, `to_global`). **Decision:** it is the
  default but can be switched off (per project or per node), and a switched-off tree
  behaves exactly as before, so existing games and replays don't change. Adds `Line2D`,
  `Polygon2D`, `Marker2D` and `VisibleOnScreenNotifier2D`.
  **Clarified (Milestone 9.1, `cuervo/script/prelude/scene.plm`, `nodes.plm`,
  `cuervo/script/vector_cache.*`, `tests/cuervo/hierarchy_tests.cpp`):**
  - *Switches.* project.toml `[scene] transform_hierarchy` (default `true`;
    `HostOptions::transform_hierarchy`, read through the `Scene.transform_hierarchy()`
    native, which is a function and not a static because the state hash covers numeric
    globals) and the per-node `@export var top_level` (true: the node ignores its
    parent). Off, `position`, `rotation` and `scale` are world values exactly as in 8.2,
    and `global_*` equal the locals. The world-space script-driven examples (shmup,
    tictak, bench, tutorial, editor_level) set it to `false`; `node_level` keeps the
    default, and every recorded hash is unchanged either way.
  - *API.* `Node2D` gets `global_position()`, `global_rotation()`, `global_scale()`,
    `global_transform()` (a `NodeTransform`), the `set_global_*` counterparts that solve
    for the local value, `to_global(p)` and `to_local(p)`. They are methods, not properties,
    until the accessors of 9.2a land. A node inherits only from a parent that is a
    (non-Control) `Node2D`; `Control` never inherits (the UI lays itself out, §4.10).
    Composition is position + rotate(scale * local) with rotations added and scales
    multiplied component-wise, so shear (a rotated parent with non-uniform scale) is not
    represented: a documented approximation.
  - *Drawing and behavior* of every built-in node (Shape, Path, Sprite, Text, Camera2D,
    Area, Emitter2D, AudioPlayer, the new nodes) use the global transform. `SceneInstance`
    makes its root a child (so it inherits `position`) with the hierarchy on and offsets it
    by hand with it off.
  - *New nodes.* `Line2D` (polyline with width, color or per-point gradient, joint, closed)
    and `Polygon2D` (filled, concave allowed via libtess2, optional outline) draw through
    the new `Canvas.polyline` / `Canvas.polygon` natives into `VectorCache::draw_shape`
    (a per-frame slot cache of tessellated meshes on the mesh batcher, so both backends
    share it with no RHI change). `Marker2D` is an editor gizmo that draws nothing in the
    game. `VisibleOnScreenNotifier2D` emits `screen_entered` / `screen_exited` from a
    separating-axis test of its rectangle against the current camera view (the screen when
    there is no `Camera2D`).
  - *Editor.* Gizmos and picking use the global position, and a drag converts the world
    delta to the node's local space (undoing the parent's rotation and scale).
- **9.2a Pluma collections, strings and language.**
  - Collections: `Map<K,V>`, `Set<T>`, fixed arrays `[T; N]`, `StrBuilder`, `Deque` and a
    priority queue. Iteration over a `Map` or `Set` is in a defined order (insertion), so
    it stays deterministic.
  - `List`: `sort`, `sort_by`, `reverse`, `index_of`, `slice`, `extend`, `swap`, `first`,
    `last`, `map`, `filter`, `reduce`, `any`, `all`, `count`, `unique`, `shuffle` (with a
    seeded stream), `min`, `max`, `sum`, `zip`.
  - `str`: `split`, `join`, `replace`, `trim`, `to_upper`, `to_lower`, `find`, `substr`,
    byte/char access, `repeat`, `pad_left`, `pad_right`, `lines`, number formatting.
  - Conversions: `parse_int`, `parse_float`, hex/radix output. Serialization helpers
    (JSON, base64, hashing) come after, outside the sim path.
  - Language: working `get`/`set` accessors, `Result<T,E>` with `?` (§5.8), method values
    and first-class callables, `Optional` helpers (`unwrap_or`).
  - Each works in the VM and in AOT-to-C.
  - **Clarified (Milestone 9.2a).** Rules in LANGUAGE.md 9.16-9.19. `Map`/`Set` keys must be
    hashable: integers, `bool`, `str`, `name`, plain enums, classes (identity); floats,
    vectors, structs and optionals are rejected. Iteration is insertion order, and changing a
    map/set/deque during a `for` loop panics. `[T; N]` is a value type. `parse_int` and
    `parse_float` return optionals (not `Result`). `Ok`/`Err` take their type from the expected
    type; `?` works on `Result` and nullables and runs `defer`s. There are no tuples, so `zip`
    takes a combining function; `sort(less)` and `sort_by(key)` use the language's own stable
    merge sort (identical on every platform and in VM and AOT), floats sort NaN last;
    `PriorityQueue` is FIFO among equals. `shuffle` draws from `VmOptions::random_below` (the
    host's seeded stream), else the VM's own PCG32 seeded by `VmOptions::random_seed`; the
    engine installs the hook (9.2b). Accessors run outside their own body; method values
    capture the receiver when bound and dispatch virtually. Cut: built-in methods as values,
    `Optional` helpers on reference types (they are already nullable), the `[x; N]` repeat
    literal, and new `IndexSet` behavior for custom types.
- **9.2b Pluma math, geometry and random.**
  - Scalar: `trunc`, `fract`, `fposmod`, `cbrt`, `hypot`, `copysign`, `snapped`,
    `pingpong`, `ease` curves, `bezier`, `lerp_angle`, `angle_difference`,
    `rotate_toward`, hyperbolic functions, `is_nan`/`is_inf`/`is_finite`,
    `is_equal_approx`. Constants `E`, `INF`, `NAN`, `SQRT2`, `INT_MAX`, `INT_MIN`,
    `FLOAT_EPSILON`.
  - Integer: `gcd`, `lcm`, integer `pow`, `isqrt`, `divmod`, `popcount`/`clz`/`ctz`,
    checked, saturating and wrapping arithmetic.
  - Vectors: per-component `floor`/`ceil`/`round`/`fract`/`sign`, `snapped`, `bounce`,
    `move_toward`, `direction_to`, `limit_length`, `ivecN`, and `v[i] = e` assignment.
  - Types: `Rect2` and `Transform2D` (exposing `cuervo/math/affine2.hpp`).
  - Geometry: point in polygon, segment/segment and ray/circle intersection, closest
    point on a segment, polygon area, convex hull, triangulation (reusing libtess2).
  - Random and noise: Perlin and simplex noise, `hash`, `pick`, `weighted_pick`, Gaussian
    values, random points in a circle or on the unit circle, and a seeded local `Random`
    stream (sub-streamed per §4.15) beside the global one. Wall-clock time and unseeded
    randomness stay out.
  - **Determinism:** every transcendental function goes through `dmath` and is
    bit-identical across SIMD, scalar, VM and AOT, with a test for each.
  - **Clarified (Milestone 9.2b).** Rules in LANGUAGE.md 9.20-9.24. The library is **Pluma
    source** (`pluma/std/math.plm`), compiled before user code; the engine embeds the same file
    as the first prelude (`cuervo/CMakeLists.txt`) and the language tests load it with
    `pluma::add_math_prelude`, so `pluma/` still never depends on `cuervo/`. Determinism holds by
    construction: every function is made of IEEE-exact float operations, integer arithmetic and the
    existing transcendental built-ins (`sin cos atan2 exp log pow`), which `VmOptions::math` routes to
    `dmath` in the engine; the new built-ins (overflow arithmetic, vector methods, `v[i] = e`) are
    exact too. So SIMD (which only has the same `dmath` sin/cos/atan2 lanes, tested equal in
    `tests/cuervo/math_tests.cpp`), scalar, VM and AOT agree; `tests/pluma/aot_tests.cpp` runs every function
    in both the VM and AOT-to-C with equal output, `tests/pluma/math_tests.cpp` holds golden output for
    the exact subset, and `tests/cuervo/math_tests.cpp` compares engine-hosted results with `dmath`,
    `Affine2` and `Pcg32`/`derive_stream` directly. The shmup replay hash is unchanged.
    `int` is 32-bit, so the integer functions are for `int` (overflow arithmetic works on any one
    integer type); `ipow` not `pow`; `divmod` returns a `DivMod` struct (no tuples); `ivec2/3/4` are
    prelude structs (no built-in kind, no `ivec5+`, swizzles or indexing); `Rect2` and
    `Transform2D` are prelude structs whose `Transform2D` repeats `Affine2`'s operation order; the local
    stream is `Rng` (the name `Random` is the engine's static class), with `Random.pick`,
    `weighted_pick`, `gaussian`, `in_circle`, `on_circle` and `fork` beside the global stream.
    **Cut:** libtess2 triangulation (ear clipping covers simple polygons without engine code; holes
    and self-intersections are not supported), 3D simplex noise, `ivec5+`, and easings.net's hand-tuned
    in-out formulas (in-out is the symmetric construction). `shuffle` and
    the new `Random` helpers are wired to the engine's PCG32 (`ScriptHost::vm_options`).
- **9.3 UI widgets.** `GridContainer`, `ScrollContainer`, `FlowContainer`,
  `SplitContainer`, `AspectRatioContainer`, `Image`/`TextureRect`, `ProgressBar`,
  `LineEdit` (text input recorded in the input frame so it replays), `OptionButton`, tabs
  and popups, all through the focus and layout model of §4.10.
  **Clarified (Milestone 9.3, `cuervo/script/prelude/ui.plm`, `tests/cuervo/ui_widget_tests.cpp`,
  `examples/ui_menus`, `tests/ui_menus.cmake`):**
  - *Widgets* (all `@export`-configured, doc-commented, laid out in the simulation with the same
    `UI.measure` as before). `GridContainer` (`columns`, separations; columns/rows size to their
    widest/tallest cell, leftover space goes to the columns and rows holding an `expand` child),
    `FlowContainer` (`vertical`; wraps at its width/height, so its minimum size follows the size
    it was last given and settles one tick after a width change), `SplitContainer` (two
    children and a focusable bar: `split`, `bar_size`, `handle_step`; keys and dragging move it,
    children keep their minimum sizes), `AspectRatioContainer` (`ratio`, `stretch` #fit/#cover,
    `align`), `ScrollContainer` (`vertical`, `horizontal`, `wheel_step`, `follow_focus`,
    `show_bars`, `scroll`; signal `scrolled`), `Image` (vector asset like `Sprite`, `stretch`
    #keep/#scale/#keep_aspect/#cover, `tint`, flips, `ignore_natural_size`) with the alias
    `TextureRect`, `ProgressBar`, `LineEdit` (`text`, `placeholder`, `max_length`, `secret`,
    `editable`; signals `text_changed`, `text_submitted`), `OptionButton` (`items`, `selected`,
    `item_height`; signal `item_selected`), `TabBar` (`tabs`, `current`, `min_tab_width`),
    `TabContainer` (pages are its Control children, `titles`, `tab_min_width`), `Popup`
    (`modal`, `light_dismiss`, `cancel_closes`; `popup_centered`, `popup_at`, `close`),
    `PopupMenu` (`items`, `item_min_size`; `item_chosen`) and `ConfirmDialog` (`title`,
    `message`, `ok_text`, `cancel_text`, `focus_ok`; `confirmed`, `cancelled`). `Control` gained
    `tooltip` (shown after the focus rests 24 ticks, drawn above everything).
  - *Focus and input model.* Everything goes through §4.10's focus model. A control that
    `floating()` (a popup) is not laid out by its parent and is a UI root of its own, wherever it
    sits in the tree; popups set `UI.modal` (restored on close) and take the focus, which comes
    back on close, so focus is trapped. A root's input is "focus inside" by `root_control()`, not
    by ancestry. New actions, all optional: `ui_scroll_up/down` (bind `Wheel.Up/Down`, the right
    stick), `ui_backspace`, `ui_delete`, `ui_home`, `ui_end`, `ui_tab_prev/next` (shoulders). The
    wheel scrolls the area under the pointer, else the one around the focus, else (gamepad) the
    first scrollable area of the root; the first press moves `wheel_step`, held presses a quarter
    of it per tick. Dragging empty content or the bar scrolls too; focus moves scroll the
    focused control into view in the next layout. A `LineEdit` edits after `ui_accept` or a
    click; typed characters, backspace/delete, caret keys and `ui_accept` (submit) / `ui_cancel`
    (stop) are consumed by it. Up/down leave the field. Printable keys bound to `ui_*` actions
    would also type: a character typed on the tick of a `ui_*` press cancels that press, but
    projects with text fields should bind navigation to arrows/Enter/Escape. There is no
    on-screen keyboard yet, so gamepads can focus a `LineEdit` but not type into it.
  - *Rendering.* New natives `Canvas.set_clip` / `clear_clip` (scroll areas, `#cover`, text
    fields) cut everything drawn after them: the host flushes the batchers and the runner sets
    the existing RHI scissor (`Device::set_scissor`, already in both backends, so no RHI change);
    `UI.push_clip` / `pop_clip` intersect nested rectangles. `Scene.overlays` +
    `Node.draw_overlay` draw popups and tooltips after every canvas layer. The editor's scene
    view ignores clipping for now (its draw targets have no clip hook). `UI.image_size` reads a
    vector file's document size for layout.
  - *Input and replays.* `InputFrame::text` carries one typed code point per tick (the devices
    queue typed characters, so a burst is spread over ticks and none is lost); the window turns
    SDL text input on only while a field is being edited (`UI.wants_text`). Replay format
    version 2 stores it; version 1 files still load (docs/REPLAYS.md). `cuervo --game` gained
    `--pointer "tick:x:y,..."`, `--type "tick:text,..."` and tick ranges in `--press`
    (`"10-20:ui_click"`) so automated runs can drive pointers and text.
  - *State hash.* The hash covers every numeric static in declaration order, so the counters of
    the 9.3 input handling live in an object (`UI.x`), not in statics: the shmup, tictak and
    other recorded hashes are unchanged.
  - *Done-when.* `examples/ui_menus` (tabs, sliders, toggles, `OptionButton`, `LineEdit`,
    `ConfirmDialog`; a `GridContainer` of slots in a `ScrollContainer`, `SplitContainer`,
    `AspectRatioContainer` + `Image`, `FlowContainer`, `ProgressBar`, tooltips and a popup menu)
    runs three scripted scenarios (keyboard, gamepad, pointer; `tests/ui_menus.cmake`) whose
    recordings verify on 1 and 8 job threads, plus doctests that drive the same menus through
    real `DeviceState` key, pad button, stick, wheel, mouse and typed-text events. Every control
    there has an explicit `min_size`, so what a recorded click hits does not depend on the font
    (the verifying editor may have none). Cut: raster images (11.2a), IME composition, an
    on-screen keyboard, horizontal wheel/shift-wheel, selection and clipboard in `LineEdit`.
- **9.4 Animation.** `AnimationPlayer` with property tracks (any `@export` field) saved
  in scene files, `Tween`, and an `AnimationTree` state machine with blending. Time is
  measured in ticks (§5.9). Cutout, skeletal and morph animation from Phase 8 plug in here.
  **Clarified (Milestone 9.4, `cuervo/script/prelude/animation.plm`, `cuervo/script/anim_bindings.cpp`,
  `tests/cuervo/animation_tests.cpp`, `examples/anim_character`):**
  - *Layout.* Almost everything is Pluma prelude code in one new file; the only native piece is
    four reflection functions on the `Anim` class: `Anim.kind(node, field)`, `Anim.read`,
    `Anim.write(node, field, value: vec4, text)` and `Anim.call(node, method)`. They reach only
    `@export` fields (the same set scene files and the inspector see). `Anim.write` decides by the
    field's own type: float (f32 or f64), int (rounded half up, no libm), bool (`>= 0.5`), `vec2/3/4`,
    `Color` (written in place), plain enum (the discriminant) and `name` (the key's `text`); anything
    else (`str`, lists, structs) is not animatable and the track is skipped with one message.
    Prelude names are prefixed (`Anim*`, `Animation`, `Tween`) so user classes don't clash, and no
    numeric statics were added (they would change every recorded state hash).
  - *Time* is ticks only: key times, `length`, durations and delays are `int`s; the playhead is a
    float so `speed` can be fractional or negative. `Anim.ticks(seconds)` converts. All easing goes
    through the 9.2b `ease_in/out/in_out(Ease, t)` and `ease(x, curve)`, i.e. `dmath`.
  - *Tracks.* A track is `AnimTrack { path, kind, keys }`; a path is `"NodePath:field"` resolved
    against a root (the player's parent, or `root_path`) by node names (`..` and `.` allowed). Kinds:
    `Property` (sampled each tick), `Method` (`Node:method()`, no arguments, fires when a key is
    crossed) and `Event` (emits `AnimationPlayer.event(text)`). A key is `AnimKey { time, value:
    vec4, text, interp, ease_kind, curve }`; `interp` (`Step`, `Linear`, `EaseIn`, `EaseOut`,
    `EaseInOut`, `Curve`) shapes the segment to the *next* key, so easing is per key. Values travel
    packed in a vec4 (float in `x`, a `Color` in `rgba`, ...), which keeps one struct for every type.
    Before the first key and after the last the track holds that key.
  - *Player.* `AnimationPlayer` (a `Node`; it advances in `update_tree` before its siblings, like
    `Timer`) has `animations`, `autoplay`, `speed`, `root_path`, `active`, `play(name, speed)`,
    `play_backwards`, `queue`, `stop`, `seek`, signals `animation_started`, `animation_finished(name)`
    and `event(name)`. `play()` applies tick 0 at once; after `k` ticks the playhead is at `k * speed`.
    Loop modes `None` (stops at the last tick, finished once), `Loop` (wraps, method/event keys on both
    sides of the wrap fire) and `PingPong`. Bindings (resolved node, field kind) are cached per path and
    re-resolved if a node is freed; a path that can't be driven is reported once and skipped.
  - *Tween.* `Node.create_tween()` (one small addition to `scene.plm`) returns a `Tween` whose driving
    coroutine is started by the node, so it is owned by it: it pauses with the node during
    `Scene.paused` and is cancelled when the node leaves the tree (§5.9). Typed builders
    (`tween_float/int/vec2/vec3/vec4/color/bool/name`, `tween_value`, `tween_method`, `tween_callback`,
    `interval`) append to the current step; `parallel()` / `set_parallel` join steps; `with_ease`,
    `with_curve`, `with_delay`, `with_from` adjust the last one; `set_loops`, `kill`, `pause`,
    `resume`, `signal finished`. A step of `N` ticks written on tick `T` reaches its value on tick
    `T + N`; the next step starts on the tick the previous one ends and ticks from the next, so a
    sequence takes the sum of its ticks; callbacks take no time. Typed builders instead of one generic
    `tween_property<T>` because natives can't be generic in this language.
  - *State machine.* `AnimationTree` (a `Node`, evaluates once per tick before its siblings) has
    `states` (`AnimStateDef { name, animation, speed }`), `transitions` (`AnimTransition { from_state
    ("*" = any), to_state, conditions, duration, interp/ease, at_end }`), `start_state`, parameters
    (`set_param`, `set_bool`, `trigger`), `goto(state, duration)`, `travel(state)` (BFS over the
    transitions, conditions skipped, `at_end` honored), `is_fading()` and `fade_progress()`. A
    condition is `param op value` with `IsTrue/IsFalse/Eq/Ne/Lt/Le/Gt/Ge/Trigger`; the first
    transition out of the current state whose conditions hold fires (a transition with no conditions
    fires at once). A trigger is consumed by the transition that uses it and cleared at the end of that
    evaluation, so setting it before or after the tree in the same tick works. Cross-fade: both
    sides keep playing and their poses are blended with weight `shape(t / duration)`; the transition
    tick outputs the pure old pose and the fade ends after `duration` more ticks. A transition that
    fires during a fade freezes the blended pose so far as the outgoing side (no jump). Numbers,
    vectors and colors blend linearly; bools, enums and names switch at weight 0.5; a track one side
    lacks keeps the other side's value, so animations of one rig should key the same tracks.
  - *Scene files.* Animations are a resource **inside the scene**: `AnimationPlayer.animations` is an
    exported `List<Animation>` (nested exportable structs and lists), `AnimationTree.states` and
    `.transitions` likewise. No sibling `.anim` file and no format version bump; the text view,
    migrations and the round trip work unchanged (docs/SCENES.md). The one-line-per-field text view
    makes hand-editing awkward, so the example is generated (`make_scene.py`).
  - *Replay hash.* The script-global hash cannot see node fields, so every `Anim.write` folds the
    written bits into a digest kept by `ScriptHost` that joins `state_hash()` once it is non-zero
    (games with no animation keep their hashes; the shmup hash is unchanged). Animation therefore
    replays like any other state, and a changed key, speed or transition moves the hash.
  - *Plugging in other animation.* Cutout, skeletal and morph animation are not implemented. The track
    system is meant to carry them: a track is a path that samples to a vec4 per tick and a binder that
    writes it. A skeletal or cutout track kind would add an `AnimTrackKind` and a path scheme that
    `AnimBinder.lookup` resolves to a bone or slot (writing the bone's transform fields), and a morph
    track would write a blend-shape weight field; interpolation, blending, the state machine and scene
    saving would be reused as they are.
  - *Cut.* Bezier-handle key curves, per-track wrap, root motion, additive/layered blending and
    blend-space nodes (only two-sided cross-fades), method tracks with arguments and a sibling `.anim`
    file. The editor timeline UI came with 9.8.
- **9.5 Tilemaps and navigation.** `TileMapLayer` with tilesets, autotiling and an editor
  paint tool; `NavigationRegion2D` and `NavigationAgent2D` (grid/mesh A* with
  deterministic tie-breaking).
  **Clarified (Milestone 9.5, `cuervo/script/prelude/tilemap.plm`, `editor/scene_view_panel.cpp`,
  `editor/inspector_panel.cpp`, `tests/cuervo/tilemap_tests.cpp`, `tests/cuervo/navigation_tests.cpp`,
  `examples/topdown_nav`):**
  - *Layout and Prelude.* The subsystem is implemented cleanly as Pluma prelude code in
    `cuervo/script/prelude/tilemap.plm`. It introduces `TileDef`, `TileSet`, `TileCell`,
    `TileMapLayer`, `NavigationRegion2D` and `NavigationAgent2D`. Public symbols are fully documented
    in `docs/API.md`. Internal structures (`_NavNode`, `_nav_node_less`) are private so they do not
    pollute user namespace or alter the global script reflection table.
  - *TileMapLayer.* A 2D uniform grid node extending `Node2D`:
    - *Grid coordinates and packing:* 2D integer cell coordinates `(x, y)` map to and from local/global
      pixel positions (`map_to_local`, `local_to_map`, `map_to_global`, `global_to_map`). Cells are stored
      with fast packed 32-bit keys (`(y << 16) | (x & 0xFFFF)`) supporting negative and positive coordinates
      via `tile_pack_coord`, `tile_unpack_x`, `tile_unpack_y`.
    - *Autotiling:* 8-neighbor bitmask calculation (North: 1, East: 2, South: 4, West: 8, NorthEast: 16,
      SouthEast: 32, SouthWest: 64, NorthWest: 128) matching against tile definition `peering_bits` with
      subset fallback. Placing or erasing a cell automatically updates and propagates peering bitmask
      recalculations across all 8 surrounding neighbors when `enabled_autotile` is active.
    - *Scene serialization:* `TileMapLayer` defines `_serialize_cells` and `_deserialize_cells` with
      `_before_save` and `_after_load` hooks. Cells are saved in strict deterministic coordinate-sorted order
      to guarantee byte-identical `.scene` saves and text dumps across runs and platforms.
    - *Canvas drawing:* In `draw()`, iterates all cells in coordinate order, drawing vector assets through
      `g.vector` or colored procedural tiles through `g.rect`.
  - *NavigationRegion2D & Pathfinding.* A 2D traversable grid region extending `Node2D`:
    - *Traversability grid:* `set_cell_walkable`, `is_cell_walkable`, `set_cell_cost`, `get_cell_cost`.
      Provides `bake_from_tilemap(layer, inherit_costs)` to automatically populate walkable cells and
      custom navigation cost multipliers from a `TileMapLayer` and its `TileSet`.
    - *Deterministic A\*:* `find_path(from, to)` uses a min-priority queue (`PriorityQueue<_NavNode>`)
      with strict total-ordering tie-breaking: when f-scores match, it compares g-scores (preferring closer
      to target), and when g-scores match, it breaks ties on a unique monotonic integer key
      (`id = y * 65536 + x`). Diagonal movement (`allow_diagonal`, `diagonal_cost`) includes corner-cutting
      prevention (both adjacent cardinal neighbors must be walkable). Returns world-space waypoints.
  - *NavigationAgent2D.* An agent node extending `Node2D` for waypoint tracking:
    - Automatically requests and tracks paths from a target `NavigationRegion2D`.
    - Exposes waypoint progression (`get_next_path_position`), distance queries, and signals:
      `waypoint_reached(vec2)`, `navigation_finished()`, `target_reached()`.
  - *Editor Paint Tool.*
    - Added `Tool::Paint` (shortcut `B`) to the editor toolbar with a custom brush icon.
    - `SceneViewPanel`: Left-click paints active tile, Right-click erases, with continuous drag painting,
      grid cell cursor hover highlight, and undo/redo command merging sealed on mouse release.
    - `InspectorPanel`: Displays a tile palette selector when a `TileMapLayer` node is selected.
  - *Replay Determinism and Verification.*
    - `examples/topdown_nav/` implements a top-down level (room perimeter walls, doorway partitions,
      and obstacle pillars) with an animated character pathing along waypoints.
    - Validated in CTest (`topdown_nav`): tick-240 state hash `882ea24679ca2d41` is identical across
      `--jobs 1` and `--jobs 4`, recorded replays verify across worker counts, and scene files round-trip
      cleanly through text views. Zero compiler warnings.
- **9.6 Physics.** Box2D v3 behind the physics interface (§7.2) as an optional feature
  (§4.11 feature build options). Scene-tree bodies: `StaticBody2D`, `CharacterBody2D`
  (`move_and_slide`), `RigidBody2D`, `Area2D`, `CollisionShape2D`, raycasts and joints.
  Determinism is verified by state hash across worker counts. The particle collision
  world (§4.7) stays separate.
  **Clarified (Milestone 9.6, `cuervo/physics/`, `cuervo/script/physics_bindings.*`, `cuervo/script/prelude/physics/*.plm`,
  `editor/icons.cpp`, `tests/cuervo/physics_tests.cpp`, `tests/platformer.cmake`, `examples/platformer`):**
  - *Optional Build Feature & Dependencies:* Box2D v3.2.0 is fetched via CMake (`CUERVO_FEATURE_PHYSICS`, default ON).
    Third-party notice and MIT license texts are added (`third_party/licenses/box2d.txt`, `THIRD_PARTY_NOTICES.md`).
    Feature detection in `cuervo/script/features.cpp` identifies physics requirements. Stripped players compile physics
    completely out with zero overhead when unneeded.
  - *Native Physics Core (`PhysicsWorld` in `cuervo/physics/`):*
    - Wraps `b2WorldId` with 32 px/meter length units scale (`b2SetLengthUnitsPerMeter(32.0f)`).
    - Rigid body lifecycle management (`create_body`, `destroy_body`, `set_body_transform`, `get_body_position`,
      `apply_force`, `apply_impulse`, `apply_torque`, `set_body_mass`, `set_body_gravity_scale`, `set_motion_locks`,
      `set_body_damp`).
    - Shape definitions (`ShapeKind::Circle`, `Box`, `Capsule`, `Segment`) with configurable density, friction, restitution,
      collision layers, and masks.
    - Joints: `PinJoint2D` backed by Box2D revolute joints with angular spring, limits, and motor support; `DistanceJoint2D`
      backed by Box2D distance joints with stiffness and damping.
    - Raycasts: `cast_ray` via `b2World_CastRay` returning hit status, intersection point, surface normal, and fraction.
    - Kinematic sliding solver: `move_and_slide()` performs multi-step shape casting (`b2World_CastShape`), surface normal
      deflection, contact angle classification (`is_on_floor`, `is_on_wall`, `is_on_ceiling`), slop offsets, and dynamic
      body pushing proportional to contact momentum and mass ratios.
    - Sensor overlap detection: `b2World_GetSensorEvents` dispatches touch begin/end events to `Area2D` nodes, emitting
      `body_entered`, `body_exited`, `area_entered`, and `area_exited` signals.
    - Determinism: Canonical state hashing (`state_hash()`) hashes sorted body positions, rotations, and linear velocities.
      Verified bit-for-bit identical across 1 and 4 worker threads.
  - *Pluma Script Prelude (`cuervo/script/prelude/physics/*.plm`):*
    - Exposes `Shape2D`, `CircleShape2D`, `RectangleShape2D`, `CapsuleShape2D`, `SegmentShape2D`, `CollisionShape2D`,
      `PhysicsBody2D`, `StaticBody2D`, `RigidBody2D`, `CharacterBody2D`, `Area2D`, `RayCast2D`, `PinJoint2D`,
      `DistanceJoint2D`, `RayCastResult2D`, and global static query singleton `Physics2D`.
    - Every public symbol is fully documented in `docs/API.md` (1480/1480 symbols). Internal bridge methods live on private
      native class `_Physics`.
  - *Editor Integration:* Added custom vector node icons in `editor/icons.cpp` for `CollisionShape2D` (`shape`),
    `PhysicsBody2D` & `RigidBody2D` (`rigid_body`), `StaticBody2D` (`static_body`), `CharacterBody2D` (`character_body`),
    `Area2D` (`area`), `RayCast2D` (`raycast`), and joints (`joint`).
  - *Replay Determinism & Verification:*
    - `examples/platformer/` implements four playable scenes showcasing all physics features: `level.scene` (classic platformer, character, crates, coins), `joints.scene` (all 9 Box2D joints including bridge, pendulum, spring pad, elevator, welded cross, pogo and motor), `vehicles.scene` (WheelJoint2D buggy with suspension and motor drive, towed trailer, chain terrain), and `sandbox.scene` (AnimatableBody2D moving platforms, polygon boulders, ShapeCast2D sweep scanner, Physics2D.explode, and CCD).
    - Validated in CTest (`platformer`): tick-240 state hash `e458fa6c80754cc5` is identical across `--jobs 1` and `--jobs 4`,
      recorded replays verify on 1 and 4 threads, and scene files round-trip cleanly through text views. Zero compiler warnings.
- **9.7 2D lighting.** SDF lighting and raymarched shadows, `PointLight2D`, occluders;
  both backends in parity (§4.4).
  **Clarified (Milestone 9.7, `cuervo/render/light_batcher.*`, `cuervo/render/shaders/light.{vert,frag}`,
  `cuervo/script/light_bindings.cpp`, `cuervo/script/prelude/lighting.plm`, `tools/rhi_parity`,
  `tests/cuervo/light_tests.cpp`, `tests/cuervo/lighting_script_tests.cpp`):**
  - *Nodes.* `PointLight2D` (radius, color, energy, falloff, an optional cone: `spot_angle` and `spot_feather`
    along the node's rotation, `source_radius`, shadows with `shadow_softness` and `shadow_color`, `light_mask`,
    `subtract`), `DirectionalLight2D` (parallel light along its rotation, shadows traced `max_distance` back),
    `AmbientLight2D` (darkens the world by `darkness`, tinted, except where lights reach; `glow` adds the lights'
    color on top) and `LightOccluder2D` (circle, rounded rectangle, capsule, or polygon walls, with
    `occluder_mask`). They queue into the frame through `Canvas.point_light`, `directional_light`,
    `ambient_light` and `occluder_*` while the tree draws; `Scene.draw_frame` calls `Canvas.apply_lights()`
    after the world and before the CanvasLayers, so HUDs and menus are never lit. Lights are drawn in the
    current Camera2D's view.
  - *Render only.* Nothing reads lighting back into the simulation: the state hash, replays and the 1/N
    thread checks are the same with lighting on, off, or with no light target (the editor's unlit view, a
    test host). `lighting_script_tests` checks the hash tick by tick against an unlit twin. A capture
    (`--screenshot`) lights only its captured frame, which keeps software WebGL (the web export's verify
    run) fast; the frames before it are never shown.
  - *Shading.* Occluders are analytic signed distance fields (circle, rounded box, capsule; polygon edges
    are capsules) in a std140 uniform block, so no render targets or new RHI features are needed and the
    shader is the same GLSL 450 source on Vulkan, GL 3.3, GLES 3.0 and WebGL2. Each light is sphere-traced
    toward per fragment. Soft shadows model a round source `shadow_softness` units across: where the ray
    passes an occluder at distance t of total length L, the penumbra is softness * t / L wide (Quilez's
    closest-approach estimate, counted only while the ray approaches a surface), so shadows are sharp
    where they touch their occluder and widen toward the light. An earlier estimate (k * h / t) widened
    them with the distance toward the light, so a lantern held over the floor darkened everything whose
    ray skimmed the floor near it and left a lit wedge above it; `light_tests` keeps a low light over a
    floor lit. The penumbra is centered on the hard shadow's edge, half that width to either side: the
    march steps on into an occluder until the whole source is hidden (Quilez's inner penumbra). An
    outer-only penumbra grew the shadow of an occluder right next to the light (a crate the lantern
    touches) far past its outline and swung it with every small move of the light, and a light dipped
    into a wall flipped between lit wedges and full shadow; with the centered penumbra it dims smoothly.
    A point on a surface (within the march's 0.25 contact distance) starts as a face point: marching
    from it hit its own surface at once, a hairline of shadow along every floor a light grazed.
    Steps are capped at 1/48 of the distance so a penumbra is sampled smoothly. Without an
    AmbientLight2D each light is an additive quad over its radius; with one, a single full-view quad
    composites every light over the darkness.
  - *Normal mapping.* An occluder's area is a face toward the viewer that bends outward over its last
    `bevel` units (default 8; 0 = flat). The normal comes from the SDF gradient, tilted by the depth
    from the edge; where two occluders touch (a tilemap's rectangles) a walk outward along it finds the
    seam is not an edge, so it stays flat. When that walk had to cross a seam, walks all around (22.5
    degrees apart) each drop their first sample outside onto the union's edge, where its distance is
    exact, and the nearest edge point wins; at an inside corner (rock around a corner of open air) the
    nearest two faces meet at the corner itself. So a wall of stacked strips keeps an even bevel past
    every seam and inside corners round off; the bevel went flat in a wedge at each (bright notches
    along walls, cut corners where a pillar meets the floor). A point on a surface (within the
    march's 0.25 contact) is shaded as the face, as the shadow march takes it. Lights hang `height` units above the plane (PointLight2D,
    default 48; directional lights shine at 45 degrees), and a face gets N.L relative to a flat surface:
    every non-occluder pixel and a face's flat interior are lit as before, a bevel turned to the light
    catches more (up to 2.5x), one turned away goes dark. Only other occluders shadow a face (the march
    leaves the occluder it starts in). `eval_surface_factor` mirrors it on the CPU.
  - *Limits.* Per view, 32 lights, 128 occluders and 512 occluder references. Each light only marches the
    occluders that can reach it (its radius plus the penumbra margin; directional lights take those in
    view), culled on the CPU in `LightBatcher::flush`; past the limits the rest are dropped and logged once.
    `cuervo/render/light_batcher.cpp` mirrors the shader's math with `dmath` (`eval_light_at`,
    `eval_raymarch_shadow`) for tests and tools.
  - *Parity.* `rhi_parity` renders two lighting scenes (additive shadows, ambient composite) and the
    `rhi_parity` test also captures `examples/lantern_cave` at tick 240: GL 3.3 and GLES 3.0 are identical
    (tolerance 0) and Vulkan matches GL within one level per channel. On the reference machine the lit
    cave runs at about 700 FPS at 1280x720 on both backends, and about 60 FPS in Chrome on WebGL2.
  - *Cut.* Normal-map textures for sprites (normals come from occluder shapes only; textures arrive with
    11.2a), light cookies, colored (translucent) occluders, per-sprite light masks and shadow casting
    from arbitrary vector shapes; occluders are declared, not derived from what is drawn.
- **9.8 Integration.** Editor support for every new node, tool and panel (§4.13), and a
  sample platformer using physics, tilemaps, animation and lighting.
  **Clarified (Milestone 9.8, `editor/animation_panel.cpp`, `editor/anim_edit.*`, `editor/icons.cpp`,
  `cuervo/script/prelude/tilemap.plm`, `cuervo/script/prelude/physics/tilemap_collision.plm`,
  `examples/lantern_cave`, `tests/lantern_cave.cmake`, `tests/editor/editor_98_tests.cpp`):**
  - *Animation timeline* (the editor UI §4.17 9.4 left out). An "Animation" panel in the bottom dock edits
    the clips of the selected AnimationPlayer (or the one under the selected node): new/delete clip,
    length, loop mode; one row per track with its keys on a tick ruler; click to scrub, drag a key to move
    it, right-click to delete; "Key" records the track's field as the document stores it at the playhead;
    the selected key's tick, value, interpolation and easing are editable. Every change rewrites the
    player's `animations` field through the document (one undo step, a drag merges into one) and saves
    with the scene. "Preview" poses the scene view at the playhead (`ProjectEnv::preview_animation`:
    `play` then `seek` on the preview node, which never runs ready()).
  - *Lighting in the editor.* The scene view and play-in-editor draw the scene's lights (View > Lighting
    Preview turns it off for editing in the dark); the light nodes draw their reach, direction or outline
    as gizmos in the editor only. New icons: light, sun, moon, occluder and animation (AnimationPlayer,
    AnimationTree); TileMapCollision2D uses the static body icon. A panel that a saved layout predates
    docks into its default dock as a tab instead of floating.
  - *Tilemaps for platformers.* `TileMapLayer.solid_rects()` merges the solid tiles (`TileDef.walkable`
    false) into rectangles: runs along each row joined with identical runs below, in row order (the same
    list every run), cached until the cells change. `light_occluders` makes them light occluders, and the
    physics prelude's `TileMapCollision2D` (a child of the layer) builds one transient StaticBody2D box per
    rectangle when it enters the tree (`rebuild()` after editing cells).
  - *Sample game.* `examples/lantern_cave`: a dark cave of tiles (collision and occluders), an explorer
    (CharacterBody2D) with a Camera2D, a lantern light on a swinging arm and an AnimationPlayer walk cycle,
    torches whose energy flickers through AnimationPlayer tracks, a sweeping spot-light beacon, crystal
    and gem lights, pushable crates (RigidBody2D with a LightOccluder2D), four gems and a door. The
    `lantern_cave` test plays it on 1 and 4 threads to tick 240 with pinned state hash `2dd8beccd71bc67e`,
    verifies the recorded replay on 1 and 4 threads, plays the whole level (every gem, out of the door at
    tick 587) and round-trips the scene; `examples_sweep_lantern_cave` exports it (bytecode, AOT) and hot
    reloads it; `cuervo-tools export --platform web` verifies its stripped web player in headless Chrome.
  - *One hash for every build.* Exported games used to reach another state hash on a stripped
    player (native or web) than on the full one, because a missing feature changed what the hash
    covered. A player without particles or the UI now hashes what those hold when unused (§4.11
    "Clarified (Milestone 7.5)"), and `cuervo-tools export` checks every stripped player against
    the full engine: the platformer reaches `8f557693da68a47e` and the cave `2bb36eb7e7e86508` at
    tick 240 on the full player, a stripped Linux player and the stripped web player in Chrome.
    Full-engine hashes (and recorded replays) are unchanged.
  - *Hot reload with physics.* The physics world holds its bodies', shapes' and joints' nodes; a hot
    reload migrates every script object to a new VM, and the world kept calling the old (freed) ones,
    which hung or crashed a physics game on its next tick. `PhysicsWorld::remap_owners` now moves them
    through the reload's Migrator (`physics_script_tests` reloads a running scene and keeps stepping).

### 4.18 Windows, language APIs and plugin ecosystem (10.5a-10.5d)
Runs after Phase 10 (numbered 10.5 so Phases 11 and 12 can follow). It makes the
engine usable by other people on the most common desktop platform and lets them extend
it. Each piece keeps replay hashes identical.
- **10.5a Windows builds.** Turns docs/WINDOWS.md from "reviewed" into "verified": continue
  active validation on the Windows laptop (Intel Core Ultra 9 185H + RTX 4070), build with MSVC
  and MinGW-w64, fix the warnings the zero-warning rule exposes, add Windows
  CI that runs `ctest`, confirm no FMA contraction under MSVC, and check the shmup state
  hash matches Linux and the web. Also covers the console UTF-8 output, high-DPI pointer
  mapping, and a GUI-subsystem release build. Windows export (§4.11) stops needing a
  hand-built player. Signed prebuilt editor and export templates are published so users
  don't compile from source.
  - *Clarified (Milestone 10.5a).* MSVC (`cl` 19.44), MinGW-w64 GCC 16 (UCRT) and Clang
    with the MSVC runtime build with zero warnings and pass `ctest`. The `msvc-*` and
    `mingw-*` presets name the compiler, and CI runs MSVC Release and MinGW-w64 Debug
    (MSVC Debug passes too, but its Debug STL makes the suite about three times slower). The shmup reaches `a3e57e79964421d3` on
    every toolchain, on GL and Vulkan. `platform_tests` checks that `a * b + c` is not
    contracted to FMA (`/fp:precise` doesn't contract since VS 2022). **Decision:** every
    Windows program embeds one manifest and sets a UTF-8 console code page for its lifetime
    (cmake/Windows.cmake). The manifest gives the UTF-8 active code page (argv and narrow
    file names), PerMonitorV2 DPI awareness and long paths. SDL3 then reports window
    coordinates in pixels (density 1 at 150 %), and pointer positions match the pixel under
    the cursor. **Decision:** `cuervo-player.exe` is a GUI-subsystem build of the same
    runtime sources, entered through a plain `main`. Exports ship it, `cuervo.exe` stays a
    console program, and the editor is GUI-subsystem too. **Decision:** prebuilt packages
    carry their export templates in `templates/<platform>/`, which `cuervo-tools export`
    finds after the build tree's own player (docs/EXPORT.md "Players"). Release files are
    listed in SHA256SUMS.txt, with Sigstore attestations once the repository is public. Authenticode signing uses a certificate the repository holds as a
    secret, so unsigned releases are possible and documented. Stripped players build on
    Windows too (a `CreateProcessW` process runner), so tictak's stripped export reaches the
    full player's hash there. Fixes the toolchains exposed:
    - MSVC's 16 KB limit on a string literal (embedded texts are split).
    - Windows' 1-2 MB main stack (now 8 MB everywhere).
    - MSVC's static-library naming for plugins, plus the Windows system libraries that a
      statically linked Rust plugin needs in AOT modules.
    - GNU-ld link flags for MinGW, in project flags and in AOT module builds.
    - MinGW's duplicate default manifest, its runtime DLLs (now linked statically) and its
      path-hashed DLL image base, which made AOT modules differ between exports.
    - The Visual C++ runtime DLLs an MSVC player imports: Windows exports ship them
      app-local.
    - `@extern("", ...)`, which now searches every loaded module as `dlopen(NULL)` does.
    - GCC's unaligned 32-byte stack spills on Win64 (bug 54412), which crashed Box2D's
      AVX2 solver; the assembler emits unaligned moves, with identical results.
    - Intel Arc's GL and Vulkan drivers light a few tile-seam pixels differently, so the
      lit GL-vs-Vulkan parity check allows 0.01 % (docs/WINDOWS.md).
- **10.5b Modules, packages and unit tests.** Today a project's files share one global namespace
  (LANGUAGE.md 9.12). Adds `import`, per-file or per-package namespaces and `pub`
  visibility across them, a `[dependencies]` table in project.toml with pinned versions,
  and packaging so libraries can be shared. Hot reload (§5.13) and the LSP follow imports.
  Also adds native unit testing to Pluma: `@test fn test_name() { ... }` declarations (omitted from
  release builds) executed via `cuervo-tools test` and `plm test` with deterministic assertions,
  giving scripts pure-logic regression tests in CI alongside full-game replays.
  - *Clarified (Milestone 10.5b).* A game's own scripts stay the root namespace, so
    existing games and replay hashes are unchanged. A package (`package.toml` plus
    `scripts/`) is a namespace of its own: its names are keyed `pkg.Name`, only `pub`
    ones are visible outside it, and files import it one by one (`import tween`,
    `import tween as tw`, `import tween.{Tween}`; LANGUAGE.md 9.26). A package sees the
    preludes' names but never the game's. **Decision:** a dependency names one exact
    version (`[dependencies]`, vendored under `packages/<name>/` or found by `path`), and
    a project holds one version of each package; version ranges and a registry wait for
    10.5d (docs/PACKAGES.md). A project is still checked as one unit: each source part
    carries its package, so the session, hot reload, AOT and the language server need no
    separate module loader. Exports pack every package under `packages/<name>/`.
    `examples/garden` (the `tween` package from `examples/libs/tween`, a vendored
    `palette`, and the game's own declarations with the same names) reaches its dev
    state hash from source and AOT packs on Linux and in headless Chrome.
    `cuervo-tools package new|check` makes and checks a package on its own. `@test`
    functions (LANGUAGE.md 9.27) are left out of normal and release builds; `plm test`
    and `cuervo-tools test` run a game's or a package's own tests, each in a fresh
    session, and ctest runs those of `examples/garden` and `examples/libs/tween`.
  - *Cycle collector fix (found while closing 10.5b).* Under ASan the `anim_character`
    sweep showed a use-after-free that predates 10.5b. The scene bridge built every
    saved list as a traced list, even where the list it replaced was untraced (an
    acyclic element type) and held by an untraced object, and the collector then
    freed such a list while still reading it. The bridge now keeps the replaced
    list's tracing, and the collector reads all untraced children before it releases
    any of them.
- **10.5c Plugin, embedding and FFI APIs.** A stable, versioned C ABI for native modules
  that register nodes, `@native` functions and importers (the engine's own preludes are
  the model, §5.12), a Pluma FFI to call C libraries with explicit marshaling, and an
  embedding API for hosting the Pluma VM in other programs. Native code in the sim path
  must be deterministic, so the API flags modules as sim-safe or not, and AOT export
  links plugins statically. The C ABI serves as the long-term extension boundary for
  proprietary store, audio, and console SDKs without requiring engine forks or closed-source
  modifications. **Decision:** the first bindings are C++, Rust and C#, all
  over the one C ABI. C++ is a header-only wrapper (the engine's own language). Rust is a
  safe crate generated from the ABI's header. C# runs on .NET hosted in the player and
  editor on desktop; it is not available on the web or in AOT exports until a
  NativeAOT/WASM path is proven, so the web build is C++/Rust/Pluma only at first.
  - *Clarified (Milestone 10.5c).* A plugin is a package (10.5b) with a `[plugin]` table
    in `package.toml`: the ABI version, the sim-safe flag and the library for each
    platform, shared and static. Its scripts declare the API in Pluma, and the library
    implements the `@native` parts. That way types, defaults and inheritance come from the
    checker, as they do for the engine's preludes. A plugin registers names only inside
    its package's namespace. The ABI (`cuervo/plugin/cuervo_plugin.h`) is one versioned
    host function table: new entries are appended, and `size` says which entries exist.
    **Decision:** a plugin whose `abi` is not the engine's ABI version is refused at load.
    The Rust and C# raw layers are generated from the header (`tools/gen_plugin_abi.py`),
    and a ctest checks they match. AOT export links each plugin's static library into the
    script module, which lists the plugins it contains. A plugin with only a shared
    library ships it in the pack, and a .NET plugin makes an AOT export fail. Importers
    produce the engine format under the output path. In development the VFS converts on
    read, and export stores the converted file, so a release game never runs an
    importer. The engine warns about plugins that are not sim-safe, since natives run
    in the simulation. **Decision:** the FFI (`@extern("lib", "symbol")`, LANGUAGE.md
    9.28) marshals numbers, `bool` and `str` only, with at most 4 parameters, and binds on
    the first call, so a missing library panics there. Anything richer is a plugin.
    The embedding API (`pluma/embed/pluma_embed.h`) is C over the language library and
    has no engine dependency. Its values convert by the declared Pluma types, and panics
    never unwind through C frames. `examples/plugin_demo` uses a node and functions from
    `orbit` (C++), `noise` (Rust) and `hues` (C#). The C++ and Rust plugins linked into
    AOT modules give the same results as in the VM (docs/PLUGINS.md).
- **10.5d Plugin ecosystem, licenses and contribution.** Builds on 10.5b and 10.5c so
  other people can share and contribute code:
  - *Plugins:* a plugin manifest (name, version, engine and ABI version range, platforms,
    sim-safe flag, license) and a registry index that `cuervo-tools plugin
    install|publish|verify` use. Verification checks the ABI version, the license
    declaration and, for sim-safe plugins, the replay hash on a conformance project.
    Plugins are built from source or shipped as per-platform binaries, with checksums.
  - *Licenses:* **Decision:** the engine is licensed under the permissive MIT License in
    [`LICENSE.md`](../LICENSE.md). Third-party components keep their own licenses,
    with full texts in `third_party/licenses/` and summarized in `THIRD_PARTY_NOTICES.md`
    (checked by the `third_party_notices` ctest and bundled into every export).
  - *Contribution:* `CONTRIBUTING.md` (build, test, the zero-warning and determinism
    rules, code style, how to add a prelude node or native), a code of conduct, a
    security policy for reports, issue and pull request templates, a contributor license
    or sign-off policy, a design-change process (proposals recorded in DESIGN.md as
    "Decision/Clarified" notes), and a public roadmap generated from the milestones table.
  - *Clarified (Milestone 10.5d).* The manifest is `package.toml`, for every package and
    not only plugins. `engine` takes a version range (`>=0.1, <0.3`, `^1.2`, `*`), and a
    game whose engine is outside it doesn't build. `[plugin] abi` takes an ABI version or
    a range of them, and `platforms` defaults to the platforms the library fields name.
    **Decision:** a project still pins one exact version per package (10.5b). Ranges
    choose what `cuervo-tools plugin install` fetches, and the version it installs is
    pinned in project.toml. **Decision:** a registry is a directory (`index.json` plus one
    archive per version), so a local folder, a shared drive or static file hosting can
    serve it with no server code. HTTP fetching waits until a hosted registry exists. An
    archive is a pack (platform "any") with the manifest, scripts, license and notices,
    the conformance project and the built libraries, and no sources. The index records the
    SHA-256 of each archive and of every file in it, and the platforms that have a built
    library. `install` refuses any mismatch, and a version can't be republished with
    other contents. Verification checks:
    - the license, as an SPDX expression, and a license text in the package;
    - the engine and ABI ranges;
    - that the library loads, and that the scripts check against its natives.
    A sim-safe plugin also needs a conformance project (`conformance/`, a game that depends
    on the plugin by `path = ".."` and keeps its results in globals). It is packed and run
    headless for 240 ticks; publish records the state hash, and install requires the same
    hash on the installing machine. A vendored package resolves its dependencies among its
    game's `packages/`, and archived manifests drop `path`. Exports write
    `licenses/PACKAGES.md` (also `cuervo-tools license report`) and copy each package's
    license text next to the engine's notices. Contribution:
    - CONTRIBUTING.md covers the zero-warning, style and prelude/native rules, a
      Developer Certificate of Origin sign-off (no CLA) and the design-change process;
    - `CODE_OF_CONDUCT.md`, `SECURITY.md` (private advisories; scripts and plugins are
      code, not a sandbox) and the issue and pull request templates are in the repository;
    - `docs/ROADMAP.md` is generated from §6 by `tools/gen_roadmap.py`, and a ctest keeps
      it current.
    `examples/plugins/orbit` publishes to a registry, installs into a fresh project and
    passes the checks there, and so does a script package with a dependency
    (`tests/cuervo/registry_tests.cpp`).

### 4.19 Production features (11.1-11.4)
Phase 11 holds what real shipped games need beyond the engine core, found by comparing
with Godot and Bevy. Each piece keeps replay hashes identical and works on desktop and
the web unless noted. Platform services (Steam, achievements, cloud saves) and proprietary
middleware (Spine, FMOD) sit behind the 10.5c C ABI plugin architecture.
- **11.1 Networking.** A transport layer (UDP on desktop, WebSocket and WebRTC on the
  web), then two models on the deterministic sim (§4.1): lockstep (inputs only) and
  rollback (snapshots, re-simulation, input prediction), both reusing the replay frame
  format (§4.8, docs/REPLAYS.md) and the state hash to detect desync. Plus HTTP and
  WebSocket clients for services, outside the sim path. Scripts see networking as
  recorded input and events, never as raw sockets, so the sim stays deterministic.
- **11.2a Raster processes.** While vector graphics remain Cuervo's primary art pipeline,
  shipped games frequently incorporate raster assets, pixel art, sprite sheets, and
  pre-rendered textures. This milestone establishes the complete offline asset processing
  and runtime raster pipeline beside the vector path:
  - *Asset ingestion and processing:* import decoders for standard image formats
    (PNG, JPEG, WebP, QOI via `stb_image`) converting into engine texture formats in
    `.import/`. Import-time generation of mipmaps, color space handling (sRGB to linear),
    and texture format transcoding.
  - *Texture atlases and sprite sheets:* automatic offline bin-packing (MaxRects algorithm)
    of loose sprites into deterministic power-of-two texture atlases with configurable
    padding and 1-pixel border extrusion to prevent filtering bleed. Slicing tools for
    sprite sheets (fixed grid and JSON metadata) into named/indexed animation frames.
  - *SVG rasterization fallback:* automated raster fallback (§4.5) for SVG documents that
    use unsupported vector features (filters like `feGaussianBlur`, complex masks, drop shadows).
    Rasterized to atlases at target game resolution/DPI and cached, avoiding runtime tessellation bottlenecks.
  - *Runtime batching and nodes:* a dedicated textured quad batcher sharing RHI instancing
    conventions with the SDF and vector batchers. Node support: `Sprite2D` (raster image and
    atlas frame node with flipping, tinting, source rects, and material assignment),
    `AnimatedSprite2D` (frame-based sprite animation playback with loop modes and speed), and
    `NinePatchRect` (9-slice procedural scaling for UI panels and scalable world sprites without corner distortion).
  - *Filtering and scale modes:* per-texture and global sampler filtering controls (`nearest`
    for pixel art and `linear`/bilinear for smooth scaling), integrated with the project
    scale modes (`fit`, `integer`, `stretch`, §4.4).
  - *RHI and tier parity:* identical textured quad rendering on Vulkan, OpenGL 3.3,
    OpenGL ES 3.0, and WebGL2 under the `compatible` render tier. Replay hashes remain
    strictly deterministic as sim objects manipulate node transforms and frame indices, while
    GPU rasterization remains purely visual.
- **11.2b Custom shaders, post-processing and linear color.** User-written shaders, screen-space
  raster post-processing passes, and configurable color spaces:
  - *User shaders:* authoring custom vertex and fragment shaders in GLSL 450 through the
    existing shader compilation and validation pipeline (§4.4), translated to target backends.
    Shader parameters are exposed as inspector-editable `@export` uniforms (scalars, vectors,
    colors, `sampler2D` textures). Tier-checked by `cuervo-tools tier check`.
  - *Post-processing pipeline:* off-screen render targets (framebuffer attachments / render passes)
    supporting multi-pass framebuffer ping-ponging. Built-in post-process passes (bloom
    downsample/upsample blur pyramid, color grading LUT, vignette, CRT scanlines) and custom
    full-screen quad blits.
  - *Color space toggle:* `project.toml` supports `[render] color_space = "srgb"` (default;
    1:1 match with Inkscape and web vector tools on UNORM framebuffers) or `"linear"` (hardware
    sRGB framebuffers, sRGB-to-linear texture sampling, and linear lighting/blending,
    eliminating dark fringe artifacts on blended sprites).
- **11.2c Text scenes on save.** Project source directories (`scenes/`) store scenes in the
  human-readable text format (emitted by `scene dump`) as their primary authoring format,
  allowing Git 3-way auto-merging across branches without binary conflicts. During release export
  (`cuervo-tools pack` and `export`), scenes are compiled into the packed binary format (`.pak`),
  retaining instant startup times and zero runtime parsing overhead for release builds.
- **11.3 Localization and text.** Translation tables (keys to strings per locale, with
  plural and format rules) and a locale switch at run time; complex-script shaping and
  bidirectional layout for the MSDF text path (a shaping library pinned in
  `cmake/Dependencies.cmake`); IME input for `LineEdit` (9.3); locale-aware number and
  date formatting that stays out of the sim.
- **11.4 Profiling and save API.** A frame, job and GPU profiler (per-system tick cost,
  job-thread timeline, draw calls) and a memory profiler for the Pluma heap (§5.10),
  shown as an editor panel and writable to a file. A general save/load API (typed save
  slots, versioning with migrations like scene files §4.12, per-user directory) and a
  settings API (graphics, audio, input) replacing the ad hoc `Scene.save` use.

### 4.20 Content tools, AI and accessibility (12.1-12.4)
Phase 12 makes the editor and runtime good for narrative, AI-heavy and accessible games.
- **12.1 Content tools.** A dialogue editor (branching, conditions, localization keys
  from 11.3), a cutscene/timeline editor built on 9.4's `AnimationPlayer`, a particle
  editor panel for the particle system (§4.6, currently script-defined), and a visual
  shader editor producing 11.2b shaders. Graph-based panels use a virtualized node-canvas
  backend (`imgui-node-editor`) for smooth zooming, link navigation, and box selection.
- **12.2 AI.** Behavior trees and a state-machine tool (nodes in the scene tree, edited
  and debugged in the editor), and steering behaviors (seek, flee, arrive, avoid,
  flocking) that use 9.5 navigation. All deterministic and tick-driven.
- **12.3 Accessibility and editor polish.** Screen-reader support through **AccessKit**,
  maintaining an in-memory semantic accessibility tree alongside the viewport that bridges
  to platform accessibility APIs (Windows UI Automation, Linux AT-SPI, web ARIA). UI scaling,
  runtime input remapping (building on the input map editor, §4.13), colorblind palettes,
  reduced-motion and subtitle options. Editor polish: SDL3 `SDL_EVENT_DROP_FILE` support
  for native OS file drag-and-drop into the scene view, dynamic high-DPI font atlas
  re-sampling, and refined brand theme styling.
- **12.4 The ECS resolution.** Settles the architecture: **Hybrid Model**. The `Node2D`
  scene tree remains the user-facing model for composition, hierarchy, transforms, and UI.
  High-throughput simulation systems (particles, bullets, spatial collision) remain in
  contiguous C++ SoA pools, exposed to Pluma scripts through deterministic query iterators
  (`for e in query(Enemy) { ... }`) executed cleanly in the job system. Low-level raw ECS
  plumbing is kept internal.

### 4.21 Extended platforms and drop-in console SDKs (Milestone 13)
Brings the 2D engine to mobile and consoles before 3D expansion:
- **Mobile platforms:** Android via SDL3 and OpenGL ES 3.0 / Vulkan with Gradle/NDK packaging;
  iOS via SDL3 and Metal (MoltenVK). Handles mobile app lifecycle events (backgrounding,
  suspend/resume state restoration), notch safe-area insets, and battery/thermal-aware
  frame pacing that protects fixed-timestep determinism.
- **Desktop parity:** macOS support via SDL3 and MoltenVK on Apple Silicon and Intel.
- **Drop-in console architecture:** The core engine remains 100% open-source MIT with zero
  proprietary NDA code. Console backends (Nintendo Switch, PlayStation, Xbox) are implemented
  against the 10.5c C ABI. Authorized developers compile private closed-source platform
  shims (`cuervo_platform_switch.so` / `.dll`) and drop them directly into export directories
  without needing private forks or closed engine sources.

### 4.22 Zero-bloat 3D expansion (Milestone 14)
Extends the engine into 3D while preserving 2D performance and binary footprint:
- **Unified math:** Generic `Vec<T,N>` already provides `vec3`, `vec4`, matrices, and
  quaternions with zero duplicate math abstractions.
- **3D systems:** 3D scene/camera transforms, 3D mesh batcher, glTF 2.0 import, and PBR
  materials (albedo, roughness, metallic, normal maps).
- **Zero-bloat guarantee:** Milestone 7.5 feature-stripping (`--engine stripped`) compiles
  out all 3D mesh batchers, PBR shaders, and glTF loaders for 2D games at export time.
  2D release binaries incur zero bytes of 3D overhead.

### 4.23 First-party jumpstart ecosystem (Milestone 15)
Seeds the ecosystem with official plugins, wrappers, and reference templates:
- **Official C ABI plugin wrappers:**
  - `cuervo-steamworks`: Achievements, leaderboards, stats, cloud saves, and overlay hooks.
  - `cuervo-spine`: Runtime Spine2D skeletal mesh animation and atlas binding.
  - `cuervo-fmod`: Optional drop-in commercial audio engine for pro sound designers (keeping
    lightweight, zero-cost `miniaudio` as the built-in engine default).
  - `cuervo-console-template`: A turn-key boilerplate repository showing authorized developers
    how to insert proprietary console SDK calls behind the C ABI.
- **Reference showcase demos:**
  - A 2-player rollback netcode arena brawler (demonstrating 11.1 zero-desync netplay).
  - A 100k-particle bullet hell / survivors clone (demonstrating 4.15 / 5.5 throughput).
  - A multi-platform narrative vertical slice (demonstrating 12.1 dialogue, 11.3 localization,
    and 12.3 AccessKit screen-reader integration).

## 5. The language

### 5.1 Overview
Statically typed with inference, brace syntax, embedded in the engine. Compiles to bytecode
for development (fast iteration, hot reload) and to C for native release builds (native speed,
no JIT). AOT export targets the host platform; web exports ship the scripts as source for the VM.

### 5.2 A taste
```
// Global class: usable anywhere by name.
class Boss extends Enemy {
    var hp = 400                  // inferred int
    var phase: Phase = Phase.Opening
    signal defeated

    fn ready() {
        start(run_phases())       // start a coroutine
    }

    fn run_phases() {
        while hp > 0 {
            match phase {
                Phase.Opening => {
                    emitter.ring(count = 36, speed = 180.0, every = 0.25)
                    await wait(6.0)
                    phase = Phase.Spiral(arms = 4)
                }
                Phase.Spiral(arms) => {
                    emitter.spiral(arms = arms, spin = 12.0, every = 0.05)
                    await until(fn() { hp < 150 })
                    phase = Phase.Rage
                }
                Phase.Rage => {
                    for i in 0..8 {
                        emitter.aimed(at = Player.main.position, spread = 5)
                        await wait(0.1)
                    }
                }
            }
        }
        emit defeated
    }

    fn draw(g: Canvas) {
        if phase is Phase.Rage {
            g.circle(position, 48.0, stroke = Color.hex("ff4060"), width = 3.0)
        }
    }
}

enum Phase {
    Opening,
    Spiral(arms: int),
    Rage,
}
```

### 5.3 Lexical
- Comments: `//` and `/* */` (nestable).
- Braces delimit blocks. Newlines end statements; `;` is optional.
- **Style guide:** always indent the contents of a block by 4 spaces, even though the
  braces define the structure. The formatter enforces this.
- Interned names: `#shoot`. String interpolation: `"Score: {score}"`.

### 5.4 Types
| Kind | Types |
|---|---|
| Integers | `int` (i32), `i8`, `i16`, `i64`, `u8`, `u16`, `u32`, `u64` |
| Floats | `float` (f32), `f64` |
| Other scalars | `bool`, `name` (interned) |
| Vectors | `vec1` … `vecN` (`float`); `ivecN` for integers. Unboxed values, swizzles for N ≤ 4 |
| Text | `str` (immutable UTF-8), `StrBuilder` |
| Collections | `List<T>`, `Map<K,V>`, `Set<T>`, fixed arrays `[T; N]` |
| User types | `struct` (value), `class` (reference), `enum` (sum type), `trait` |
| Functions | `fn(int) -> bool`, closures |
| Engine | `Entity` (generational handle), `Color`, `Canvas`, `Pool<T>`, … |

- Inference: locals and most fields are inferred; function signatures are annotated.
- No implicit numeric conversions except int literal → float. `as` for explicit casts.
- **Structs are values** (copied, stored inline, no refcount). **Classes are references.**

### 5.5 Classes, inheritance, traits
- Single inheritance: `class Boss extends Enemy`. Script classes can extend engine node
  types (`extends Sprite`) and each other.
- Traits declare required methods and can provide default implementations:
  ```
  trait Damageable {
      fn hurt(amount: int)
      fn is_dead() -> bool { return false }
  }
  class Enemy extends Node2D: Damageable { ... }
  ```
- Methods are virtual by default; `final` prevents overriding.
- Generics: `class Pool<T>`, `fn max<T: Ord>(a: T, b: T) -> T`. Monomorphized under AOT;
  type-erased with specialized fast paths in the VM.

### 5.6 Enums and match
Sum types with data, matched exhaustively. A non-exhaustive `match` is a compile error.

### 5.7 Null
All class/reference types are nullable. Using `null` (calling a method, reading a field)
**panics** with the variable name, file and line. `?.` (safe call) and `??` (default)
are available. Value types (`int`, `vecN`, structs) can never be null.

### 5.8 Errors
- Expected failures return `Result<T, E>`; `?` propagates the error to the caller.
- Bugs (null use, out of bounds, use-after-free on checked manual memory, failed `assert`)
  **panic**:
  - In development the game pauses and the editor shows the stack trace and locals.
  - In release builds the error is logged and the offending script instance is stopped;
    the game keeps running if it can.

### 5.9 Coroutines and signals
- Any function containing `await` is a coroutine. `start(f())` runs it; it is owned by the
  object that started it and cancelled when that object is destroyed.
- Built-in awaitables: `wait(seconds)`, `wait_ticks(n)`, `until(cond)`, `signal`s,
  animation and audio completion.
- Time is measured in simulation ticks, so coroutines stay deterministic.
- `signal name(args)` declares an event; `emit`, `connect`, `disconnect`.

**Clarified (Phase 3; details in LANGUAGE.md 9.10):**
- A coroutine runs only through `start(f())` (returns a `Task`: `cancel()`, `is_done()`,
  `await task`) or `await f()` inside another coroutine (runs it to completion, value = its
  result). A bare call is a compile error. `yield` waits one tick.
- `start` runs the coroutine immediately up to its first suspension. The VM's scheduler
  resumes waiting coroutines once per tick, in start order. `wait_ticks(n)` resumes on the
  n-th tick after the current one; `wait(s)` is `round(s × tick_rate)` ticks, at least one;
  `until(cond)` checks immediately, then once per tick.
- Owner = the `self` of the method that called `start` (none in free functions). "Destroyed"
  means leaving the scene tree (`queue_free`); a suspended coroutine keeps its owner alive.
- Heap frames: while suspended a coroutine's frames are stored in its Task object; resuming
  copies them back onto the VM stack.
- Signals are fields holding a signal object. `emit` (own signals only) calls the connected
  handlers in connection order right away; coroutines awaiting the signal resume at the
  next tick's coroutine pass, with its argument as the value (one parameter) or nothing.
  `disconnect` matches by function identity.

### 5.10 Memory model
**Automatic (default):**
- Class instances, lists, maps and strings are reference-counted and freed as soon as the
  count reaches zero.
- A **cycle collector** (trial deletion, Bacon-Rajan) handles cycles:
  - Only objects whose count dropped without reaching zero become "suspects".
  - It runs incrementally at a fixed point in the tick, with a budget measured in **work
    units, not time**, so it is deterministic.
  - Types the compiler proves acyclic are never traced.
- `weak var` exists for breaking cycles explicitly.

**Manual (opt-in):**
```
var tmp  = new(frame) List<vec2>()     // frame arena, freed at end of tick
var boss = new(scene) Boss()           // scene arena, freed when scene unloads
var buf  = alloc Buffer(4096)          // manual
defer free(buf)
manual class ParticleSystem { ... }    // every instance is manual
```
- Manual objects are not refcounted or traced.
- References to manual objects are **always checked** (pointer + generation). Using one after
  it is freed panics with where it was freed.
- The compiler rejects obvious escapes (storing a `frame` value into a longer-lived object).
- `unsafe { }` blocks skip the generation checks inside them, for proven hot loops.
- A manual object that references automatic objects holds counts on them like any other
  owner, and releases them when freed or when its arena resets.

**Clarified (Phase 7A, `pluma/heap.hpp`, `pluma/acyclic.hpp`, `cuervo/script/script_host.hpp`):**
- *Cycle collector:* Bacon-Rajan synchronous trial deletion (`MarkGray`, `Scan`,
  `ScanBlack`, `CollectWhite`) over a suspect buffer. A non-zero `release` buffers an
  object once unless it is `Untraced` (provably acyclic type, string, vector, weak cell,
  captureless closure, or manual object). Each step processes whole candidate sets against
  a work-unit credit (`1` unit per object colored or edge followed; `HostOptions::gc_budget`
  default `20000` per tick), repaying any overshoot across subsequent ticks so collection
  never interleaves with mutators and remains deterministic. Garbage is finalized
  depth-first in buffer order (weak references nulled and native finalizers run before
  freeing memory).
- *Acyclicity (`pluma/acyclic.hpp`):* the compiler analyzes the type reachability graph
  (classes, structs, payload enums, `List<T>`, and an `ANY` sink for closures/tasks/signals/traits/generics;
  `weak` fields and manual classes add no edges). `@acyclic` is trusted in release builds
  and checked in development (unproven `@acyclic` classes are traced with `objflag::Promised`
  and panic if found in a garbage cycle).
- *Weak references:* `weak var` is allowed on class instance fields of class or `List<T>`
  type; reading a weak field yields `null` as soon as the target is freed (by refcount,
  cycle collection, `free`, or arena reset).
- *Manual memory & arenas:* `alloc T(...)`, `new(frame) T(...)`, `new(scene) T(...)` (for
  classes and `List<T>`), `manual class` (subclasses must also be `manual`), and `free(x)`
  (`free(null)` is a no-op; freeing an automatic object or double-freeing panics).
  References carry `pointer + 32-bit generation`; freed slots reuse the same kind with the
  incremented generation (and retire on generation exhaustion) while recording a `FreeSite`
  (`file:line` of `free`, or the arena and tick of a reset) reported on use-after-free.
  Freeing a manual object cancels its coroutines, nulls weak references to it, runs its
  native finalizer, and releases its strong references to automatic objects. `ScriptHost`
  resets the `frame` arena and steps the cycle collector at the end of step 5 of every
  tick, and resets the `scene` arena when replacing the scene root.

### 5.11 Names and visibility
All classes, enums, traits and structs are **global by name** (GDScript style); there are
no imports. The compiler reports duplicate names across the project as errors.
Members starting with `_` are private to the class.

### 5.12 Execution
- **Development:** source → lexer → parser → type checker → typed IR → bytecode → register VM
  (7.1). The typed IR (`pluma/tir.hpp`) is also the input of the AOT-to-C path.
- **Release:** the same typed IR is emitted as C, compiled with the engine and linked in.
  Panics, refcounting and checks behave identically.
- Both paths share one test suite to keep their semantics identical.

**Clarified (Phase 7B, `pluma/aot.hpp`, `cuervo/script/script_host.hpp`, `tools/cuervo_tools/export.hpp`):**
- *AOT C code generator (`pluma::emit_c`):* translates a checked `Program` + compiled `VM`
  into a self-contained C11/C++20 translation unit. Each `Proto` becomes a generated C
  function (`pluma_fn_<idx>`) specialized to its instruction operands with direct `goto`
  control flow, inline `f32`/`f64`/integer/bitwise/comparison/`vec2` operations, non-object
  register dataflow analysis (`analyze_non_obj_regs`) to elide refcount traffic on scalar
  and vector temporaries, and a coroutine `switch (*ctx.pc_ptr)` resume table for functions
  that `await` or `yield`.
- *Source-less runtime metadata:* the generated C translation unit embeds static tables for
  all interned names, `RecordRT` (classes/structs, fields, `@export`, `weak`, `@acyclic`
  `Untraced`/`Promised` flags, vtables), `EnumRT`, globals, `Proto` constants, local-variable
  debug ranges (`LocalDebug`), and a deduplicated `SpanLoc` pool (`file:line:column` + source
  slice text). A native build (`ScriptForm::Native`, `.cuervo/scripts.so`) ships with zero
  `.plm` source files while preserving bit-identical simulation, full panic stack traces,
  live-local inspection (`PanicFrame::locals`), and use-after-free diagnostics.
- *Determinism:* `pluma::compile_c_module` compiles the generated C code with
  `-std=c11 -O2 -ffp-contract=off -fno-fast-math -g0 -fno-ident -Wl,--build-id=none` so
  floating-point arithmetic and exported `.pak` archives are byte-for-bit deterministic and
  match the bytecode VM (`examples/shmup` reaches state hash `a3e57e79964421d3` at tick 240
  in both VM and AOT native modes).

### 5.13 Hot reload
On save, the module is recompiled and swapped in. Live objects keep their state:
- fields with the same name and type keep their values;
- new fields get their default values;
- removed fields are dropped;
- fields whose type changed are reset, with a warning;
- running coroutines restart from the beginning.
A full scene reload is always available as a fallback.

**Clarified (Phase 3, `pluma/reload.hpp`, `cuervo/script/`):** the whole project is
recompiled into a fresh VM and live state is migrated by name: from the scene root, the
globals (incl. statics) and the running coroutines. Types match by type name; enum values
by variant name; strings and vectors are shared; closures survive when a function with the
same stable key (lambdas: enclosing function + ordinal) and capture count exists, else
become null with a warning; host data (e.g. an emitter's particles) moves with its object.
Restarted coroutines get their original (migrated) arguments; `ready()` is not called
again. A compile error keeps the running game and shows the diagnostics. The runner
watches `scripts/**/*.plm`; F5 forces a reload and F6 restarts the game from scratch.

### 5.14 Tooling
Formatter (enforces the style guide), language server (completion, go-to-definition,
diagnostics) for the editor and VS Code, a debugger over DAP, and a profiler view.

**Clarified (Phase 8 — Formatter & LSP, `pluma/format.hpp`, `pluma/lsp.hpp`, `cuervo/script/script_host.hpp`):**
- *Formatter (`pluma::format_source`, `pluma::format_script_source`, `pluma::check_style`):*
  parses a `.plm` source file while collecting all `//` and `/* ... */` comments in a side
  stream, normalizes recoverable fix-it syntax errors (`?` spacing in ternaries, missing
  commas between enum variants), and emits canonical 4-space K&R Pluma code with trailing
  commas on multiline lists, maps, calls, struct literals, and enums. `check_style` enforces
  the §6 naming rules (`PascalCase`, `snake_case`, `SCREAMING_SNAKE_CASE`, `#snake_case`)
  and attaches `FixIt` replacements. Exposed via `plm fmt [--check|--write|--lint]` and
  `cuervo-tools fmt <file-or-project> [--check|--write]`.
- *Language Server (`pluma::lsp::Server`):* standalone in `pluma/` with zero dependency on
  `cuervo/`. Analyzes a multi-file workspace (`scripts/**/*.plm`) together with any injected
  prelude parts and native symbol names, building a unified symbol occurrence index over AST
  declarations, type expressions, patterns, and typed IR (`tir`). Provides diagnostics,
  hover (with leading `//` doc comments), go-to-definition, find references, document
  highlights, rename refactoring (rejecting prelude edits), dot/`?.`/`#name`/`@attribute`/scope
  completion (patching incomplete `.` receivers in-memory so the type checker resolves the
  receiver type), signature help, document & workspace symbols, formatting, and quickfix code
  actions over JSON-RPC 2.0 (`Content-Length` stdio framing). `cuervo::script::default_lsp_options()`
  injects the 7 engine prelude files (`cuervo/math.plm` .. `cuervo/ui.plm`) so `cuervo-tools lsp [<project>]`
  provides full completion, hover, signature help, and navigation for the engine API.

**Clarified (Phase 8 — Debugger, `pluma/debug.hpp`, `pluma/dap.hpp`, `runtime/dap_cli.hpp`):**
- *VM hooks (`pluma::DebugHooks`):* `on_line` before the first instruction of a source line in a
  frame (function entry, a new line, a backward jump: each pass of a one-line loop), balanced
  `on_call`/`on_return` (coroutine suspend/resume and panic unwinding included), `on_panic`
  before the stack unwinds, `on_code_loaded` after a compile and before the code runs. The
  interpreter loop is compiled twice (`execute_loop<bool>`), so a VM without line hooks pays
  nothing (an always-on check cost ~8% on a dispatch-bound benchmark); hooks installed while a
  loop runs apply from the next script call. Compiler glue (post-statement cleanup, the implicit
  return) carries an empty span: no line, so stepping never jumps back to an enclosing line.
  Inspection: `VM::stack_frames`, `frame_locals`, `children` (fields, elements, vector
  components), `code_lines`. Bytecode only: AOT-native builds (5.12) aren't debuggable.
- *Debug adapter (`pluma::dap::Debugger`, in `pluma/`, no engine dependency):* DAP line
  breakpoints (moved to the next line with code, re-checked on every compile and hot reload),
  step in / over / out by frame depth, pause (flagged from the transport's reader thread, so a
  busy loop still stops), stop on panic (exception filter `panic`, default on) with the
  panicking frame's locals intact, stack trace, Locals/Globals scopes, expandable values,
  `evaluate` of variable paths only (`self.enemies[2].hp`; no code runs while stopped, so the
  simulation can't change), and `source` for frames in the engine prelude.
- *`cuervo --dap`:* the runtime is the adapter on stdin/stdout (Content-Length framing); the
  `launch` request names the `project` (+ `backend`, `stopOnEntry`, `noDebug`). Logs and script
  `print` become `output` events, nothing else touches stdout (`log::set_stdout_free`). The game
  starts after `configurationDone`; while stopped the window keeps pumping events, and the frame
  clock restarts after a stop instead of catching up. `tests/dap_session.py` drives a full
  session against `tests/dap_game`.

**Clarified (Phase 8 — Profiler, `cuervo/core/profiler.hpp`, `pluma/profiler.hpp`, `editor/profiler_panel.cpp`):**
- *Engine zones.* `CUERVO_PROFILE_ZONE("name")` times its scope; `profiler::frame_mark()`
  closes a frame. Each frame stores every zone's inclusive time and entry count in a ring
  buffer of the last 300 frames; `zone_stats()` gives avg/max/last per frame in tree order
  (a zone sits under the zone it was first opened in), `frame_history()`/`zone_history()`
  feed graphs. Names are static strings; sites with the same name share a zone (at most
  64). Only the main thread records (the thread that called `set_enabled(true)`): zones
  opened on job workers are ignored, which keeps recording lock-free and the per-frame
  numbers additive. Recording is off until enabled (`--profile`, the editor panel); a
  disabled zone is one relaxed atomic load. The clock is injectable (tests).
  Instrumented: the runtime frame (`frame` > `simulation`, `render` > `begin_frame`/`draw`/
  `flush`, `end_frame`), `GameSession::tick` (`tick`), `ScriptHost::tick` (`scripts` >
  `coroutines`, `update`, `particles` > `simulate`/`spawn`/`colliders`/`hits`, `on_hit`,
  `scene flush`; then `gc`), and the editor frame (`editor frame` > `play`).
- *Determinism.* Profiling reads `steady_clock`, which §3 forbids *in simulation paths*:
  here it only observes and nothing it records reaches the simulation. Verified: the
  shmup's tick-240 hash is `a3e57e79964421d3` with and without `--profile` (1 and 4
  threads), and a replay recorded under `--profile` verifies unprofiled tick by tick.
- *Script profiler* (`pluma::ScriptProfiler`, in `pluma/`, no engine dependency): a
  `DebugHooks` using only the `calls` event; per function (aggregated by `Proto::key`, so
  one profiler spans the VMs of hot reloads) it counts entries and measures inclusive and
  self time (inclusive minus script callees; natives count as self; a recursive
  function's inclusive time is its outermost frame's). A VM leaves a coroutine's frames on
  suspend and enters them on resume, so suspended time isn't counted and a resume counts
  as an entry ("calls" are entries); panics unwind every frame, so the stack stays
  balanced. `snapshot()` sorts by self time; `report()` is the text table. AOT-native code
  (§5.12) has no hooks and is not profiled. A VM has **one** `DebugHooks`: a debugger
  session simply doesn't profile (no multiplexer). `ScriptHost::set_script_profiling`
  (and `HostOptions`/`SessionOptions::profile_scripts`) installs it on the current VM and
  on each new one a reload makes, unless other hooks are installed.
- *`cuervo --game <dir> --profile`*: enables both and logs at exit (`profile |` lines) the
  zone table over the last 300 frames and the top 20 script functions (calls, self ms,
  inclusive ms, % of script time). Combines with `--bench`, `--quit-after`, `--record`.
- *Editor Profiler panel* (bottom dock; `cuervo-editor --profile` starts it recording):
  *Record* and *Reset*, graphs of frame ms and simulation ms (the `tick` zone), the zone
  table (avg/max ms, entries per frame), and while playing, the script table (function,
  calls, self, inclusive, % self) since Play or Reset. Recording follows every new play
  session.
- *Compiled out.* CMake `CUERVO_PROFILING` (default ON): OFF makes the zone macros and
  `frame_mark()` empty (shipping builds); `--profile` then logs only the script table,
  which costs nothing unless enabled (no hooks installed). `CUERVO_TRACY` (default OFF,
  needs `CUERVO_PROFILING`) adds Tracy v0.11.1 (pinned in `cmake/Dependencies.cmake`,
  `TRACY_ON_DEMAND`): every zone (on every thread) and frame mark also goes to Tracy.
- *Overhead* (shmup, release build, GL offscreen, `--bench 5 --press 30:ui_accept`, 3 runs
  each): sim 0.069–0.083 ms/tick without, 0.099–0.103 with `--profile` (~+30%); render CPU
  (the scripts' `draw`) 0.060–0.062 vs 0.075–0.077 ms/frame (~+25%). Nearly all of it is
  the script profiler's per-call work (two virtual hook calls, two clock reads, a cache
  lookup): the shmup's draw alone makes ~220 script calls per frame, i.e. roughly 70 ns
  per call. Engine zones are a few dozen per frame and don't show in these numbers.

## 6. Milestones

A row whose "Done when" ends in "(done ...)" is finished; every other row is planned. The
support status of what exists is in [STATUS.md](STATUS.md).

| # | Phase | Done when |
|---|---|---|
| 0 | **Skeleton:** CMake, SDL3 window, fixed-timestep loop, input actions, logging, `Vec<T,N>`, RHI interface + GL and Vulkan backends (Vulkan pulled forward from Phase 5) | A window clears to a color and responds to actions, on Linux and Windows (done on Linux and, with milestone 10.5a, on Windows) |
| 1 | **Language core** (in parallel): lexer, parser, type checker, bytecode, VM, refcounting, REPL | Closures, structs, classes and enums run in the REPL; language test suite passes (done: `pluma_tests`) |
| 2 | **Vector rendering:** SDF batcher, SVG import + tessellation, MSDF text, camera, scale modes | 50k SDF shapes at 60 fps; an SVG character drawn sharp at any zoom (done: `examples/bench`, `cuervo --demo vector`) |
| 3 | **Integration:** bindings, scripts on nodes, coroutines, hot reload | **A shmup stage written entirely in the language** (done: `cuervo --game examples/shmup`) |
| 4 | **Gameplay systems:** particle system (gameplay + visual), collision, audio, UI nodes, binary scenes + dump | A playable boss fight with menus (done: `examples/shmup`) |
| 5 | **Web + tiers:** WebGL2 export, Vulkan feature parity, tiers + migration report | The same project runs on Linux, Windows and the web (Linux + web done and verified, docs/WEB.md; Windows validation is in progress on Intel Arc integrated graphics and an RTX 4070 on the Core Ultra 9 185H laptop: the Clang debug build passes `ctest` and reaches the Linux hashes, docs/WINDOWS.md) |
| 5.5 | **Performance core** (§4.15): activate the job system, explicit SIMD (AVX2 / WASM SIMD128), sub-seeded RNG streams, secondary spatial hash for large shapes | The particle demo's sim time scales with worker count, SIMD and scalar paths are bit-identical, and the replay hash is unchanged at 1 and N threads (done: §4.15 "Verified") |
| 6 | **Editor:** scene view, inspector, assets, play-in-editor, replay viewer | A level can be built without code (done: 6A, the editor shell, §4.13 "Clarified (Phase 6A)" -- `examples/editor_level` was built from editor operations only and runs with `cuervo --game`; 6B, the asset browser, input-map editor, play-in-editor with hot reload, replays and the debugger, and the tier report, §4.13 "Clarified (Phase 6B)") |
| 7 | **Release path:** AOT-to-C, cycle collector, manual memory, packing, export | A shipped build runs measurably faster than the VM (done: 7A cycle collector + manual memory, §5.10 "Clarified (Phase 7A)"; 7B AOT-to-C compilation, §5.12 "Clarified (Phase 7B)"; 7C packing + export, §4.11 "Clarified (Phase 7C)", docs/EXPORT.md. Both source and AOT native exports of `examples/shmup` reach the dev state hash `a3e57e79964421d3`, and AOT runs measurably faster than the bytecode VM) |
| 7.5 | **Modular footprint:** Feature-stripping build system, dead-code elimination, unreferenced module pruning | An exported project strips unused engine subsystems (e.g., audio, unused RHI features, UI) resulting in measurably smaller release binaries and web payloads (done: §4.11 "Clarified (Milestone 7.5)", docs/EXPORT.md "Stripped players". Per-export Release players of `examples/tictak`, `examples/bench` and `examples/shmup` are 40%, 37% and 20% smaller than the full Release player on Linux, and their web `index.wasm` 18%, 15% and 4% smaller than the full web runtime; each runs its exported pack to its dev run's state hash, the web ones in headless Chrome. stripped Windows players build on Windows since 10.5a) |
| 8 | **Tooling:** formatter, LSP, debugger, animation (cutout, skeletal, morph), docs (expanded as 8.4) | Someone else makes a game with it (tooling done, animation deferred to 9.4: formatter and LSP, debugger over DAP, and profiler, §5.14 "Clarified (Phase 8 — ...)"; the VS Code extension `tools/vscode-pluma`; docs/GUIDE.md with the `examples/tutorial` game and the generated docs/API.md, expanded further by 8.4. All of 8.1-8.9 are done; the done-when needs an outside user to confirm) |
| 8.1 | **Brand integration** (§4.16): Cuervo Mark, wordmark, palette and type from the design kit, synced into `assets/brand/`, plus generated color tokens | The editor and runtime show the mark as window icon and splash in light and dark themes, and a sync script regenerates the brand assets from the kit (done: §4.16 "Clarified (Milestone 8.1)". `tools/sync_brand.py` syncs `assets/brand/` and records the kit commit; `cuervo` and `cuervo-editor` set the embedded app icon as their window icon; the editor's splash draws the imported mark and wordmark in the Bone and Raven themes on Vulkan and GL, and its UI switches between the two token-generated themes) |
| 8.2 | **Node library** (§4.16): Shape, Path, Sprite, Text, Camera2D, Timer, Area, Emitter2D, AudioPlayer as prelude nodes | A level using every new node is built in the editor without code and round-trips through scene files with the same state hash (done: §4.16 "Clarified (Milestone 8.2)"; `examples/node_level` from `level.cmds`, the `node_level` test: editor-built, dump/build round trip and editor-played runs all reach `115a6d76bb125635` at tick 240) |
| 8.3 | **Editor UI overhaul** (§4.16): themed with the brand tokens, gizmos, toolbar, command palette, multi-select, undo history, welcome screen | The `examples/editor_level` workflow is redone with gizmos and the palette, with no raw ImGui-default styling left (done: §4.16 "Clarified (Milestone 8.3)". The editor moved to ImGui's docking branch with a DockBuilder default layout and saved dock layouts; `editor_ui_tests` fails on any ImGui-default color; `examples/editor_level/level.cmds` uses the palette and gizmo verbs and still produces the same scene) |
| 8.4 | **Documentation** (§4.16): manual, Pluma guide, generated API/node reference, first-game tutorial, branded static site | Every public prelude symbol is documented (enforced by a test) and the tutorial game builds in CI (done: §4.16 "Clarified (Milestone 8.4)". `tools/build_site.py` builds the branded static site and `docs_site` checks its links; `cuervo-tools docs --check-coverage` (`api_docs_coverage`) enforces a doc comment on every public prelude symbol; the tutorial runs, exports and matches GUIDE.md in CI) |
| 8.5 | **Documentation reconciliation** (§4.16): align design, user and API documentation with the actual MIT license, current milestone state, implemented systems, and support status; separate shipped, experimental, planned and unsupported behavior | A fresh reader can determine what is implemented and supported without relying on historical milestone notes; no active document claims that the license is undecided or that completed systems are still only planned(done: §4.16 "Clarified (Milestone 8.5)". [STATUS.md](STATUS.md) is the support matrix (shipped, experimental, planned, unsupported; desktop and web; Vulkan and GL), linked from the README, the guide and the docs site; the license summaries in the README, the guide, §2 and §4.18 match `LICENSE.md` (MIT License); stale "planned"/"later"/"not yet" notes for finished systems, wrong commands and counts, phase labels in user-facing pages and dates in status lines were fixed) |
| 8.6 | **Performance baselines** (§4.15): publish repeatable simulation, render, memory and export-size measurements for representative particle/shape loads, native backends, WebGL2, debug and release builds | Benchmark commands record versioned baselines and flag regressions when agreed thresholds are exceeded; the 1k/10k/50k/100k workload results are reproducible on the reference machine (done: §4.15 "Clarified (Milestone 8.6)". `tools/bench.py record|capacity|compare` and `docs/perf/baseline-5800x3d-rtx3060ti-linux.json`; three runs agree within a few percent on the reference machine and every workload reaches one state hash on both backends, both builds and 1 or 8 jobs; `docs/PERFORMANCE.md` has the thresholds, the capacity table and what was measured. Known limit: the web baseline is recorded by hand) |
| 8.7 | **Stabilization pass** (§4.12, §4.13, §5): prioritize Pluma semantics, scene serialization and migrations, editor workflows, replay behavior and export reliability before another major subsystem | A clean checkout can build the reference examples, round-trip scenes, hot-reload scripts, verify replays and launch source, AOT and stripped exports without known correctness regressions (done: §4.12 and §4.13 "Clarified (Milestone 8.7)", LANGUAGE.md 9.25. `examples_sweep_<name>` takes every example through runs, scene round-trips, replays, hot reload and exports; Pluma has a VM/AOT differential corpus and an integer reference model. Known limit: hot reload is not bit-exact for projects holding emitters or coroutines) |
| 8.8 | **Supported-features contract** (§4.4, §4.16): classify features as stable, experimental, desktop-only, Web-compatible or unsupported; document the guarantees and limits of `compatible` and `full` render tiers | The README and generated/user documentation expose the same support matrix, and tier checks reject configurations that claim support the selected target does not provide(done: §4.4 "Clarified (Milestone 8.8)". [STATUS.md](STATUS.md) and the README summary are generated from `docs/support.json`, which the tier check and the exporter also read; `tier check --platform` and `export` refuse a Full project for the web and AOT when the export host is not the target) |
| 8.9 | **End-to-end user workflow** (§4.11, §4.13): automate the fresh-checkout path from project creation through scene editing, running, exporting and launching the exported game | A workflow smoke test creates or opens a project, edits and saves a scene, runs it, exports it, launches the shipped artifact outside the project directory and verifies its exit status and deterministic state hash(done: §4.11 and §4.13 "Clarified (Milestone 8.9)". `workflow_smoke` starts from an empty directory with the shipped binaries: `cuervo-editor --new-project`, an editor command script that edits, saves and plays a scene, a hand-added script with a hot reload, bytecode and AOT (and stripped with `CUERVO_SLOW_TESTS`) exports launched from another directory, and the same state hash in the editor, `cuervo --game` and the shipped game) |
| 9 | **Advanced Local Systems** (§4.17): transform hierarchy and more nodes, Pluma library, UI widgets, animation, tilemaps and navigation, Box2D v3 physics, 2D SDF lighting/raymarching | A physics-based platformer with 2D lighting is playable (done: milestones 9.1 to 9.8, §4.17; `examples/lantern_cave` and its `lantern_cave` test) |
| 9.1 | **Transform hierarchy and node gaps** (§4.17): parent-relative transforms (toggleable), `Line2D`, `Polygon2D`, `Marker2D`, `VisibleOnScreenNotifier2D` | Moving a parent moves its children when the hierarchy is on, and world-space scenes behave exactly as before with it off (done: §4.17 "Clarified (Milestone 9.1)"; `hierarchy_tests` move a parent and check the child with the hierarchy on and off, and the shmup, tictak and editor_level hashes are unchanged with it off) |
| 9.2a | **Pluma collections, strings and language** (§4.17): `Map`/`Set`/fixed arrays/`StrBuilder`, `List` and `str` methods, conversions, property accessors, `Result` and `?`, method values | A script uses each feature and the language test suite passes under the VM and AOT (done: §4.17 "Clarified (Milestone 9.2a)"; `tests/pluma/library_tests.cpp` runs the same program in the VM and AOT) |
| 9.2b | **Pluma math, geometry and random** (§4.17): scalar/integer/vector math, `Rect2`, `Transform2D`, geometry helpers, noise, seeded `Random` streams | Every function is bit-identical across SIMD, scalar, VM and AOT, and the replay hash is unchanged (done: §4.17 "Clarified (Milestone 9.2b)") |
| 9.3 | **UI widgets** (§4.17): `GridContainer`, `ScrollContainer`, Flow/Split/AspectRatio, `Image`/`TextureRect`, `ProgressBar`, `LineEdit`, `OptionButton`, tabs, popups | A settings and inventory menu built from them is driven by keyboard, gamepad and pointer, and replays (done: §4.17 "Clarified (Milestone 9.3)" -- `examples/ui_menus` is driven by keyboard, gamepad and pointer in `tests/ui_menus.cmake`; each run is recorded and replays with identical state hashes on 1 and 8 threads, and the shmup hash `a3e57e79964421d3` is unchanged) |
| 9.4 | **Animation** (§4.17): `AnimationPlayer` (property tracks), `Tween`, `AnimationTree` state machine | An animated character blends between states and the replay hash is unchanged at 1 and N threads (done: §4.17 "Clarified (Milestone 9.4)"; the `anim_character` ctest runs `examples/anim_character` with scripted input to tick 240 on `--jobs 1` and `--jobs 4` with one state hash, verifies its recorded replay on 1 and 4 threads and round-trips its scene) |
| 9.5 | **Tilemaps and navigation** (§4.17): `TileMapLayer`, tilesets, autotiling, `NavigationRegion2D`, `NavigationAgent2D` | A top-down level is painted in the editor and an agent paths around obstacles (done: §4.17 "Clarified (Milestone 9.5)"; `tests/topdown_nav.cmake` runs `examples/topdown_nav` to tick 240 on 1 and 4 threads with pinned state hash `882ea24679ca2d41`, verifies replay recording on 1 and 4 threads, and round-trips the scene through text dump) |
| 9.6 | **Physics** (§4.17): Box2D v3 behind the physics interface; `StaticBody2D`, `CharacterBody2D`, `RigidBody2D`, `Area2D`, `CollisionShape2D`, raycasts, joints | A platformer character runs, jumps and pushes bodies deterministically (done: §4.17 "Clarified (Milestone 9.6)"; `tests/platformer.cmake` runs `examples/platformer` to tick 240 on 1 and 4 threads with pinned state hash `e458fa6c80754cc5`, verifies replay recording on 1 and 4 threads, and round-trips the scene through text dump) |
| 9.7 | **2D lighting** (§4.17): SDF lighting/raymarching, `PointLight2D`, occluders | Lights and shadows render identically on Vulkan and GL (done: §4.17 "Clarified (Milestone 9.7)"; the `rhi_parity` test compares two lighting scenes and a frame of the lit `examples/lantern_cave`: GL and GLES identical, Vulkan within one level per channel) |
| 9.8 | **Integration** (§4.17): editor support for all of the above and a sample game | A physics-based platformer with 2D lighting is playable (done: §4.17 "Clarified (Milestone 9.8)"; the `lantern_cave` test plays `examples/lantern_cave` through every gem and out of the door, with the same state hash on 1 and 4 threads and a replay that verifies on both; the editor gained the animation timeline, lighting preview and icons for the 9.x nodes) |
| 10 | **Next-Gen Web:** WebGPU, WASM multithreading (SharedArrayBuffer)| A stable benchtest runs natively in the browser (WASM threads done early: docs/WEB.md "Threads", `CUERVO_WEB_THREADS`. **Design decision:** the WebGPU backend's shaders are translated SPIR-V → WGSL by **naga** (Rust; a host build tool), keeping one GLSL 450 source per shader; WebGPU starts only after Phases 8 and 9 are done) (done: 10.1-10.4; the browser benchtest runs stably on WebGPU with threads, PERFORMANCE.md "Web") |
| 10.1 | **WebGPU backend skeleton** (§4.4): naga pinned as a host tool, `cuervo_add_shaders` also emits WGSL from the same SPIR-V, an `rhi` WebGPU device that clears, draws a triangle and presents on native and in the browser | The clear and triangle scenes of `rhi_parity` render through WebGPU and match GL; the native WebGPU implementation used for testing is chosen here and recorded in §4.4 (done: §4.4 "Clarified (Milestone 10.1)" -- wgpu-native natively, emdawnwebgpu in the browser; the `rhi_parity` test renders every scene, the many-frames check and the shmup and `lantern_cave` frames headless on WebGPU and compares them with GL, and the shmup reaches `a3e57e79964421d3` on WebGPU in headless Chrome) |
| 10.2 | **WebGPU feature parity** (§4.4–4.6): every RHI feature in the `cuervo/rhi/README.md` audit table (buffers, textures, pipelines, stream buffers, frame limit, particle compute, SDF lighting) on WebGPU | `rhi_parity` compares every scene on WebGPU against GL within the Vulkan tolerance, and the WGSL for every engine shader is generated with no hand-written WGSL (done: §4.4 "Clarified (Milestone 10.2)" -- points are 1 px everywhere; every scene equals Vulkan exactly and GL within the Vulkan tolerance; stream buffers, the frame bound, particles (shmup) and SDF lighting (`lighting_*`, `lantern_cave`) all compared; WGSL for all 15 shaders generated) |
| 10.3 | **WebGPU web runtime and export** (§4.11): the web build uses WebGPU when the browser has it and falls back to WebGL2 otherwise; `export --platform web` ships both; works with `CUERVO_WEB_THREADS` | `examples/shmup` and `examples/lantern_cave` run in headless Chrome on WebGPU and on the WebGL2 fallback, reaching their dev state hashes (done: §4.4 "Clarified (Milestone 10.3)" -- one `.wasm` with both backends, the page requests the WebGPU device before `main` (no JSPI); shmup `a3e57e79964421d3` and `lantern_cave` `2dd8beccd71bc67e` on WebGPU and on the WebGL2 fallback, single-threaded and with threads) |
| 10.4 | **Browser benchtest** (§4.6): `examples/bench` in the browser on WebGPU with threads, `tools/bench.py` browser runs and recorded web baselines in docs/perf | The browser benchtest runs stably on WebGPU (no frame-time cliffs over its run, regression compare passing), which closes milestone 10 (done: §4.15 "Clarified (Milestone 10.4)" -- `bench.py --web`, `builds.web` in the reference baseline: 100k shapes at 390 FPS on WebGPU with 8 threads, worst cliff ratio 1.21, a second record compares clean; fixed on the way: a stale WebGPU vertex binding, the 60 Hz web cap and the millisecond web clock) |
| 10.5 | **Windows and language APIs** (§4.18): Windows builds, module/package system, plugin and embedding API, FFI | The shmup builds, passes its tests and exports on Windows with the same state hash as Linux, and a third-party plugin and package are used by a game (done: 10.5a-10.5d below) |
| 10.5a | **Windows builds** (§4.18): MSVC and MinGW builds, CI, determinism check, signed prebuilt editor and export templates | `ctest` passes on Windows and a Windows export of `examples/shmup` reaches the Linux state hash (done: §4.18 "Clarified (Milestone 10.5a)"; `ctest` passes with MSVC, MinGW-w64 GCC and Clang, and `export_shmup` ships the GUI-subsystem player and reaches `a3e57e79964421d3` from another directory with bytecode and AOT scripts. The release workflow builds attested editor, tools and export templates, Authenticode-signed when a certificate is configured (docs/WINDOWS.md)) |
| 10.5b | **Modules, packages and unit tests** (§4.18): `import`, namespaces, project dependencies, versioning, plus native `@test` runner in Pluma | A game imports a package from another directory with no global name clashes, and `cuervo-tools test` / `plm test` runs pure-logic `@test` functions with pass/fail reporting in CI (done: §4.18 "Clarified (Milestone 10.5b)"; `examples/garden` imports `tween` from `examples/libs/tween` and the vendored `palette` beside its own same-named declarations, and reaches its dev state hash from source and AOT packs and in headless Chrome; `@test` runs through `plm test` and `cuervo-tools test`) |
| 10.5c | **Plugin, embedding and FFI APIs** (§4.18): stable C ABI for native modules, C++, Rust and C# bindings, Pluma FFI to C libraries, API versioning | A native plugin written in each of C++, Rust and C# adds a node and a function that a script uses (C# on desktop), in the VM and in AOT where supported (done: §4.18 "Clarified (Milestone 10.5c)"; `examples/plugin_demo` uses nodes and functions from `orbit` (C++), `noise` (Rust) and `hues` (C#). `orbit` and `noise` linked statically into AOT modules match their VM results, and C# AOT is refused as decided. `@extern` calls libc and libm in the VM and AOT, and `pluma/embed/pluma_embed.h` is exercised from plain C (docs/PLUGINS.md)) |
| 10.5d | **Plugin ecosystem, licenses and contribution** (§4.18): plugin manifest and registry, project license, third-party notices, contribution guides | The repo has a license, generated third-party notices and contribution docs, and a plugin published to the registry installs into a fresh project and passes the checks (done: §4.18 "Clarified (Milestone 10.5d)". `cuervo-tools plugin publish` puts `examples/plugins/orbit` in a directory registry with SHA-256 checksums and its conformance hash, and `plugin install` vendors it into a fresh project, pins it, and verifies the license, the engine and ABI ranges, the library, the checksums and the conformance hash there. Exports carry `licenses/PACKAGES.md`, and CONTRIBUTING.md, CODE_OF_CONDUCT.md, SECURITY.md and the generated docs/ROADMAP.md are in place) |
| 11 | **Production features** (§4.19): rollback/lockstep networking, raster processes and sprite pipeline, custom shaders and post-processing, localization and text shaping, profiler and save/settings API | A networked two-player game with a localized UI, a custom post-process shader and an atlas-based sprite scene is shipped and profiled |
| 11.1 | **Networking** (§4.19): transport, lockstep and rollback on the deterministic sim, HTTP and WebSocket | Two clients on different machines (native and web) stay in sync, with the same state hash every tick |
| 11.2a | **Raster processes** (§4.19): image import, texture atlasing, sprite sheet slicing, SVG rasterized fallback, textured quad batcher, `Sprite2D`/`AnimatedSprite2D`, 9-slice, filtering modes | An atlas-batched sprite scene with 9-slice panels and rasterized SVG fallbacks renders identically on Vulkan, GL and WebGL2 |
| 11.2b | **Shaders, post-processing and linear color** (§4.19): user shaders (GLSL 450), parameter binding in inspector, off-screen render targets, multi-pass post-processing (bloom, blur, blits), optional linear color space | A custom user shader and a multi-pass post-process effect render identically on Vulkan and GL; `project.toml` supports `color_space = "srgb"` or `"linear"` |
| 11.2c | **Text scenes on save** (§4.19): human-readable text format becomes the default save format in `scenes/`; automatic compile to binary `.pak` on export | Saving in the editor writes human-readable scene text that Git can 3-way auto-merge without binary merge conflicts; release exports pack them into binary `.scene` files with zero runtime overhead |
| 11.3 | **Localization and text** (§4.19): translation tables, complex-script shaping, bidirectional text, IME | A UI switches between English, Arabic and Japanese at run time and `LineEdit` accepts IME input |
| 11.4 | **Profiling and save API** (§4.19): frame/job/GPU profiler, memory profiler, general save/load and settings API | A profile of the shmup shows per-system tick cost, and a game saves and restores progress and settings |
| 12 | **Content tools, AI and accessibility** (§4.20): dialogue and cutscene tools, particle and shader editors, behavior trees and steering, accessibility, the ECS resolution | A narrative game with AI enemies and a screen-reader-usable menu is built mostly in the editor |
| 12.1 | **Content tools** (§4.20): visual node-graph canvas (`imgui-node-editor`), dialogue and cutscene/timeline editor, particle editor UI, visual shader editor | A cutscene with dialogue and a particle effect is authored without code using visual graph panels |
| 12.2 | **AI** (§4.20): behavior trees, state-machine tool, steering behaviors | An enemy with a behavior tree is built and debugged in the editor |
| 12.3 | **Accessibility and editor polish** (§4.20): AccessKit integration (screen-reader support), UI scaling, remapping, desktop file drag-and-drop, crisp DPI font oversampling | The shmup menus can be used with a screen reader and at 200% UI scale; platform screen readers read UI elements via AccessKit; desktop file drag-and-drop into viewport functions |
| 12.4 | **The ECS resolution** (§4.20): finalize the Hybrid Architecture (Godot-style `Node2D` hierarchy for scene logic + contiguous internal SoA pools exposed via Pluma query loops) | The decision is documented: scripts iterate bulk entities via deterministic queries (`for e in query(Enemy)`) in the job system without exposing raw low-level ECS internals |
| 13 | **Extended Platforms & drop-in console SDKs** (§4.21): macOS (MoltenVK), iOS, Android (SDL3 + GLES 3.0), mobile lifecycle/thermal mitigation, drop-in Console C ABI | The shmup runs natively on a Mac, iPhone, and Android device; developers with private console DevKits drop closed-source C ABI shims in without engine forks |
| 14 | **Zero-bloat 3D Expansion** (§4.22): 3D rendering pipeline, 3D math (`Vec<T,N>`), 3D scene/camera transforms, glTF import, PBR materials | A full 3D scene runs with PBR materials and 3D transforms; 2D exports strip all 3D pipelines via Milestone 7.5 dead-code elimination with 0 bytes overhead |
| 15 | **First-party jumpstart ecosystem** (§4.23): official C ABI plugin wrappers (`cuervo-steamworks`, `cuervo-spine`, `cuervo-fmod`, `cuervo-console-template`) and genre showcase demos | Shipped templates for a 2-player rollback brawler, a 100k-particle survivors game, and turn-key Steamworks/Spine/FMOD wrappers unblock commercial shipping |
Both backends exist from Phase 0, so every RHI feature is built for both at once and the
interface can't drift toward either API.

## 7. Conventions and assumed defaults

### 7.1 Core design decisions
- **Coordinates:** Y-down, origin top-left, units are pixels of the virtual coordinate space
  (SVG coordinates map 1:1). A positive rotation turns clockwise on screen.
- **Angles:** radians everywhere; the language has a `deg` literal suffix (`45deg`).
- **Integer overflow:** panics in development builds, wraps (two's complement) in release.
- **VM:** register-based. **Batch ops:** general `foreach`, with slow-path warnings.
- **Determinism guarantee:** bit-identical on the same platform (not across platforms).
- **Workflow:** git, clean modular milestones.
- **Names:** the engine is **Cuervo Engine** (directory/namespace/library `cuervo`, macros
  `CUERVO_*`) and its language is **Pluma** (directory/namespace/library `pluma`); script
  files use `.plm`. The commands are `cuervo` (the runtime player) and `plm` (the REPL),
  plus `cuervo-tools`; nothing is installed as bare `pluma`, which would clash with MATE's
  `pluma` text editor.

### 7.2 Core conventions
| Area | Assumption |
|---|---|
| Script lifecycle | `ready()` once; `update(dt: float)` every fixed tick (`dt` is constant); `draw(g: Canvas)` every rendered frame (render only, may not change state); `exit()` on removal |
| Integer math | `/` truncates toward zero; `%` takes the dividend's sign; division by zero panics; `float as int` truncates and saturates |
| Color | Colors are authored in sRGB and blended in sRGB space by default (matching browsers and vector tools; UNORM swapchain). Optional linear color space (hardware sRGB framebuffers, gamma-correct blending) in 11.2b |
| Text | UTF-8; FreeType + HarfBuzz for shaping, then MSDF atlases; right-to-left and CJK shaping and bidi layout are planned (11.3); today text is MSDF without shaping |
| Localization | Planned (11.3): string tables (CSV) keyed by `#name`, with `tr(#key)` in scripts; switchable at runtime |
| General physics | Box2D v3 behind the physics interface, as an optional module (implemented, 9.6) |
| Audio formats | WAV, OGG Vorbis, FLAC, MP3 (miniaudio's decoders) |
| Fonts | TTF/OTF imported into MSDF atlases |
| Project layout | `project.toml`, `assets/`, `scenes/`, `scripts/`, `.import/` (generated, gitignored) |
| Save data | Per-user directory from `SDL_GetPrefPath`; `Scene.save` today, a general `Save` API with versioned data is planned (11.4) |
| Window | VSync on; fullscreen/windowed/borderless toggle; 60 Hz tick (configurable) |
| Profiling | Tracy integration, compiled out of release builds |
| AOT export | Uses the system C compiler (Clang/GCC/MSVC); web export uses Emscripten |
| Editor layout | Godot-like dock layout: scene tree left, viewport center, inspector right, console bottom; panels are dockable (4.16) |
| Accessibility | Rebindable input today; a UI scale setting and a reduced-flashing toggle exposed to games are planned (12.3) |
| Platforms | Linux, Windows, Web verified/in-progress; macOS, mobile (Android/iOS) and drop-in console SDKs are Phase 13 |
| Engine versioning | SemVer; scene/project migrations keyed by format version |

### 7.3 Resolved decisions
- Architecture model (milestone 12.4): Hybrid (Node2D hierarchy for scene structure + contiguous internal SoA pools exposed via deterministic query loops).

### 7.4 Validation environments
- **Primary Arch development machine:** AMD Ryzen 7 5800X3D with an NVIDIA RTX 3060 Ti.
  This is the reference environment for day-to-day engine, renderer and web development.
- **Windows validation machine:** Intel Core Ultra 9 185H laptop with Intel Arc integrated
  graphics and an NVIDIA RTX 4070 discrete GPU. The compiler, test, determinism and export checks
  of milestone 10.5a passed there (docs/WINDOWS.md "Validation results").

## 8. Risks
| Risk | Mitigation |
|---|---|
| Scope: engine + language + editor is very large | Strict milestones; every phase ends with something playable |
| Two render backends double the work | GL first; RHI kept small; shared shader source |
| Two script execution paths diverge | One typed IR, one test suite run against both |
| Determinism breaks silently | Replay comparison in CI on every platform |
| Binary scenes cause merge pain | Versioning, migrations, `scene dump` textconv |
| Global class names collide in big projects | Duplicate names are compile errors; editor rename refactoring |
| Runtime tessellation cost (morphing, canvas) | Cache meshes until paths change; prefer SDF for simple shapes |
