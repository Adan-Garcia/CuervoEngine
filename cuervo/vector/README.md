# cuervo/vector

SVG import, curve flattening, tessellation and stroking (DESIGN.md 4.5), plus
the ImportedVector binary format (DESIGN.md 4.11). Rendering lives in
`cuervo/render/mesh_batcher.hpp`.

Pipeline:

```
SVG text --svg.hpp--> VectorDocument --mesh.hpp (DocumentTessellator)--> VectorMesh --MeshBatcher--> GPU
            (curves, resolved styles)        flatten + fill/stroke/clip       (triangles + paint)
                          \--imported_vector.hpp--> .evec file (document + pre-tessellated base mesh)
```

## Files
- `path.hpp`: `Path` (move/line/quad/cubic/close; arcs become cubics of at
  most 90 degrees per SVG 1.1 F.6), the SVG path-data parser (every command,
  implicit repetition, compact numbers and arc flags), tight bounds, hashing.
- `flatten.hpp`: adaptive flattening. Segment counts come from Wang's formula,
  which bounds the curve-to-polyline distance by the tolerance.
  `tolerance_for_scale(pixels_per_unit)` = 0.25 px in document units.
- `tessellate.hpp`: libtess2 wrapper. Fills (nonzero/evenodd, any
  self-intersection) become non-overlapping triangles. Normalized regions
  support union / intersection / subtraction (`|winding| >= 2`,
  positive-after-reversal), which is how clip paths and stroke occlusion work.
- `stroke.hpp`: dashes, then one outline polygon per polyline (offsets plus
  inline joins and caps, with a pivot through the vertex on the inner side).
  Its nonzero fill is exactly the stroke area, so translucent strokes cover
  each pixel once.
- `document.hpp`: the resolved document. Shapes keep their curves and their
  full transform; styles, gradients (with href inheritance) and clip paths
  are resolved.
- `svg.hpp`, `xml.hpp`: the importer and its minimal XML parser.
- `mesh.hpp`: `VectorMesh` (16-byte vertices: position, RGBA8, paint index),
  gradient blocks, material batches, `DocumentTessellator`.
- `imported_vector.hpp`: binary format, `import_svg`, `load_vector_file`.
- `vector_asset.hpp`: an asset plus its zoom-level mesh cache.

## Zoom levels
Meshes are built per level `L = ceil(log2(pixels per unit))` at scale `2^L`,
so a mesh is never coarser than 0.25 px and is rebuilt only when the zoom
crosses a power of two (a change of more than 2x). `VectorAsset` keeps the 4
most recently used levels. `DocumentTessellator` caches geometry per shape,
keyed by a hash of path, transform, stroke style, fill rule and clip contents.
Editing one path (morphing, the future canvas API) re-tessellates only that
shape, and paint changes re-tessellate nothing.

## Paint
Solid colors are vertex colors. Gradients are evaluated per fragment
(`render/shaders/mesh.frag`): each gradient use is a block in a 16 KiB paint
window (1024 vec4; the GLES 3.0 minimum uniform block size) holding the
inverse document-to-gradient transform, the geometry, premultiplied stop
colors and stop offsets. A mesh whose gradients overflow one window is split
into several material batches, one draw call each. Gradients with more than
64 stops are resampled to 64. Radial gradients support the focal point (kept
just inside the circle, SVG 1.1); `fr` is not supported. Spread pad, reflect
and repeat all work.

## Anti-aliasing
Vector meshes rely on MSAA (`rhi::DeviceOptions::msaa_samples`, default 4x).
It is available on every target: Vulkan multisampled attachment + resolve, and
on GL / GLES 3.0 / WebGL2 a multisampled renderbuffer blitted to the default
framebuffer. MSAA was chosen over feathered edge geometry because it:
- has no conflation seams where shapes share an edge (feathering draws a
  half-transparent fringe on both sides of every shared edge);
- covers strokes, clip edges and self-intersecting fills the same way;
- stays exactly one pixel wide at any zoom, with no geometry rebuild or
  per-zoom vertex extrusion;
- matches librsvg's output within a fraction of a percent of pixels in our
  test captures.
The cost is fill rate (4 samples per pixel), which is cheap for 2D scenes.

## Opacity
`opacity` is applied per shape, multiplied down the group tree (no offscreen
layers). That's exact when the faded shapes don't overlap. One common case is
handled exactly: a translucent element with an opaque stroke gets the
stroke's area cut out of its fill, so the fill doesn't show through. Grouped
overlapping shapes under group opacity still show their overlaps (librsvg
composites the group first). Real group layers need render targets in the RHI.

## Unsupported (skipped, listed in one log warning per file)
CSS `<style>` sheets (presentation attributes and `style=""` work), `<text>`,
`<image>`, filters, masks, patterns, markers, `<foreignObject>`, SMIL
animation, `vector-effect`, `stroke-linejoin="arcs"`, `fr`, clip paths on
`<clipPath>` or its children, external references. DESIGN.md 4.5 plans a
rasterized fallback for some of these; it isn't built yet. The importer was
run over the 42,246 SVGs in `/usr/share` (ASan + UBSan, import + tessellate +
binary round trip) with no failures. The most common skipped feature there is
CSS stylesheets (KDE's Breeze icons use them for color schemes).

## ImportedVector format
Little-endian: magic `EVEC`, u32 version (1), u32 flags (bit 0 = base mesh
present), u32 reserved, then the document (size, gradients, clip paths,
shapes, with curves) and optionally the base mesh with its level. The reader
validates every count and index, rejects truncation, trailing bytes and newer
versions, and is where migrations from older versions go. Shipping builds set
`-DCUERVO_SVG_IMPORT=OFF` so `load_vector_file` accepts only `.evec`. To import:
`cuervo --svg in.svg --export out.evec`.

## Determinism
None of this runs in the simulation, so it uses `<cmath>` (DESIGN.md 4.1
restricts libm in sim paths only).
