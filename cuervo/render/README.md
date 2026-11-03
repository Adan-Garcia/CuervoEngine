# cuervo/render

Backend-independent rendering on top of the RHI (DESIGN.md 4.4).

## Conventions (user decision, DESIGN.md 7.1)
- World and virtual coordinates are **y-down**, origin top-left, units are
  pixels of the virtual coordinate space.
- Angles are radians; a positive rotation turns **clockwise** on screen.

## Camera2D and scale modes (`camera.hpp`)
- `Camera2D` maps world to virtual coordinates (position = world point at the
  view center, zoom > 1 magnifies, rotation in radians).
- `fit_viewport(mode, virtual_size, framebuffer_size)` places the virtual space
  in the framebuffer:
  - `Fit` (default): uniform scale, letterboxed. Rendering happens at the
    framebuffer's native resolution with logical (virtual) coordinates, so
    vectors stay sharp at any window size.
  - `Integer`: largest whole-number scale (at least 1x, cropped if the window is
    smaller), for pixel art.
  - `Stretch`: fills the framebuffer, non-uniform.
- Pass `1 / (pixels per world unit)` to the batcher so anti-aliasing padding
  stays one framebuffer pixel wide.

## SdfBatcher (`sdf_batcher.hpp`)
Instanced analytic SDF shapes: circle, capsule, rounded rect, ring. Each
instance (40 bytes) carries center, half-extent, rotation, a shape parameter,
fill color, optional outline width + color. All instances live in one
contiguous array that `flush()` uploads in one `update_buffer` and draws with
one instanced indexed draw (one unit quad, 6 indices). Anti-aliasing uses
`fwidth` of the distance, so edges are one pixel wide at any zoom or scale
mode. The fragment shader outputs premultiplied alpha (`BlendMode::Premultiplied`).

Shaders: `shaders/sdf.vert`, `shaders/sdf.frag` (GLSL 450, compiled for both
backends by the build).

## MeshBatcher (`mesh_batcher.hpp`)
Draws tessellated vector meshes (`cuervo/vector`, usually a whole SVG
document merged into one mesh). Meshes go into static GPU buffers once
(`create_mesh` / `update_mesh` for a new zoom level or an edited path). Each
frame, `draw(mesh, transform, tint)` instances are submitted in paint order:
- consecutive instances of the same single-batch mesh become **one instanced
  draw** (per-instance 2x3 transform + RGBA8 tint, 28 bytes);
- a mesh with several material batches (more gradients than one 16 KiB paint
  window holds) is drawn per instance, batch by batch, so overlapping
  instances keep painter's order.

Vertices are 16 bytes (document-space position, RGBA8, paint index). Solid
paint is the vertex color. Gradients are evaluated per fragment from the
batch's paint window (uniform binding 1). The fragment shader outputs
premultiplied alpha. Edges are anti-aliased by the device's MSAA (see
`cuervo/vector/README.md` for why MSAA rather than feathered geometry).
Conventions match Camera2D: world units, y down, radians clockwise.

Shaders: `shaders/mesh.vert`, `shaders/mesh.frag`.

## Text: MSDF fonts (`font.hpp`, `font_import.hpp`, `text_layout.hpp`, `text_renderer.hpp`)
DESIGN.md 4.4 "Text" and 7.2 "Fonts": TTF/OTF fonts become MSDF atlases.

- **Import** (`import_font`, `load_font_file`): FreeType + msdfgen +
  msdf-atlas-gen (`cmake/Dependencies.cmake`, behind `CUERVO_FONT_IMPORT`,
  default ON). Default charset: printable ASCII + Latin-1; 48 atlas texels per
  em; distance range 8 texels; MTSDF (rgb = multi-channel distance, a = true
  distance, kept for future outlines/glow); tightly packed square atlas
  (Liberation Sans Latin-1: 191 glyphs in 532x532, ~40 ms). Same font and
  options always give identical bytes (fixed edge-coloring seed; any thread
  count). Kerning comes from the font's `kern` table via FreeType.
- **FontAtlas / .efont**: the runtime format (glyph quads in ems, y down,
  atlas rectangles in texels, sorted kerning pairs, RGBA8 atlas rows top
  first). `serialize_font_atlas` / `deserialize_font_atlas` follow the
  ImportedVector versioning rule. `cuervo --font x.ttf --export x.efont`
  imports one; builds with `CUERVO_FONT_IMPORT=OFF` load only `.efont`.
- **Layout** (`layout_text`): UTF-8 (malformed bytes become U+FFFD), per
  codepoint glyphs with pair kerning, missing glyphs fall back to U+FFFD or
  `?`, `\n` / `\r\n` line breaks, greedy word wrap at spaces (`max_width`),
  tabs, per-line Left/Center/Right alignment, vertical anchor
  Top/Middle/Alphabetic/Bottom, line spacing. Returns positioned quads plus
  line and box metrics (for panels and hit tests). Not supported (planned, 11.3): HarfBuzz shaping
  (GPOS kerning, ligatures, complex scripts) and bidi, listed in DESIGN.md 7.2.
- **TextRenderer**: one RGBA8 linear-filtered texture per font; glyphs are
  48-byte instances (origin, two axis vectors, uv rectangle, color, distance
  range) on a unit quad, so any affine transform (rotation, shear, zoom) works.
  Consecutive text in one font is one instanced draw. `flush()` may run
  several times per frame (world text, then a screen HUD); each flush draws
  what was queued since the previous one. The fragment shader converts the
  median distance to screen pixels using `fwidth` of the uv, giving a
  one-pixel anti-aliased edge at any zoom (corners stay sharp when magnified;
  the range clamps to at least one pixel when minified). Premultiplied output.

Shaders: `shaders/text.vert`, `shaders/text.frag` (texture in set 1, binding 0).

## Benchmark checks (`cuervo`)
- `cuervo --game examples/bench --bench 10 [--backend vulkan|gl]`: VSync off,
  a 3 s warmup fills the pool (50k animated SDF shapes; `--press 1:more` for
  100k), then logs avg / p50 / p95 / p99 / max frame time.
  Release build, RTX 3060 Ti, 1280x720, 4x MSAA:
  50k shapes ~1120 FPS Vulkan / ~1070 FPS GL (0.9 ms/frame),
  100k ~570 FPS, 200k ~287 FPS on both; CPU-bound on instance building
  (~16 ns/shape). Debug build: ~100 FPS at 50k.
- `cuervo --demo vector [--zoom Z] [--screenshot f.ppm]`: SVG character plus
  world-space MSDF text (title, wrapped paragraph, rotated text, micro text
  readable only when zoomed in) and a screen-space HUD; zoom sweeps 1x..32x
  unless fixed. Both backends' captures match (differences only in the
  backend name and MSAA edge samples).
- `cuervo --demo particles [--bench S] [--intensity X]`: bullet-hell pattern on
  `cuervo/particles` (10k+ gameplay particles with collision, drawn through the
  SdfBatcher with `particles::draw_emitter`); see `cuervo/particles/README.md`
  for the numbers.

## Blend modes and post-processing (11.2b)
- `SdfBatcher::set_blend(SdfBlend::Normal | Additive | Screen)` marks a run: instances
  added afterwards composite that way, and `flush()` issues one draw per run with the
  run's pipeline (created on first use, so Normal-only users pay nothing). The shader
  outputs premultiplied color, so Additive is `src + dst` (`BlendMode::AdditivePremultiplied`)
  and Screen is `src + dst*(1 - src)`. `particles::RenderConfig::blend` sets an emitter's
  mode; `draw_emitters` turns each emitter into a run.
- `PostFx` (`post_fx.hpp`): the game draws into a render target (`rhi::RenderTargetDesc`);
  `apply(source, size, destination, PostSettings)` runs a bloom pyramid (threshold + 5-tap
  down chain, additive 3x3 tent up chain), then one composite pass with LUT grading, CRT
  (barrel, scanlines, aperture mask) and vignette; nothing enabled = a plain copy.
  `create_pass_pipeline` + `fullscreen` are the custom full-screen blit: a fragment shader
  that reads `v_uv`, `v_screen` and the `PostParams` block of `shaders/post.vert`.
- Sampling a render target: pass `flip_targets = true` (PostFx does), because GL stores
  target rows bottom-up (`DeviceInfo::render_target_flip_v`); `v_uv` is flipped for you.
- Cost note: targets use the device's MSAA count (so every pipeline fits every target), so
  each full-screen pass resolves; a single-sample target flavor needs pipeline variants.
- Parity: scenes `render_targets`, `additive_glow`, `post_bloom_vignette`, `post_lut_crt`
  (gl == gles, webgpu == vulkan exactly, gl vs vulkan within 1).

## Linear color and user shaders (11.2b)
- `[render] color_space = "linear"` (project.toml): `DeviceOptions::linear_color` asks for sRGB
  targets. Vulkan and WebGPU pick an sRGB surface (the window encodes on write,
  `DeviceInfo::window_srgb`); GL/GLES/WebGL2 keep a UNORM window and sRGB render targets
  (`SRGB8_ALPHA8`; desktop GL toggles `GL_FRAMEBUFFER_SRGB` per target, GLES keeps it on).
  The game then always draws into a scene target and `PostFx` (post_composite.frag) encodes
  where needed. Authored colors are converted by `working_color()` in the vertex shaders
  (`Globals.row0.w` = 1); sprite atlases are created `srgb`; the clear color is converted on
  the CPU (`srgb_to_linear`). Known gaps: mesh gradient paints and light colors still use the
  authored values; a device without sRGB targets logs a warning and stays in sRGB blending.
- `[render.post]` (`bloom`, `vignette`, `crt`, `lut`, ... see project.hpp) wires `PostFx`
  into the game loop; `shader = "shaders/x.glsl"` adds a user shader pass and
  `[render.post.params]` sets its `@export` values.
- User shaders (`user_shader.hpp`): a `.glsl` is a full-screen fragment shader written against
  `v_uv`, `v_screen`, `u_src`, `u_aux`, `o_color` with `@export float|vec2|vec3|vec4 name = default;`
  uniforms packed into `PostParams` v0..v2. `cuervo-tools shader build <project|file>` (and
  pack/export) compiles it with glslangValidator + shader_tool (+ spirv-opt + naga for WGSL)
  into a `.cshader`; the game loads only that, so there is no runtime compiler and the web
  build has none. `tier check` warns about a `.glsl` without a compiled `.cshader`.
