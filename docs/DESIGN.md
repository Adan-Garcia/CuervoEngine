# Cuervo Engine & Pluma Design

> Status: v0.1. The engine is **Cuervo Engine**, its scripting language
> is **Pluma**, and script files use `.plm` (see 7.1 "Names"). The
> text below often says just "the engine" and "the language".

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

Non-goals for now: 3D, networking, engine-level modding, macOS, a built-in test framework
for users.

## 2. Decision summary

| Area | Decision |
|---|---|
| Engine language | C++20, CMake |
| Platforms | Linux, Windows, Web (WASM) |
| Dimensionality | 2D. Generic `Vec<T,N>` (vec1…vecN) for extra axes, not 3D transforms |
| Simulation | Deterministic, fixed timestep, seeded RNG, strict float rules |
| Scene model | ECS core (SoA) + node tree on top for authoring |
| Graphics backends | Vulkan (desktop), OpenGL / WebGL2 (web + legacy), behind an RHI |
| Render tiers | `compatible` (exports to all backends) or `full` (Vulkan-only features) |
| Vector rendering | SDF primitives + tessellated SVG + MSDF text + raster fallback |
| SVG | Practical subset |
| Vector animation | Cutout/transform, skeletal + mesh deformation, shape morphing |
| Runtime drawing | Canvas-style script API |
| Resolution | Virtual coordinate space + scale mode; vectors render at native resolution |
| Particles | General particle system: gameplay (CPU, deterministic, collidable) and visual (GPU) modes, native modules + script `foreach`. No dedicated bullet system: bullets are built on particles |
| Collision | Circles + capsules in a spatial hash; general physics is separate and optional |
| Input | Named actions/axes, rebindable, recorded per tick |
| Audio | miniaudio + mixer buses + voice limiting |
| Game UI | Retained UI nodes in the scene tree |
| Assets | Import pipeline (atlases, hot reload) + packed archive for release |
| Scene files | Binary, versioned with migrations, `scene dump` text view for git |
| Editor | Standalone Dear ImGui application |
| Threading | Scripts single-threaded; engine uses a job system (1 thread on web) |
| Netcode | None yet; stay rollback-ready |
| Modding | None at engine level |
| Script execution | Bytecode VM in development + AOT-to-C for release |
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
| License | Undecided; project private for now |
| Names | Cuervo Engine, Pluma language, `.plm` files (7.1) |

## 3. Architecture

```
cuervo/
  core/       fixed-timestep loop, logging, allocators, file system, events, RNG
  math/       Vec<T,N>, Mat, transforms, deterministic trig
  platform/   SDL3 window + input devices, timing
  rhi/        render hardware interface
    vulkan/   desktop backend
    gl/       OpenGL 3.3 / GLES 3.0 / WebGL2 backend
  render/     vector renderer, SDF batcher, mesh batcher, text (MSDF), camera, canvas API
  vector/     SVG parser (subset), tessellator (import + runtime), stroker, animation
  scene/      ECS, node tree, serialization (binary), scene dump
  particles/  emitters, SoA pools, native modules, GPU visual particles
  collision/  shapes, spatial hash, queries
  physics/    optional general physics (later)
  audio/      miniaudio wrapper, buses, voice limiting
  input/      action maps, input frames, recording/playback
  ui/         retained UI nodes, layout, focus navigation
  assets/     handles, importers, file watcher, pack format
  jobs/       job system
  script/     engine bindings for the language runtime
pluma/        standalone: lexer, parser, type checker, bytecode compiler, VM, AOT-to-C, REPL
editor/       Dear ImGui editor, hosts the engine
runtime/      thin player executable for exported games (`cuervo`)
tools/        CLI: import, pack, scene dump, export
docs/
tests/        cuervo/ and pluma/ tests (internal; users get no test framework)
```

**Rule:** `pluma/` does not depend on `cuervo/`. The engine registers its API with the
language through `script/`. This keeps the language testable on its own and gives it a REPL.

### Third-party libraries

| Purpose | Library | License |
|---|---|---|
| Window, input, platform | SDL3 | zlib |
| Vulkan helpers | volk + VMA | MIT |
| Shaders | glslang/shaderc → SPIR-V, SPIRV-Cross → GLSL 330 / ES 300 | Apache-2.0 |
| ECS | EnTT (or in-house) | MIT |
| Audio | miniaudio | MIT-0 / public domain |
| SVG parsing base | nanosvg (extended), or in-house parser | zlib |
| SVG raster fallback | lunasvg | MIT |
| MSDF fonts | msdfgen + msdf-atlas-gen | MIT |
| Font loading (for msdfgen) | FreeType | FTL (BSD-style) |
| Editor UI | Dear ImGui | MIT |
| Images | stb_image | public domain |
| Internal tests | doctest | MIT |

All permissive, so any license chosen later stays open.

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
- Guarantee: bit-identical on the same build and platform. Goal: identical across platforms
  (verified by replay comparison in CI).

### 4.2 Math: `Vec<T,N>`
```cpp
template<typename T, std::size_t N>
struct Vec { T v[N]; /* +, -, *, /, dot, length, normalize, swizzles for N<=4 */ };
using vec2 = Vec<float,2>;  // SIMD-specialized for N = 2..4
```
Transforms are 2D by default: `vec2` position, `float` rotation, `vec2` scale, `int` layer.
Components that need more axes use larger vectors explicitly.

### 4.3 Scene model: ECS + node tree
- **Runtime:** ECS with components in SoA arrays. Hot systems (particles, collision, rendering,
  animation) iterate the arrays directly.
- **Authoring:** a node tree (parent/child, names, scripts) sits on top. Each node is an
  entity plus a `Parent`/`Children` relation. Users mostly see nodes; systems see components.
- Entities are referenced by **generational handles** (`index + generation`), never raw pointers.

**Clarified (Phase 3, `cuervo/script/`):** the node tree is written in the language itself
(`cuervo/script/prelude/scene.plm`: `Node`, `Node2D`, `Scene`), so scripts extend it like
any class and hot reload migrates it like any object. The ECS core stays minimal for now:
the hot data (particles, collision) already lives in SoA systems that nodes drive through
bindings; nodes get entity handles when scene files and the editor need them (Phase 4+).
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
  nothing blocks it. The editor will show the same report.
- *Parity:* every RHI entry point is implemented identically on both backends
  (audit table in `cuervo/rhi/README.md`); `tools/rhi_parity` renders one scene per
  feature through `capture_frame`, and the `rhi_parity` test compares GL 3.3 vs GLES 3.0
  (identical) and GL vs Vulkan (identical except anti-aliased edges: Vulkan's y-flipped
  viewport mirrors the MSAA sample pattern and half-pixel rounding).
- *GLES on desktop:* `--gl-es` / `CUERVO_GL_ES=1` asks the GL backend for an OpenGL ES
  3.0 context, so the web's shader path (GLSL ES 300) is testable without a browser.

**Shaders** are written once in GLSL 450, compiled to SPIR-V for Vulkan and
cross-compiled to GLSL 330 / ES 300 for GL. Shader variants are built at import time, not
at runtime.

**Vector pipeline:**

| Content | Technique |
|---|---|
| Particles, UI panels, primitives | Analytic SDF shapes (circle, capsule, rounded rect, polygon, star, ring), instanced; one draw call per material |
| SVG art | Tessellated to triangle meshes at import; anti-aliased with MSAA where available, otherwise with feathered edge geometry |
| Text | MSDF font atlases |
| SVG filters, blurs | Rasterized to an atlas at the needed scale, cached |
| Canvas API, shape morphing | Runtime tessellator; meshes cached until the path changes |

**Resolution.** A project defines a virtual coordinate space (e.g. 1280×720) and a scale
mode: `fit` (default; renders vectors at native resolution), `integer` (pixel art),
`stretch`. Filtering (`linear`/`nearest`) applies to raster assets only.

### 4.5 Vector assets and animation
**SVG subset:** paths, basic shapes, fills (nonzero/evenodd), strokes (width, joins, caps,
miter limit, dashes), linear and radial gradients, transforms, groups, opacity, clip paths.
Unsupported features (filters, masks, CSS, embedded text) fall back to rasterization, with
an import warning.

**Animation:**
- *Cutout/transform:* SVG groups become parts animated on a timeline (position, rotation,
  scale, opacity, draw order).
- *Skeletal + mesh deformation:* bones with weighted vertices of the tessellated mesh.
- *Shape morphing:* keyframed path points, interpolated and re-tessellated. Paths must have
  compatible topology; the importer resamples or reports an error if they don't.

### 4.6 Particle system
There is **no dedicated bullet system**. The engine provides a general particle system;
bullets (and any other projectile) are built on top of it by games or a later library.

**Two simulation modes**, chosen per emitter:

| Mode | Runs on | Deterministic | Collision | Use for |
|---|---|---|---|---|
| `gameplay` | CPU (SoA arrays, job system) | Yes | Opt-in | Bullets, pickups, anything that affects game state |
| `visual` | GPU compute on the Full tier; CPU fallback on Compatible / web | No | No | Sparks, smoke, dust, ambient effects |

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
  on the web; visual mode 1M+ on the Full tier.

**Clarified (Phase 4A, see `cuervo/particles/README.md`):**
- *Tick order* of a `ParticleSystem`: simulate every emitter in id order (integrate,
  native modules in stack order, lifetime, `foreach` hook, swap-remove dead), then run
  every emitter's spawners in id order, then rebuild the collision world (particles in
  emitter/index order, then the game's shapes) and resolve hits. Simulation is
  per-emitter independent (the job-system split point); spawning was serial because
  gameplay emitters shared the one simulation RNG (superseded in milestone 5.5: every
  emitter has its own child stream, §4.15).
- *Visual mode* runs on the CPU for now (GPU compute is a Full-tier TODO), uses its own
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
- **Clarified (Phase 4A):** circles and capsules are implemented; AABB/OBB hitboxes come
  later. 32 layers, each proxy on one layer, queries take a layer mask. Touching shapes
  overlap. The hash is a stable counting sort into power-of-two buckets (no unordered
  containers); results are sorted by proxy id (insertion order), and `pairs(A, B)`
  reports a pair whose proxies are both in A and B once, as (lower id, higher id).
  Proxies spanning more than 1024 cells go to a list every query scans (replaced in
  milestone 5.5 by a coarse secondary hash, §4.15).
- General rigid-body physics is a separate, optional module (later).

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
  widgets `Label`, `Button`, `Toggle`, `Slider`. `Image` and `Grid` are deferred.
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
- Scripts ship as **sources** (`scripts = "source"` in the manifest): `pluma/`
  has no bytecode serialization yet, so precompiled bytecode is a 7B follow-up.
  AOT-to-C (7B, 5.12) plugs in as the scripts step with form `native`.
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
  Release player. A player's state hash depends on its features (`ScriptHost::state_hash`
  covers the optional preludes' globals), so one without audio/ui/particles/collision is
  compared with its own dev run; the frames equal the full engine's. AOT packs need the
  full script surface. Web exports build their player the same way with emscripten
  (`CUERVO_WEB_PACK=fetch`, verified in headless Chrome); Windows players are a follow-up
  (10.5a).

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
  (v1→v2 names nodes after their class); `cuervo-tools scene upgrade` rewrites a file.
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
  text back (the authoring path until the editor exists).
- Hot reload (§5.13) does not re-read scene files; instances loaded earlier keep their
  (migrated) state. F6 (restart) picks up edited scenes.

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
  instance's `position` when it is a `Node2D` (nodes draw in world coordinates; there is no
  transform hierarchy). Saving keeps the reference, not a copy; the editor's view expands
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

### 4.14 Threading
- Scripts run on the main thread, in a fixed order.
- A job system runs particle simulation, collision broadphase, animation, tessellation and
  render preparation. Jobs write to disjoint ranges and merge in a fixed order, so
  results stay deterministic.
- On the web the job system runs inline on one thread. WASM threads are an optional later
  addition.

**Clarified (Phase 5, `cuervo/platform/`, docs/WEB.md):** the frame loop is callback-driven
(`platform::FrameLoop` / `run_main_loop`): natively a `while`, on the web
`emscripten_set_main_loop_arg`, so all per-run state lives in a heap object. The web
build has no threads at all (no `-pthread`: no SharedArrayBuffer/COOP/COEP; miniaudio
mixes on the main thread via Web Audio), no file watching (the project is a read-only
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

### 4.16 Polish milestones (8.1-8.4)
The brand, a node library, an editor overhaul and
real documentation, as four milestones in that order (each later one uses the earlier).
- **8.1 Brand integration.** The logo and design style come from the Cuervo design kit at
  `/mnt/storage/CuervoSpine/design/` (its own git repo): the Cuervo Mark and wordmark
  (`logo/*.svg`, `logo/png/`), the palette (Graphite neutrals with "Bone" `#F6F5F2` and
  "Raven" `#111214`, Ember as the single accent, Sage for success), and the type
  (Outfit display, Figtree UI, JetBrains Mono code). `tokens/tokens.css` is the kit's
  source of truth and `tokens.json` is generated from it. Only the visual identity applies;
  the kit's product copy and CuervoOS flows are for another product. Plan:
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
    fallback) there is no PNG loader and no icon. A game-chosen icon is left for later.
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
- **8.2 Node library.** Today the prelude has `Node`, `Node2D`, `SceneInstance`,
  `CanvasLayer` and the UI controls (§4.10); everything else is built in scripts. New nodes
  are prelude classes extending `Node2D` that wrap existing systems, so the editor and
  scene files pick them up through the usual reflection (`@export` fields, §4.12, §4.13).
  Candidates: `Shape` (SDF primitive: circle, rect, rounded rect, capsule, with fill and
  stroke), `Path` (polyline/bezier stroke), `Sprite` (an imported SVG), `Text` (MSDF text
  in the world), `Camera2D`, `Timer`, `Area` (collision shape with layers and signals,
  §4.7), `Emitter2D` (a particle emitter as a node, §4.6) and `AudioPlayer` (§4.9). Each
  one is deterministic in sim paths (§3) and has doctest coverage plus a scene-file
  round-trip test. Animation nodes stay in Phase 8.

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
  - **EULA agreement.** Using the compiled engine requires accepting
    the [EULA](../LICENSE.md) with an explicit check box. On first launch the editor's
    welcome screen shows the agreement (scrollable, with the plain-English summary on top
    and a link to THIRD_PARTY_NOTICES.md) and an unchecked "I have read and agree to the
    Cuervo Engine License Agreement" box; nothing else opens until it is checked and
    confirmed. The command-line programs (`cuervo`, `cuervo-tools`, `plm`) ask once on an
    interactive terminal, and accept `--accept-eula` (or `CUERVO_ACCEPT_EULA=<version>`)
    for scripts and CI. Acceptance is stored per user (under `SDL_GetPrefPath`, §7.2
    "Save data") with the EULA version and date; a new EULA version asks again (EULA
    §19.2). Exported games never ask: their players are run by the game's customers, who
    aren't Engine licensees, so the gate is skipped when running a pack. Declining exits
    without using the Engine. Tests and CI pass `CUERVO_ACCEPT_EULA=<version>` (or `--accept-eula`).

  **Clarified (Milestone 8.3, `editor/`, `cuervo/platform/eula.*`, `cmake/Eula.cmake`):**
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
  - *EULA gate.* `cuervo/platform/eula.*` holds the version and text (generated from
    LICENSE.md by `cmake/Eula.cmake`) and the decision logic (`run_gate`): stored
    acceptance of this version, `--accept-eula` (stores it), `CUERVO_ACCEPT_EULA=<version>`
    (accepts that one run without storing; a wrong value is refused with a message), a
    prompt only when stdin and stderr are terminals (LSP and DAP are never interactive),
    else refused. The file is `eula-accepted.txt` in `platform::user_data_dir("Cuervo
    Engine")` (`CUERVO_USER_DIR` overrides), holding the version and date. `cuervo` and
    `cuervo-tools` call it at start (`cuervo --pack` / a `game.pak` never does, so exported
    games never ask); `plm` has its own copy in `pluma/repl/eula_gate.*` because `pluma/`
    never depends on `cuervo/`, with a test that keeps the two in agreement. The editor
    shows the agreement in its first-launch window (`editor/launcher.*`, `eula_view.*`):
    the summary, the scrollable text, a tab for THIRD_PARTY_NOTICES.md, an unchecked box,
    and Accept/Decline of equal weight; declining exits 1, and screenshots never store
    acceptance. The welcome screen follows when no `--project` was given. Every test that
    runs a program passes `CUERVO_ACCEPT_EULA=<version>` except `eula_gate`, which tests the
    gate itself; the export test strips it for the shipped launchers.
- **8.4 Documentation.** This makes Phase 8's "docs" concrete: a user manual (editor,
  projects, export), a Pluma guide and tutorial built from `docs/LANGUAGE.md`, a node and
  engine API reference generated from the prelude's doc comments (`cuervo-tools doc`,
  reusing the LSP's symbol index, §5), and a "first game" tutorial that is checked by
  building it in CI. The output is a static site styled with the 8.1 brand, and a test
  fails on any public prelude symbol that has no documentation.

### 4.17 Advanced local systems (9.1-9.8)
Phase 9 holds what a Godot comparison found missing (transform hierarchy, more nodes, a
bigger Pluma library, more UI, animation, tilemaps, navigation, scene-tree physics) on top
of the original Box2D and lighting plans. The target games are shmups, platformers,
top-down RPGs, UI-heavy and puzzle games, and general 2D. 3D stays in Phase 13. The
sub-milestones build on each other in order, and each keeps replay hashes identical at 1
and N job threads (§3, §4.15). Nodes already planned in 8.2 (`Shape`, `Path`, `Sprite`,
`Text`, `Camera2D`, `Timer`, `Area`, `Emitter2D`, `AudioPlayer`) are not repeated here.
- **9.1 Transform hierarchy and node gaps.** Today nodes draw in world coordinates
  (scene.plm). `Node2D` gains parent-relative transforms (`global_position`,
  `global_rotation`, `global_scale`, `to_local`, `to_global`). **Decided:** it is the
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
    engine does not install the hook yet. Accessors run outside their own body; method values
    capture the receiver when bound and dispatch virtually. Cut: built-in methods as values,
    `Optional` helpers on reference types (they are already nullable), the `[x; N]` repeat
    literal, new `IndexSet` behavior for custom types, and the engine RNG hookup for `shuffle`.
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
- **9.4 Animation.** `AnimationPlayer` with property tracks (any `@export` field) saved
  in scene files, `Tween`, and an `AnimationTree` state machine with blending. Time is
  measured in ticks (§5.9). Cutout, skeletal and morph animation from Phase 8 plug in here.
- **9.5 Tilemaps and navigation.** `TileMapLayer` with tilesets, autotiling and an editor
  paint tool; `NavigationRegion2D` and `NavigationAgent2D` (grid/mesh A* with
  deterministic tie-breaking).
- **9.6 Physics.** Box2D v3 behind the physics interface (§7.2) as an optional feature
  (§4.11 feature build options). Scene-tree bodies: `StaticBody2D`, `CharacterBody2D`
  (`move_and_slide`), `RigidBody2D`, `Area2D`, `CollisionShape2D`, raycasts and joints.
  Determinism is verified by state hash across worker counts. The particle collision
  world (§4.7) stays separate.
- **9.7 2D lighting.** SDF lighting and raymarched shadows, `PointLight2D`, occluders;
  both backends in parity (§4.4).
- **9.8 Integration.** Editor support for every new node, tool and panel (§4.13), and a
  sample platformer using physics, tilemaps, animation and lighting.

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
- **10.5b Modules and packages.** Today a project's files share one global namespace
  (LANGUAGE.md 9.12). Adds `import`, per-file or per-package namespaces and `pub`
  visibility across them, a `[dependencies]` table in project.toml with pinned versions,
  and packaging so libraries can be shared. Hot reload (§5.13) and the LSP follow imports.
- **10.5c Plugin, embedding and FFI APIs.** A stable, versioned C ABI for native modules
  that register nodes, `@native` functions and importers (the engine's own preludes are
  the model, §5.12), a Pluma FFI to call C libraries with explicit marshaling, and an
  embedding API for hosting the Pluma VM in other programs. Native code in the sim path
  must be deterministic, so the API flags modules as sim-safe or not, and AOT export
  links plugins statically. **Decided:** the first bindings are C++, Rust and C#, all
  over the one C ABI. C++ is a header-only wrapper (the engine's own language). Rust is a
  safe crate generated from the ABI's header. C# runs on .NET hosted in the player and
  editor on desktop; it is not available on the web or in AOT exports until a
  NativeAOT/WASM path is proven, so the web build is C++/Rust/Pluma only at first.
- **10.5d Plugin ecosystem, licenses and contribution.** Builds on 10.5b and 10.5c so
  other people can share and contribute code:
  - *Plugins:* a plugin manifest (name, version, engine and ABI version range, platforms,
    sim-safe flag, license) and a registry index that `cuervo-tools plugin
    install|publish|verify` use. Verification checks the ABI version, the license
    declaration and, for sim-safe plugins, the replay hash on a conformance project.
    Plugins are built from source or shipped as per-platform binaries, with checksums.
  - *Licenses:* **Decided:** the engine is source-available under the Fair-Code EULA in
    `LICENSE.md` (draft, needs attorney review before release), not an OSI license. Free
    under $30,000 Net Revenue per Product; $1,000 per Seat from $30,000; at $200,000 the
    greater of 1.5% of Net Revenue or the Seat Fees; quarterly reports and payment within
    45 days; one audit a year; modifications and forks stay under the license; the
    Product Code is the user's; contributions come with a built-in CLA; non-commercial
    forks are free. The export must not force the game's code under it. The license also
    needs engine support: a `cuervo-tools license report` that totals Net Revenue inputs and
    Seats into the quarterly report. Generated third-party notices
    (SDL3, Vulkan headers, volk, VMA, SPIRV-Cross, libtess2, msdfgen, Dear ImGui,
    miniaudio, FreeType, doctest, and the brand fonts under SIL OFL) bundled into editor
    and game exports, and a license check in CI that fails on a dependency without a
    compatible license. Plugins and packages declare their license in their manifest, and
    the tier check (§4.4) reports any with an incompatible one.
  - *Contribution:* `CONTRIBUTING.md` (build, test, the zero-warning and determinism
    rules, code style, how to add a prelude node or native), a code of conduct, a
    security policy for reports, issue and pull request templates, a contributor license
    or sign-off policy, a design-change process (proposals recorded in DESIGN.md as
    "Decided/Clarified" notes), and a public roadmap generated from the milestones table.

### 4.19 Production features (11.1-11.4)
Phase 11 holds what real shipped games need beyond the engine core, found by comparing
with Godot and Bevy. Each piece keeps replay hashes identical and works on desktop and
the web unless noted. Platform services (Steam, achievements, cloud saves, ads and
purchases) are **out of scope** for now, as is the community and ecosystem work.
- **11.1 Networking.** A transport layer (UDP on desktop, WebSocket and WebRTC on the
  web), then two models on the deterministic sim (§4.1): lockstep (inputs only) and
  rollback (snapshots, re-simulation, input prediction), both reusing the replay frame
  format (§4.8, docs/REPLAYS.md) and the state hash to detect desync. Plus HTTP and
  WebSocket clients for services, outside the sim path. Scripts see networking as
  recorded input and events, never as raw sockets, so the sim stays deterministic.
- **11.2 Shaders and raster pipeline.** User-written shaders in GLSL 450 through the
  existing shader pipeline (§4.4), tier-checked by the migration report; post-process
  passes; sprite sheets and texture atlases (import-time packing, batched draws);
  9-slice; raster images beside the vector path. Shader parameters are `@export`-style
  properties in the inspector.
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
  shader editor producing 11.2 shaders.
- **12.2 AI.** Behavior trees and a state-machine tool (nodes in the scene tree, edited
  and debugged in the editor), and steering behaviors (seek, flee, arrive, avoid,
  flocking) that use 9.5 navigation. All deterministic and tick-driven.
- **12.3 Accessibility.** Screen-reader support through the platform accessibility APIs
  (desktop first, web through ARIA), UI scaling, runtime input remapping (building on
  the input map editor, §4.13), colorblind palettes, reduced-motion and subtitle
  options.
- **12.4 ECS decision.** §4.3 says the runtime is an ECS but scripts only see nodes. This
  milestone decides: either expose component queries and systems to Pluma (iterated in
  the job system with a deterministic order) or drop the ECS from the design and say so.
  The decision is recorded here before any work starts.

## 5. The language

### 5.1 Overview
Statically typed with inference, brace syntax, embedded in the engine. Compiles to bytecode
for development (fast iteration, hot reload) and to C for release builds (native speed,
no JIT, which also works on the web).

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
  (decided, 7.1). The typed IR (`pluma/tir.hpp`) is the input the AOT-to-C path will share.
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

### 5.14 Tooling (later phases)
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

| # | Phase | Done when |
|---|---|---|
| 0 | **Skeleton:** CMake, SDL3 window, fixed-timestep loop, input actions, logging, `Vec<T,N>`, RHI interface + GL and Vulkan backends (Vulkan pulled forward from Phase 5) | A window clears to a color and responds to actions, on Linux and Windows |
| 1 | **Language core** (in parallel): lexer, parser, type checker, bytecode, VM, refcounting, REPL | Closures, structs, classes and enums run in the REPL; language test suite passes |
| 2 | **Vector rendering:** SDF batcher, SVG import + tessellation, MSDF text, camera, scale modes | 50k SDF shapes at 60 fps; an SVG character drawn sharp at any zoom |
| 3 | **Integration:** bindings, scripts on nodes, coroutines, hot reload | **A shmup stage written entirely in the language** (done: `cuervo --game examples/shmup`) |
| 4 | **Gameplay systems:** particle system (gameplay + visual), collision, audio, UI nodes, binary scenes + dump | A playable boss fight with menus |
| 5 | **Web + tiers:** WebGL2 export, Vulkan feature parity, tiers + migration report | The same project runs on Linux, Windows and the web (Linux + web done and verified, docs/WEB.md; Windows validation is in progress on Intel Arc integrated graphics and an RTX 4070 on the Core Ultra 9 185H laptop, docs/WINDOWS.md) |
| 5.5 | **Performance core** (§4.15): activate the job system, explicit SIMD (AVX2 / WASM SIMD128), sub-seeded RNG streams, secondary spatial hash for large shapes | The particle demo's sim time scales with worker count, SIMD and scalar paths are bit-identical, and the replay hash is unchanged at 1 and N threads (done: §4.15 "Verified") |
| 6 | **Editor:** scene view, inspector, assets, play-in-editor, replay viewer | A level can be built without code (done: 6A, the editor shell, §4.13 "Clarified (Phase 6A)" -- `examples/editor_level` was built from editor operations only and runs with `cuervo --game`; 6B, the asset browser, input-map editor, play-in-editor with hot reload, replays and the debugger, and the tier report, §4.13 "Clarified (Phase 6B)") |
| 7 | **Release path:** AOT-to-C, cycle collector, manual memory, packing, export | A shipped build runs measurably faster than the VM (done: 7A cycle collector + manual memory, §5.10 "Clarified (Phase 7A)"; 7B AOT-to-C compilation, §5.12 "Clarified (Phase 7B)"; 7C packing + export, §4.11 "Clarified (Phase 7C)", docs/EXPORT.md. Both source and AOT native exports of `examples/shmup` reach the dev state hash `a3e57e79964421d3`, and AOT runs measurably faster than the bytecode VM) |
| 7.5 | **Modular footprint:** Feature-stripping build system, dead-code elimination, unreferenced module pruning | An exported project strips unused engine subsystems (e.g., audio, unused RHI features, UI) resulting in measurably smaller release binaries and web payloads (done: §4.11 "Clarified (Milestone 7.5)", docs/EXPORT.md "Stripped players". Per-export Release players of `examples/tictak`, `examples/bench` and `examples/shmup` are 40%, 37% and 20% smaller than the full Release player on Linux, and their web `index.wasm` 18%, 15% and 4% smaller than the full web runtime; each runs its exported pack to its dev run's state hash, the web ones in headless Chrome. Windows players remain part of the active 10.5a validation) |
| 8 | **Tooling:** formatter, LSP, debugger, animation (cutout, skeletal, morph), docs (expanded as 8.4) | Someone else makes a game with it (tooling done, animation deferred to 9.4: formatter and LSP, debugger over DAP, and profiler, §5.14 "Clarified (Phase 8 — ...)"; the VS Code extension `tools/vscode-pluma`; docs/GUIDE.md with the `examples/tutorial` game and the generated docs/API.md, expanded further by 8.4. The done-when needs an outside user to confirm) |
| 8.1 | **Brand integration** (§4.16): Cuervo Mark, wordmark, palette and type from the design kit, synced into `assets/brand/`, plus generated color tokens | The editor and runtime show the mark as window icon and splash in light and dark themes, and a sync script regenerates the brand assets from the kit (done: §4.16 "Clarified (Milestone 8.1)". `tools/sync_brand.py` syncs `assets/brand/` and records the kit commit; `cuervo` and `cuervo-editor` set the embedded app icon as their window icon; the editor's splash draws the imported mark and wordmark in the Bone and Raven themes on Vulkan and GL, and its UI switches between the two token-generated themes) |
| 8.2 | **Node library** (§4.16): Shape, Path, Sprite, Text, Camera2D, Timer, Area, Emitter2D, AudioPlayer as prelude nodes | A level using every new node is built in the editor without code and round-trips through scene files with the same state hash (done: §4.16 "Clarified (Milestone 8.2)"; `examples/node_level` from `level.cmds`, the `node_level` test: editor-built, dump/build round trip and editor-played runs all reach `115a6d76bb125635` at tick 240) |
| 8.3 | **Editor UI overhaul** (§4.16): themed with the brand tokens, gizmos, toolbar, command palette, multi-select, undo history, welcome screen, EULA agreement check box | The `examples/editor_level` workflow is redone with gizmos and the palette, with no raw ImGui-default styling left, and a fresh user account must check the EULA box before the editor or tools run (exported games never ask) (done: §4.16 "Clarified (Milestone 8.3)". The editor moved to ImGui's docking branch with a DockBuilder default layout and saved dock layouts; `editor_ui_tests` fails on any ImGui-default color; `examples/editor_level/level.cmds` uses the palette and gizmo verbs and still produces the same scene; the editor, `cuervo`, `cuervo-tools` and `plm` refuse to start until the EULA is accepted, and packs never ask) |
| 8.4 | **Documentation** (§4.16): manual, Pluma guide, generated API/node reference, first-game tutorial, branded static site | Every public prelude symbol is documented (enforced by a test) and the tutorial game builds in CI |
| 8.5 | **Documentation reconciliation** (§4.16): align design, user and API documentation with the actual EULA, current milestone state, implemented systems, and support status; separate shipped, experimental, planned and unsupported behavior | A fresh reader can determine what is implemented and supported without relying on historical milestone notes; no active document claims that the license is undecided or that completed systems are still only planned |
| 8.6 | **Performance baselines** (§4.15): publish repeatable simulation, render, memory and export-size measurements for representative particle/shape loads, native backends, WebGL2, debug and release builds | Benchmark commands record versioned baselines and flag regressions when agreed thresholds are exceeded; the 1k/10k/50k/100k workload results are reproducible on the reference machine |
| 8.7 | **Stabilization pass** (§4.12, §4.13, §5): prioritize Pluma semantics, scene serialization and migrations, editor workflows, replay behavior and export reliability before another major subsystem | A clean checkout can build the reference examples, round-trip scenes, hot-reload scripts, verify replays and launch source, AOT and stripped exports without known correctness regressions |
| 8.8 | **Supported-features contract** (§4.4, §4.16): classify features as stable, experimental, desktop-only, Web-compatible or unsupported; document the guarantees and limits of `compatible` and `full` render tiers | The README and generated/user documentation expose the same support matrix, and tier checks reject configurations that claim support the selected target does not provide |
| 8.9 | **End-to-end user workflow** (§4.11, §4.13): automate the fresh-checkout path from project creation through scene editing, running, exporting and launching the exported game | A workflow smoke test creates or opens a project, edits and saves a scene, runs it, exports it, launches the shipped artifact outside the project directory and verifies its exit status and deterministic state hash |
| 9 | **Advanced Local Systems** (§4.17): transform hierarchy and more nodes, Pluma library, UI widgets, animation, tilemaps and navigation, Box2D v3 physics, 2D SDF lighting/raymarching | A physics-based platformer with 2D lighting is playable |
| 9.1 | **Transform hierarchy and node gaps** (§4.17): parent-relative transforms (toggleable), `Line2D`, `Polygon2D`, `Marker2D`, `VisibleOnScreenNotifier2D` | Moving a parent moves its children when the hierarchy is on, and world-space scenes behave exactly as before with it off (done: §4.17 "Clarified (Milestone 9.1)"; `hierarchy_tests` move a parent and check the child with the hierarchy on and off, and the shmup, tictak and editor_level hashes are unchanged with it off) |
| 9.2a | **Pluma collections, strings and language** (§4.17): `Map`/`Set`/fixed arrays/`StrBuilder`, `List` and `str` methods, conversions, property accessors, `Result` and `?`, method values | A script uses each feature and the language test suite passes under the VM and AOT (done: §4.17 "Clarified (Milestone 9.2a)"; `tests/pluma/library_tests.cpp` runs the same program in the VM and AOT) |
| 9.2b | **Pluma math, geometry and random** (§4.17): scalar/integer/vector math, `Rect2`, `Transform2D`, geometry helpers, noise, seeded `Random` streams | Every function is bit-identical across SIMD, scalar, VM and AOT, and the replay hash is unchanged (done: §4.17 "Clarified (Milestone 9.2b)") |
| 9.3 | **UI widgets** (§4.17): `GridContainer`, `ScrollContainer`, Flow/Split/AspectRatio, `Image`/`TextureRect`, `ProgressBar`, `LineEdit`, `OptionButton`, tabs, popups | A settings and inventory menu built from them is driven by keyboard, gamepad and pointer, and replays |
| 9.4 | **Animation** (§4.17): `AnimationPlayer` (property tracks), `Tween`, `AnimationTree` state machine | An animated character blends between states and the replay hash is unchanged at 1 and N threads |
| 9.5 | **Tilemaps and navigation** (§4.17): `TileMapLayer`, tilesets, autotiling, `NavigationRegion2D`, `NavigationAgent2D` | A top-down level is painted in the editor and an agent paths around obstacles |
| 9.6 | **Physics** (§4.17): Box2D v3 behind the physics interface; `StaticBody2D`, `CharacterBody2D`, `RigidBody2D`, `Area2D`, `CollisionShape2D`, raycasts, joints | A platformer character runs, jumps and pushes bodies deterministically |
| 9.7 | **2D lighting** (§4.17): SDF lighting/raymarching, `PointLight2D`, occluders | Lights and shadows render identically on Vulkan and GL |
| 9.8 | **Integration** (§4.17): editor support for all of the above and a sample game | A physics-based platformer with 2D lighting is playable |
| 10 | **Next-Gen Web:** WebGPU, WASM multithreading (SharedArrayBuffer)| A stable benchtest runs natively in the browser (WASM threads done early: docs/WEB.md "Threads", `CUERVO_WEB_THREADS`. **Design decision:** the WebGPU backend's shaders are translated SPIR-V → WGSL by **naga** (Rust; a host build tool), keeping one GLSL 450 source per shader; WebGPU starts only after Phases 8 and 9 are done) |
| 10.5 | **Windows and language APIs** (§4.18): Windows builds, module/package system, plugin and embedding API, FFI | The shmup builds, passes its tests and exports on Windows with the same state hash as Linux, and a third-party plugin and package are used by a game |
| 10.5a | **Windows builds** (§4.18): MSVC and MinGW builds, CI, determinism check, signed prebuilt editor and export templates | `ctest` passes on Windows and a Windows export of `examples/shmup` reaches the Linux state hash |
| 10.5b | **Modules and packages** (§4.18): `import`, namespaces, project dependencies, versioning | A game imports a package from another directory with no global name clashes |
| 10.5c | **Plugin, embedding and FFI APIs** (§4.18): stable C ABI for native modules, C++, Rust and C# bindings, Pluma FFI to C libraries, API versioning | A native plugin written in each of C++, Rust and C# adds a node and a function that a script uses (C# on desktop), in the VM and in AOT where supported |
| 10.5d | **Plugin ecosystem, licenses and contribution** (§4.18): plugin manifest and registry, project license, third-party notices, contribution guides | The repo has a license, generated third-party notices and contribution docs, and a plugin published to the registry installs into a fresh project and passes the checks |
| 11 | **Production features** (§4.19): rollback/lockstep networking, custom shaders and a raster sprite pipeline, localization and text shaping, profiler and save/settings API | A networked two-player game with a localized UI, a custom post-process shader and an atlas-based sprite scene is shipped and profiled |
| 11.1 | **Networking** (§4.19): transport, lockstep and rollback on the deterministic sim, HTTP and WebSocket | Two clients on different machines (native and web) stay in sync, with the same state hash every tick |
| 11.2 | **Shaders and raster pipeline** (§4.19): user shaders, post-processing, sprite sheets, texture atlases, 9-slice | A custom shader and an atlas-batched sprite scene render identically on Vulkan and GL |
| 11.3 | **Localization and text** (§4.19): translation tables, complex-script shaping, bidirectional text, IME | A UI switches between English, Arabic and Japanese at run time and `LineEdit` accepts IME input |
| 11.4 | **Profiling and save API** (§4.19): frame/job/GPU profiler, memory profiler, general save/load and settings API | A profile of the shmup shows per-system tick cost, and a game saves and restores progress and settings |
| 12 | **Content tools, AI and accessibility** (§4.20): dialogue and cutscene tools, particle and shader editors, behavior trees and steering, accessibility, the ECS decision | A narrative game with AI enemies and a screen-reader-usable menu is built mostly in the editor |
| 12.1 | **Content tools** (§4.20): dialogue and cutscene/timeline editor, particle editor UI, visual shader editor | A cutscene with dialogue and a particle effect is authored without code |
| 12.2 | **AI** (§4.20): behavior trees, state-machine tool, steering behaviors | An enemy with a behavior tree is built and debugged in the editor |
| 12.3 | **Accessibility** (§4.20): screen-reader support, UI scaling, remapping, colorblind and reduced-motion options | The shmup menus can be used with a screen reader and at 200% UI scale |
| 12.4 | **ECS decision** (§4.20): expose the ECS to scripts as queries and systems, or remove it from the design | The decision is documented and, if exposed, a script system iterates a component query in the job system |
| 13 | **3D Expansion:** 3D rendering pipeline, 3D math, 3D scene/camera transforms, glTF import | A full 3D scene runs with PBR materials and 3D transforms |
| 14 | **Extended Platforms:** macOS, iOS, Android (Metal, JNI), drop-in Console SDK architecture | The shmup runs natively on a Mac, iPhone, and Android device |
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
| Color | Colors are authored in sRGB and blended in sRGB space, matching browsers and vector tools, so SVGs look as they do in Inkscape. Swapchain/framebuffer stays UNORM |
| Text | UTF-8; FreeType + HarfBuzz for shaping, then MSDF atlases; right-to-left and CJK shaping supported; bidi layout later |
| Localization | String tables (CSV) keyed by `#name`, with `tr(#key)` in scripts; switchable at runtime |
| General physics | Box2D v3 behind the physics interface, as an optional module (Phase 4+) |
| Audio formats | WAV, OGG Vorbis, FLAC, MP3 (miniaudio's decoders) |
| Fonts | TTF/OTF imported into MSDF atlases |
| Project layout | `project.toml`, `assets/`, `scenes/`, `scripts/`, `.import/` (generated, gitignored) |
| Save data | Per-user directory from `SDL_GetPrefPath`; a `Save` API with versioned data |
| Window | VSync on; fullscreen/windowed/borderless toggle; 60 Hz tick (configurable) |
| Profiling | Tracy integration, compiled out of release builds |
| AOT export | Uses the system C compiler (Clang/GCC/MSVC); web export uses Emscripten |
| Editor layout | Godot-like: scene tree left, viewport center, inspector right, assets + console bottom |
| Accessibility | Rebindable input, UI scale setting, a reduced-flashing toggle exposed to games |
| Platforms | Linux, Windows, Web only: no macOS, mobile or consoles planned |
| Engine versioning | SemVer; scene/project migrations keyed by format version |

### 7.3 Still open (not blocking)
- License (all dependencies are permissive, so any choice is open).

### 7.4 Validation environments
- **Primary Arch development machine:** AMD Ryzen 7 5800X3D with an NVIDIA RTX 3060 Ti.
  This is the reference environment for day-to-day engine, renderer and web development.
- **Windows validation machine:** Intel Core Ultra 9 185H laptop with Intel Arc integrated
  graphics and an NVIDIA RTX 4070 discrete GPU. Windows builds and runtime behavior are
  being tested actively on both GPU paths where selectable; Windows support is not marked
  complete until the required compiler, test, determinism and export checks in milestone
  10.5a pass.

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
