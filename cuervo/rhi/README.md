# RHI: tiers, capabilities and backend parity

The render hardware interface (`rhi.hpp`, DESIGN.md 4.4) has two backends,
Vulkan (`vulkan/`) and GL (`gl/`: GL 3.3 core, GLES 3.0, WebGL2). Both
implement every feature in the same change; this page is the parity
audit and the rules for tiers.

## Tiers and capabilities

- `rhi::Feature` lists every feature a device may or may not have, each tagged
  with a tier (`feature_tier`) and whether a project needs it (`feature_required`;
  MSAA and frame capture are optional: they degrade).
- `DeviceInfo::hardware_features` = what the backend + GPU can do;
  `DeviceInfo::features` = what may be used now, after the **tier gate**
  (`DeviceOptions::tier`, lowered to `max_tier`). A Vulkan device gated to
  Compatible refuses Full features (log once, draw nothing / invalid handle)
  exactly like GL, and clamps its limits to `compatible_limits()`, so a
  Compatible project tested on Vulkan behaves as on WebGL2.
- `check_tier(info, tier)` says what a device lacks for a tier; the game
  runner refuses a Full project on a device that can't serve Full and only
  warns for Compatible (`script::check_device_tier`).
- `rhi_parity features --backend gl|vulkan [--gl-es]` prints the report.

| Feature | Tier | GL / GLES / WebGL2 | Vulkan |
|---|---|---|---|
| instanced draws, `first_instance` | Compatible | yes (`first_instance` emulated by offsetting instance streams) | yes |
| 32-bit indices | Compatible | yes | yes |
| uniform buffers with offsets | Compatible | yes | yes |
| RGBA8 / R8 textures, nearest / linear | Compatible | yes | yes |
| integer vertex attributes (`UInt1`) | Compatible | yes | yes |
| points, lines, line strips | Compatible | yes | yes |
| MSAA (optional) | Compatible | if `GL_MAX_SAMPLES` >= 2 | if `framebufferColorSampleCounts` >= 2 |
| frame capture (optional) | Compatible | yes | if the surface allows `TRANSFER_SRC` |
| base-vertex draws (`draw_indexed_base_vertex`) | **Full** | no (GLES 3.0 has none) | yes |

Compatible limits (what the tier promises everywhere): textures 4096 px,
4x MSAA, 16 KB per uniform range, 16 vertex attributes, uniform offsets
aligned to 256 bytes. The GLES 3.0 spec minimums are lower for textures
(2048) and uniform blocks; DESIGN.md 4.4 "Clarified (Phase 5)" explains the choice.

## Parity audit

Every `Device` entry point, checked against both backends. "Scene" names the
`tools/rhi_parity` capture that exercises it; the `rhi_parity` test renders all
of them on GL 3.3, GLES 3.0 and (with a display) Vulkan and compares the images.

| Entry point / behavior | Status | Scene |
|---|---|---|
| `begin_frame` resets viewport, scissor and all bindings | **fixed**: GL kept uniform ranges bound across frames; now unbinds them like Vulkan | every scene |
| `clear` honors the scissor | same | `clear_scissor_viewport` |
| `set_viewport` / `set_scissor`, top-left origin | same | `clear_scissor_viewport` |
| `create/update/destroy_buffer`, Static/Dynamic/Stream, GL ordering (draws see data as of recording) | same; Stream: GL rotates three buffers behind a bounded frame fence ring, Vulkan writes disjoint ranges in place | `buffer_update_order` |
| `bind_uniform_buffer` offsets and ranges | same | `uniform_offsets` |
| `create/update/destroy_texture`, formats, filters, zero-initialized | same | `textures` |
| `create_pipeline`: blend modes | same factors | `blend_modes` |
| `create_pipeline`: triangle list / strip | same | `triangles_and_strip` |
| `create_pipeline`: points (`gl_PointSize`), lines | **fixed**: GL 3.3 core ignored `gl_PointSize` without `GL_PROGRAM_POINT_SIZE` (always on in GLES and Vulkan) | `points_lines` |
| vertex formats Float2, UByte4Norm, UInt1; per-instance streams; stream offsets | same | `instanced_first_instance` |
| `draw` / `draw_indexed`: `first_vertex`, `first_index`, `first_instance`, u16/u32 | same | `indexed_16_32`, `instanced_first_instance` |
| `draw_indexed_base_vertex` (Full) | Vulkan draws, matches its reference; GL and Compatible-gated Vulkan refuse | `base_vertex` |
| `capture_frame` | same PPM output (after the MSAA resolve) | all |
| texture size limits | **unified**: one check (`detail::texture_size_allowed`) against the gated limit | unit test |
| MSAA sample count choice | **unified** (`detail::msaa_sample_count`) | unit test |
| GLES 3.0 context on desktop (`--gl-es`) | new; images identical to GL 3.3 | `rhi_parity` test |

Known, accepted differences (documented in `rhi.hpp` / the backend READMEs):

1. **Edges.** Vulkan flips y with a negative viewport height; GL renders
   bottom-up. That mirrors the MSAA sample pattern vertically and the rounding
   of positions exactly halfway between pixels or texels, so anti-aliased edges
   differ by a step (up to ~1.5 % of a scene's pixels, all on color edges).
   `rhi_parity compare` therefore fails only on *interior* differences.
   The shmup frame differs by at most 1/255 on 29 pixels.
2. **Texture updates mid-frame.** On Vulkan an `update_texture` is visible to
   the whole frame it lands in; on GL only to draws recorded after it. Update
   before drawing (rhi.hpp).
3. **Validation timing.** Vulkan checks uniform/texture handles at draw time,
   GL at bind time; both log and skip.
4. Vertex formats Float1/Float3/Float4 are not in a parity scene (the engine's
   batchers use them and render identically in the shmup comparison).
