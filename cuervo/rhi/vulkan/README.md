# Vulkan backend

Desktop backend (DESIGN.md 4.4). Requires Vulkan 1.3 (dynamic rendering +
synchronization2); machines without it fall back to the GL backend.

- Loading: volk, initialized from SDL's `vkGetInstanceProcAddr`. Nothing links
  against a Vulkan SDK; headers, volk and VMA are fetched by CMake.
- Memory: VMA. Every buffer is currently host-visible and persistently mapped
  (`HOST_ACCESS_SEQUENTIAL_WRITE`; VMA picks ReBAR/device-local memory when it can).
- Frames: 2 in flight, one "render finished" semaphore per swapchain image.
- Present mode: FIFO (VSync), or IMMEDIATE/MAILBOX with `DeviceOptions::vsync = false`.
  UNORM swapchain format to match GL's colors.
- Rendering: dynamic rendering into the swapchain image, no render pass objects.
  `clear()` before any draw becomes the attachment's load op; mid-pass or
  scissored clears use `vkCmdClearAttachments`.
- Y flip: the viewport has negative height (core since 1.1), so NDC +1 is the
  top on both backends and one projection serves both.
- Pipelines: one shared pipeline layout (set 0 = `rhi::max_uniform_bindings`
  uniform buffers, vertex + fragment). Viewport and scissor are dynamic state.
  Blend factors are identical to the GL backend's.
- Uniform buffers: descriptor sets are allocated per draw (only when the bound
  ranges change) from per-frame pools that are reset when the frame slot comes
  around again.
- Dynamic/Stream buffers are a per-buffer ring of versions: an update to a
  version the GPU may still read (in flight, or used by a draw recorded earlier
  this frame) switches to a version whose last-use serial has completed, or
  allocates one. Steady state is `frames_in_flight + 1` versions. Dynamic keeps
  a CPU shadow so the new version starts with the full contents. Stream keeps none (the
  extra pass cost 4 to 7 ms a frame at 2M instances). A Stream update that lands beside
  the bytes this frame already wrote (a batcher flushed once per canvas layer) writes in
  place: no draw recorded so far reads those bytes, and renaming instead meant a
  full-size second allocation per frame, four 77 MB versions at 1.8M instances, past the
  256 MB BAR window of a GPU without Resizable BAR, where writes then ran ~15x slower:
  the 1.9M-particle cliff. Only an update that overlaps and does not replace the frame's
  earlier writes renames and copies those bytes forward, reading the old version's mapped
  memory (write-combined, slow, but nothing in the engine does it).
- Memory placement: a Stream buffer whose versions would take more than half of the
  CPU-writable device-local heap (the BAR window; all of VRAM with Resizable BAR) is placed
  in system memory (`VMA_MEMORY_USAGE_AUTO_PREFER_HOST`), which the GPU reads over PCIe.
  This gives GL semantics: draws see the data as of when they were recorded.
- Destroyed buffers and pipelines are freed only after the last submission that
  could use them completes (submission serials).
- MSAA (`DeviceOptions::msaa_samples`, default 4, clamped to
  `framebufferColorSampleCounts`): a transient multisampled color image per
  swapchain size; dynamic rendering resolves it (`VK_RESOLVE_MODE_AVERAGE_BIT`)
  into the swapchain image when rendering ends, and the samples are never
  stored. Every pipeline uses the device's sample count. One image serves all
  frames in flight, ordered by a barrier at the start of each frame.
- `capture_frame` copies the swapchain image to a host buffer (needs
  `TRANSFER_SRC` swapchain usage, which NVIDIA/Mesa offer) and stalls the queue:
  debug only.
- Debug builds enable `VK_LAYER_KHRONOS_validation` when installed
  (Arch: `pacman -S vulkan-validation-layers`) and route messages to the log as
  `[vulkan]` lines; treat any of them as a bug.

- Textures: device-local `VK_IMAGE_TILING_OPTIMAL` images (`R8G8B8A8_UNORM`,
  `R8_UNORM`), one mip, sampled through one of two shared samplers (nearest,
  linear; clamp to edge). The shared pipeline layout's set 1 holds
  `max_texture_bindings` combined image samplers; like uniforms, a set is
  written per draw only when the bound textures changed. Writes
  (`create_texture` data or a clear, `update_texture`) copy into a staging
  buffer immediately and are recorded at `end_frame` into a per-frame upload
  command buffer submitted ahead of the frame's commands in the same
  `vkQueueSubmit2`; barriers order them after earlier frames' shader reads and
  before this frame's. Consequence (rhi.hpp): an update is visible to the
  *whole* frame it lands in, whereas GL keeps draws recorded before it on the
  old texels, so update before drawing. Images, views and staging buffers are
  freed through the submission-serial deferred delete (uploads count as uses).

Not implemented: staging uploads into device-local memory for Static buffers, depth
attachment, mipmaps, pipeline cache.

Headless note: the `offscreen` SDL driver needs `VK_EXT_headless_surface`,
which NVIDIA's driver lacks, so headless Vulkan smoke tests fall back to GL.

Tiers and parity (see `../README.md`):
- `max_tier` is Full. Capabilities: every RHI feature is core 1.3; MSAA from
  `framebufferColorSampleCounts`, frame capture if the surface offers
  `TRANSFER_SRC`. Limits from `maxImageDimension2D`, `maxUniformBufferRange`,
  `maxVertexInputAttributes`, `minUniformBufferOffsetAlignment`.
- Tier gate: with `DeviceOptions::tier = Compatible` (every Compatible
  project) Full features are refused like on GL and limits are clamped to
  `rhi::compatible_limits()` (4096 px textures, 4x MSAA, 256-byte uniform
  offsets), so Vulkan never lets a web-bound project use more than WebGL2 has.
- `draw_indexed_base_vertex` = `vkCmdDrawIndexed` with `vertexOffset`.
