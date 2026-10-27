# Support status

What works today, what is experimental, what is planned and what is not supported. Cuervo
Engine is in **Beta (v0.2)**. This page is the one place that states support; the other
documents describe how things work. [DESIGN.md](DESIGN.md) holds the architecture, the
decisions and the full milestone list (section 6).

The tables below are generated from `docs/support.json`, the single source of truth
that the tier check and the exporter also read (`python3 -I tools/support_matrix.py --write`
regenerates them; the `support_matrix_up_to_date` test fails when they are stale).

<!-- support-matrix:begin legend -->
| Level | Meaning |
|---|---|
| **Stable** | Shipped and covered by the test suite (`ctest`) or a verified end-to-end run. Changes that break it are bugs. |
| **Experimental** | Shipped and usable, but with known gaps, limited verification, or an interface that may still change. |
| **Planned** | Designed, with a milestone in DESIGN.md section 6, but not in the tree. Do not build on it. |
| **Unsupported** | Not provided and not on the near-term roadmap. |

Each row also says **where** the feature works and which render **tier** it needs. *Desktop only* means Linux and Windows but not the web; a row that lists Web is web-compatible. `-` in Tier means the feature does not depend on the render tier. `cuervo-tools tier check --platform <linux|windows|web>` and `cuervo-tools export` refuse a project that claims more than a row here provides for the target.
<!-- support-matrix:end legend -->

The engine is licensed under the [MIT License](../LICENSE.md).

## Platforms

<!-- support-matrix:begin platforms -->
| Feature | Level | Where | Tier | Notes |
|---|---|---|---|---|
| Linux x86-64, Vulkan 1.3 | Stable | Linux (desktop only) | `compatible` | Development and verification platform (Arch Linux; NVIDIA RTX 3060 Ti). Serves both render tiers. |
| Linux x86-64, OpenGL 3.3 core | Stable | Linux (desktop only) | `compatible` | The fallback when there is no Vulkan driver; renders the same scenes as Vulkan (the `rhi_parity` test). Compatible tier only. |
| Linux, OpenGL ES 3.0 (`--gl-es`) | Stable | Linux (desktop only) | `compatible` | The web's shader path, testable on the desktop. |
| Web (WebGPU or WebGL2 / WASM), Chrome | Stable | Web | `compatible` | Single-threaded by default; WebGPU when the browser has it, else WebGL2 (one build). The shmup reaches the same state hash as the native build on both. See [WEB.md](WEB.md). Compatible tier only. |
| Web, WASM threads | Experimental | Web | `compatible` | Opt-in (`CUERVO_WEB_THREADS`, presets `web-threads` and `web-pack-threads`); the page must be cross-origin isolated. |
| Web, Firefox and Safari | Experimental | Web | `compatible` | Untested; any WebGL2 browser should work. |
| Windows 10/11 x86-64 | Stable | Windows (desktop only) | - | Milestone 10.5a. MSVC, MinGW-w64 GCC and Clang builds have zero warnings and pass the test suite (CI: MSVC Release, MinGW-w64 Debug), and the shmup reaches the Linux state hash on GL and Vulkan. UTF-8 code page and console, per-monitor DPI awareness, a GUI-subsystem player. See [WINDOWS.md](WINDOWS.md). |
| macOS, iOS, Android, consoles | Unsupported | - | - | Only the long-term Phase 14 mentions them. |
<!-- support-matrix:end platforms -->

## Rendering

<!-- support-matrix:begin rendering -->
| Feature | Level | Where | Tier | Notes |
|---|---|---|---|---|
| Vulkan backend | Stable | Linux, Windows (desktop only) | `compatible` | Desktop only; supports both render tiers. |
| GL backend (GL 3.3, GLES 3.0, WebGL2) | Stable | Linux, Windows, Web | `compatible` | Every RHI feature is implemented in both backends in the same change. |
| `compatible` render tier | Stable | Linux, Windows, Web | `compatible` | Promises what WebGL2 offers: 4096 px textures, 4x MSAA, 16 KB uniform ranges. Exports to every target. See Render tiers below. |
| `full` render tier | Experimental | Linux, Windows (desktop only) | `full` | Vulkan on the desktop only. The only Full-only feature today is base-vertex indexed draws; compute, storage buffers and bindless are planned. A Full project on GL or the web is refused. See Render tiers below. |
| Tier migration report (`cuervo-tools tier check`, editor Tier Report) | Stable | Linux, Windows (desktop only) | - | Scans project settings, shaders, scripts, scenes and assets, and checks the claimed tier and target platform against this page. |
| SDF shapes, MSDF text, SVG import (practical subset) | Stable | Linux, Windows, Web | `compatible` | Unsupported SVG features (filters, masks, CSS, embedded text) are skipped with a warning. |
| GPU compute visual particles | Planned | - | `full` | Visual particles run on the CPU on every tier today. |
| Raster processes: images, sprite sheets, atlases, 9-slice, SVG fallback | Planned | - | - | Milestone 11.2a. Art is vector first. |
| Custom user shaders, post-processing | Planned | - | - | Milestone 11.2b. |
| SVG rasterized fallback, text shaping (right-to-left, complex scripts) | Planned | - | - | Milestones 11.2a and 11.3. |
| 2D lighting (`PointLight2D`, `DirectionalLight2D`, `AmbientLight2D`, `LightOccluder2D`) | Stable | Linux, Windows, Web | `compatible` | Milestone 9.7. SDF occluders with raymarched hard and soft shadows, identical on Vulkan and GL; render only (never changes a state hash). Up to 32 lights and 128 occluders per view; see [API.md](API.md). |
| WebGPU | Experimental | Linux, Web | `compatible` | Milestones 10.1-10.3: the web default where the browser has it (WebGL2 otherwise, in the same build and in web exports), `--backend webgpu` on Linux (wgpu-native, `CUERVO_WEBGPU`). Every Compatible RHI feature; matches GL in every `rhi_parity` scene and Vulkan exactly, all WGSL generated from GLSL; the shmup and `lantern_cave` reach their native state hashes on WebGPU in headless Chrome, and the browser benchtest (100k shapes, 8 threads) runs stably at 390 FPS (milestone 10.4, [PERFORMANCE.md](PERFORMANCE.md) "Web"). Not in desktop exports. See [WEB.md](WEB.md) "WebGPU". |
<!-- support-matrix:end rendering -->

## Render tiers

A project's `render_tier` in `project.toml` (default `compatible`) is a contract about where
it runs (DESIGN.md 4.4). The tier check (`cuervo-tools tier check <project> [--to TIER]
[--platform linux|windows|web]`), `cuervo-tools export` and the `cuervo` runtime enforce it:

- `export --platform web` refuses a `full` project, and refuses any feature whose row above
  does not list the target platform.
- `cuervo` refuses a `full` project on a device that cannot serve Full (the OpenGL backend,
  or a Vulkan device missing a required Full feature) instead of running it degraded.
- `tier check` reports shaders, scripts, scenes and assets the tier cannot carry, with
  file and line.

<!-- support-matrix:begin tiers -->
### The `compatible` tier

The portable tier and the default. A project that stays inside it runs unchanged on Vulkan, OpenGL 3.3, OpenGL ES 3.0 and WebGL2.

- **Backends:** Vulkan, OpenGL 3.3, OpenGL ES 3.0, WebGL2.
- **Platforms:** Linux, Windows, Web.
- **Limits:** textures up to 4096 px, 4x MSAA, 16 KB per uniform range, 16 vertex attributes, 256-byte uniform offset alignment (WebGL2 reality, not the GLES 3.0 spec minimums).
- **Guarantee:** Every Compatible RHI feature in the table below is available on every backend and platform that serves the tier.
- **Guarantee:** The device is clamped to the limits above even when the hardware offers more, so a game tested on Vulkan cannot use what WebGL2 lacks.
- **Guarantee:** The optional features (MSAA, frame capture) degrade instead of breaking a project when a device lacks them.
- **Refused:** Custom shader constructs GLES 3.0 cannot express (storage buffers, image load/store, atomics, subgroups, bindless, 64-bit floats, ...): a tier check error.
- **Refused:** Compute, geometry, tessellation, mesh and task shader stages: a tier check error.
- **Refused:** Textures larger than the limit above (split them into an atlas): a tier check error.
- **Refused:** Full-only RHI features: the gated device logs once and ignores the call.

### The `full` tier

The desktop tier. Everything Compatible has, plus features only Vulkan provides. It is the migration target when a game outgrows WebGL2.

- **Backends:** Vulkan.
- **Platforms:** Linux, Windows.
- **Guarantee:** Runs on the Vulkan backend on Linux and Windows only. Full has no fixed numeric minimums yet.
- **Guarantee:** The Full RHI features in the table below are required: a Vulkan device missing one is refused.
- **Refused:** The OpenGL backend: a Full project on GL, or on any device that cannot serve Full, is refused at startup with an error naming --backend vulkan.
- **Refused:** The web: a Full project cannot be exported with --platform web (tier check and export both refuse it).
- **Refused:** Exports for a platform this page does not list for the tier.

### RHI features by tier

Generated from `docs/support.json`; the `support_tiers` test checks it against the feature table in `cuervo/rhi/rhi.cpp`.

| RHI feature | Tier | Required | Notes |
|---|---|---|---|
| instanced draws | `compatible` | yes | draw and draw_indexed with instance_count and first_instance |
| 32-bit indices | `compatible` | yes | IndexFormat::UInt32 |
| uniform buffers | `compatible` | yes | bound with offsets |
| textures | `compatible` | yes | RGBA8 and R8 2D textures, nearest and linear |
| integer vertex attributes | `compatible` | yes | VertexFormat::UInt1 |
| points and lines | `compatible` | yes | point lists, line lists and line strips, 1 px wide |
| MSAA | `compatible` | optional | falls back to 1 sample |
| frame capture | `compatible` | optional | a debug aid |
| base-vertex draws | `full` | yes | draw_indexed_base_vertex; GLES 3.0 has no base vertex |
<!-- support-matrix:end tiers -->

## Simulation and gameplay systems

<!-- support-matrix:begin simulation -->
| Feature | Level | Where | Tier | Notes |
|---|---|---|---|---|
| Fixed timestep, seeded PCG32 RNG, deterministic math (`dmath`) | Stable | Linux, Windows, Web | - | Bit-identical on the same build and platform. Native Linux and the web build agree on the shmup state hash. |
| Replays (record, verify, seek) | Stable | Linux, Windows, Web | - | [REPLAYS.md](REPLAYS.md). |
| Job system and SIMD kernels | Stable | Linux, Windows, Web | - | The same state hash for any thread count and with SIMD on or off. |
| Particles (gameplay and visual modes) | Stable | Linux, Windows, Web | - | Gameplay mode is deterministic and collidable; see `cuervo/particles/README.md`. |
| Collision (circles, capsules, spatial hash) | Stable | Linux, Windows, Web | - | AABB and OBB shapes are planned. |
| Audio (miniaudio decoding, own mixer, buses, voice limiting) | Stable | Linux, Windows, Web | - | Per-bus effects and positional audio are planned. |
| Node tree, scene files (versioned binary, text dump) | Stable | Linux, Windows, Web | - | [SCENES.md](SCENES.md). |
| Node library (shapes, paths, sprites, text, camera, timer, area, emitter, audio player, lines, polygons) | Stable | Linux, Windows, Web | - | See [API.md](API.md). |
| Transform hierarchy | Stable | Linux, Windows, Web | - | On by default; `[scene] transform_hierarchy = false` keeps world-space behavior. |
| UI controls and widgets (containers, scroll, tabs, popups, `LineEdit`, ...) | Stable | Linux, Windows, Web | - | Keyboard, gamepad and pointer. No on-screen keyboard and no IME composition yet. |
| Animation: `AnimationPlayer` (property, method and event tracks), `Tween`, `AnimationTree` state machine with cross-fades | Stable | Linux, Windows, Web | - | Time is in ticks and animations are saved in scene files; see [GUIDE.md](GUIDE.md) and [SCENES.md](SCENES.md). Replays include the values animation writes. |
| Cutout, skeletal and morph animation, Bezier key handles, blend spaces, layered blending | Planned | - | - | Not implemented. The track system is designed to carry cutout, skeletal and morph tracks (DESIGN.md 4.17). |
| Tilemaps (`TileMapLayer`, autotiling, paint tool) and navigation (`NavigationRegion2D`, `NavigationAgent2D`) | Stable | Linux, Windows, Web | - | Milestone 9.5. Deterministic A* with strict tie-breaking; see [API.md](API.md). |
| Physics (Box2D v3 scene-tree bodies) | Stable | Linux, Windows, Web | - | Milestone 9.6. Box2D v3 scene-tree bodies (StaticBody2D, CharacterBody2D, RigidBody2D, Area2D, CollisionShape2D), raycasts, joints; see [API.md](API.md). |
| General ECS exposed to scripts | Planned | - | - | Milestone 12.4 decides whether to expose it or drop it. |
| Networking (lockstep, rollback), HTTP and WebSocket | Planned | - | - | Milestone 11.1. |
| Localization, save and settings API, accessibility features | Planned | - | - | Milestones 11.3, 11.4 and 12.3. Today games save through `Scene.save`. |
| 3D | Unsupported | - | - | Phase 13, long term. |
<!-- support-matrix:end simulation -->

## The Pluma language and its tools

<!-- support-matrix:begin language -->
| Feature | Level | Where | Tier | Notes |
|---|---|---|---|---|
| Bytecode VM, type checker, coroutines, signals, hot reload | Stable | Linux, Windows, Web | - | [LANGUAGE.md](LANGUAGE.md), [PLUMA_GUIDE.md](PLUMA_GUIDE.md). |
| Collections, strings, `Result` and `?`, math, geometry, noise, seeded `Rng` | Stable | Linux, Windows, Web | - | The same results in the VM and in AOT. |
| Memory model (refcounting, cycle collector, opt-in manual memory) | Stable | Linux, Windows, Web | - |  |
| AOT-to-C compilation | Stable | Linux, Windows (desktop only) | - | Host platform only: the export host must be the target (Linux or Windows). Needs a C compiler. On Windows the C compiler is LLVM clang with every toolchain (docs/WINDOWS.md). Not used for web exports (web games ship scripts as source); an AOT export for another platform is refused. |
| Formatter, language server, VS Code extension | Stable | Linux, Windows (desktop only) | - | `cuervo-tools fmt`, `cuervo-tools lsp`, [the extension](../tools/vscode-pluma/README.md). Host tools. |
| Debugger (DAP) and profiler | Stable | Linux, Windows (desktop only) | - | On the VM only; not available for AOT-native exports or the web. |
| Modules, `import` and packages | Stable | Linux, Windows, Web | - | Milestone 10.5b: `import`, `pub` across packages, `[dependencies]` with pinned versions (vendored or by path), packed into exports; see [PACKAGES.md](PACKAGES.md). `examples/garden` reaches its dev state hash from source and AOT packs on Linux, on Windows and in headless Chrome. Packages are shared through registries (10.5d). |
| Unit tests (`@test`) | Stable | Linux, Windows (desktop only) | - | Milestone 10.5b: `plm test` and `cuervo-tools test` run a game's or a package's `@test` functions, each in a fresh session; left out of builds. See [LANGUAGE.md](LANGUAGE.md) 9.27. Host tools. |
| Plugin API, FFI, embedding | Stable | Linux, Windows (desktop only) | - | Milestone 10.5c: native plugins over one C ABI (version 1) in C++, Rust and C# (.NET, desktop only), with nodes, natives and importers; C++ and Rust plugins link statically into AOT exports. `@extern` calls C functions ([LANGUAGE.md](LANGUAGE.md) 9.28); `pluma/embed/pluma_embed.h` hosts the VM from C. See [PLUGINS.md](PLUGINS.md). Not on the web yet. |
<!-- support-matrix:end language -->

## Editor, tools and export

<!-- support-matrix:begin tools -->
| Feature | Level | Where | Tier | Notes |
|---|---|---|---|---|
| Editor (`cuervo-editor`) | Stable | Linux, Windows (desktop only) | - | Scene tree, inspector, gizmos, docking layouts, play-in-editor, replays, debugger, profiler. Desktop only; there is no web editor. See [editor/README.md](../editor/README.md). |
| Export for Linux (source and AOT scripts, stripped players) | Stable | Linux (desktop only) | - | Shared libraries (SDL3, and FreeType for full players) are not bundled. [EXPORT.md](EXPORT.md). |
| Export for the web | Stable | Web | `compatible` | Needs an emsdk (6.0.12) and a host-tools build. Compatible tier only; a Full project is refused. |
| Export for Windows (source and AOT scripts, stripped players) | Stable | Windows (desktop only) | - | Ships the GUI-subsystem `cuervo-player.exe` of the build tree or a prebuilt package's export template. The shmup and the tutorial exported with source and AOT scripts reach their dev state hashes from another directory, with MSVC and MinGW-w64 players. Stripped players build on a Windows host (tictak's: -31 %, same hash). [EXPORT.md](EXPORT.md). |
| Pack format (`game.pak`) | Stable | Linux, Windows, Web | - | Deterministic, integrity-checked, uncompressed (the web server compresses). |
| Documentation site (`docs_site` target) | Stable | Linux, Windows (desktop only) | - | Built from these Markdown files; every link is checked, and the support tables are checked against `docs/support.json`. |
| Dev-only features (`--demo`, `--svg`, `--bench`) | Stable | Linux, Windows (desktop only) | - | Not in stripped players. |
| Package registry and `license report` | Stable | Linux, Windows (desktop only) | - | Milestone 10.5d: `cuervo-tools plugin verify\|publish\|install\|list` over a directory registry with SHA-256 checksums, SPDX license checks, engine and ABI version ranges and a conformance state hash for sim-safe plugins; `license report` and exports list the packages' licenses. See [PLUGINS.md](PLUGINS.md). HTTP registries are not built yet. |
| Signed prebuilt editor and export templates | Experimental | Linux, Windows (desktop only) | - | Milestone 10.5a. The release workflow builds the editor, tools and players on a `v*` tag, with export templates in `templates/<platform>/`, a SHA-256 sum for every file (and a Sigstore attestation once the repository is public), and Authenticode signatures on the Windows executables when the repository has a signing certificate (docs/WINDOWS.md "Prebuilt binaries"). No release has been tagged yet. |
| Content tools (dialogue, timeline, particle and shader editors), AI tools | Planned | - | - | Milestones 12.1 and 12.2. |
<!-- support-matrix:end tools -->

## Where the project is

Milestones 0 to 8.9 and 9.1 to 9.8 are implemented (DESIGN.md section 6 marks each "done").
The supported-features contract (8.8) is this page: the classification lives in `docs/support.json`
and the tier checks enforce it. The remaining milestones are in DESIGN.md section 6.
