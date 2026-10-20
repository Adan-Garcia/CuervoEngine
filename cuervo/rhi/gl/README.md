# OpenGL backend

One backend for OpenGL 3.3 core, OpenGL ES 3.0 and WebGL2 (DESIGN.md 4.4).
Only functions that exist in GLES 3.0 / WebGL2 may be used; they are listed
in `gl_loader.hpp` (no `glMapBuffer*`, no base-vertex/base-instance draws, no
`layout(binding = N)`, no compute or geometry shaders).

- Shaders: the build cross-compiles GLSL 450 to GLSL 330 and GLSL ES 300
  (`cmake/Shaders.cmake`); the device picks the ES source when the context
  version string starts with "OpenGL ES". Uniform block bindings are applied
  after linking with `glUniformBlockBinding`, from the table the shader tool
  reflected out of the SPIR-V.
- Vertex input: one VAO stays bound for the device's lifetime; each draw
  re-specifies the bound pipeline's attributes (`glVertexAttrib[I]Pointer` +
  `glVertexAttribDivisor` for per-instance streams).
- `first_instance` is emulated by offsetting per-instance stream pointers,
  because GLES 3.0 has no base-instance draws. Indexed draws have no base vertex.
- Buffers: a buffer is only ever bound to the target of its `BufferKind`
  (WebGL2 forbids rebinding to another target). A Stream buffer is
  three GL buffers used in turn, one per frame it is updated in, so the first update of a
  frame writes storage the GPU finished with and never waits on, or reallocates, what
  earlier frames still read (the web build, which has no fences, orphans with
  `glBufferData(nullptr)` instead). Later updates in the same frame hit the same version,
  keeping the frame's other bytes, matching the Vulkan backend's renaming. Uploads go in 1 MB
  `glBufferSubData` slices: one 76 MB call made the NVIDIA driver stage the whole range
  and cost 3 to 4 times as much beyond ~1.9M instances.
- Frames in flight: `end_frame` inserts a fence (`glFenceSync`) after the swap and, once more
  than two are outstanding, blocks (`glClientWaitSync`, in bounded slices) on the oldest. Without
  the bound the CPU outran the GPU by hundreds of frames at small loads and every queued frame
  kept its stream buffers alive (1.5 GB peak at 1M particles); it also made GL frame times
  measure CPU queueing, not GPU completion. WebGL2 forbids blocking waits on the main thread
  (and the browser paces frames), so the web build keeps no fences.
  `FenceRing` (`cuervo/rhi/frame_fence_ring.hpp`) is the backend-independent bookkeeping and is
  unit tested; `gl::peak_frames_in_flight` reports the most that queued.
- Viewport/scissor take top-left-origin rectangles and are flipped to GL's
  bottom-left origin. The scissor test stays enabled so `clear()` honors the
  scissor like Vulkan's.
- MSAA (`DeviceOptions::msaa_samples`, default 4, clamped to `GL_MAX_SAMPLES`):
  frames render into a multisampled renderbuffer FBO (`glRenderbufferStorageMultisample`)
  that `end_frame` resolves into the default framebuffer with `glBlitFramebuffer`,
  all GLES 3.0 / WebGL2. The renderbuffer is RGBA8 or RGB8 to match the window's
  format (GLES requires identical formats for a multisample resolve blit). The
  scissor test is disabled around the blit (it clips blits too). An incomplete
  FBO falls back to no MSAA with an error.
- `capture_frame` reads the back buffer with `glReadPixels` after the resolve, before the swap.
- Debug builds log `glGetError` results once per frame.
- Textures (`create_texture` etc.): `glTexImage2D` with sized formats (`GL_RGBA8`,
  `GL_R8`), one level (`GL_TEXTURE_MAX_LEVEL` 0), clamp-to-edge, filter from
  the desc; no `glTexStorage2D` so GL 3.3 contexts work too. Texture created
  without data are uploaded zeros (GL leaves them undefined). Sampler binding N
  is texture unit N: after linking, each sampler uniform from the shader tool's
  table is set with `glUniform1i`. Uploads (`glTexSubImage2D`) use unit
  `max_texture_bindings` so they never disturb bound textures. Updates are
  ordered like buffer updates (draws before see old texels).

Tiers, parity and web (see `../README.md`):
- Capabilities: every `Tier::Compatible` feature; MSAA if `GL_MAX_SAMPLES` >= 2.
  Limits from `GL_MAX_TEXTURE_SIZE`, `GL_MAX_UNIFORM_BLOCK_SIZE`,
  `GL_MAX_VERTEX_ATTRIBS`, `GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT`, `GL_MAX_SAMPLES`.
  `max_tier` is always Compatible: `draw_indexed_base_vertex` logs once and
  draws nothing (GLES 3.0 has no base vertex).
- ES contexts: `WindowDesc::gl_es` (`--gl-es`, `CUERVO_GL_ES=1`, always on the
  web) requests OpenGL ES 3.0; the device also detects ES from the version
  string ("OpenGL ES ...", Emscripten's "OpenGL ES 3.0 (WebGL 2.0 ...)") and then
  compiles the GLSL ES 300 sources. On NVIDIA desktop the ES path renders
  bit-identically to GL 3.3 core (`rhi_parity` test).
- `begin_frame` unbinds the uniform ranges of the previous frame
  (`glBindBufferBase(..., 0)`), matching Vulkan's per-frame reset.
- Desktop core contexts enable `GL_PROGRAM_POINT_SIZE` so `gl_PointSize` works as
  on GLES/Vulkan (the enum doesn't exist in GLES, where it's always on).
- The window has no multisampled default framebuffer
  (`SDL_GL_MULTISAMPLEBUFFERS 0`): WebGL2 can't blit into one, and the MSAA
  resolve blit needs a single-sampled destination everywhere.
