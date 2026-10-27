# Third-party notices

Cuervo Engine is distributed under the MIT License ([LICENSE.md](LICENSE.md)). It includes,
links to or ships with the components below, each under its own license.

The full license texts are in [`third_party/licenses/`](third_party/licenses/), copied
verbatim from each component's pinned release (`cmake/Dependencies.cmake`) or, for
libraries taken from the system, from the system's license directory. `cuervo-tools export`
copies this file and that directory into every exported game (`licenses/`), so a shipped
game carries the notices its player needs. The packages and plugins a game uses
(docs/PACKAGES.md) keep their own licenses: export generates `licenses/PACKAGES.md` from
their manifests and copies their license texts to `licenses/packages/`.

`ctest -R third_party_notices` fails when a dependency is added to
`cmake/Dependencies.cmake` without an entry here and a license text.

## In the engine and its players

A full player (`--engine full`) contains all of these; a stripped one only what its game uses.

| Component | Version | License | Used by | License text |
|---|---|---|---|---|
| SDL3 | 3.2.10 if fetched, else the system's (3.4 here) | zlib | runtime, editor, tools: windows, input, GL context | [SDL3.txt](third_party/licenses/SDL3.txt) |
| Vulkan-Headers | vulkan-sdk-1.4.363.0 | Apache-2.0 / MIT | Vulkan backend (players of Full-tier games) | [Vulkan-Headers.txt](third_party/licenses/Vulkan-Headers.txt) |
| volk | vulkan-sdk-1.4.363.0 | MIT | Vulkan backend | [volk.txt](third_party/licenses/volk.txt) |
| Vulkan Memory Allocator | 3.3.0 | MIT | Vulkan backend | [VulkanMemoryAllocator.txt](third_party/licenses/VulkanMemoryAllocator.txt) |
| wgpu-native (with wgpu, naga and their Rust crates) | v29.0.1.1 | MIT OR Apache-2.0 | WebGPU backend, desktop (`CUERVO_WEBGPU`; experimental) | [wgpu-native.txt](third_party/licenses/wgpu-native.txt) |
| emdawnwebgpu | Emscripten's port (emsdk 6.0.12: v20260423.175430) | MIT / University of Illinois NCSA | WebGPU backend, web builds with `CUERVO_WEBGPU` | [emdawnwebgpu.txt](third_party/licenses/emdawnwebgpu.txt) |
| libtess2 | 8dbd648 | SGI Free Software License B 2.0 | vector path tessellation | [libtess2.txt](third_party/licenses/libtess2.txt) |
| miniaudio | 0.11.25 | Public domain (Unlicense) or MIT-0 | audio (players of games with sound) | [miniaudio.txt](third_party/licenses/miniaudio.txt) |
| Box2D | 3.2.0 | MIT | 2D physics simulation (players of games with physics) | [box2d.txt](third_party/licenses/box2d.txt) |
| FreeType | system, or 2.13.3 if fetched | FreeType License (FTL) | font import (development builds, tools) | [FreeType.txt](third_party/licenses/FreeType.txt) |
| msdfgen | 1.12 | MIT | font and vector import (MSDF atlases) | [msdfgen.txt](third_party/licenses/msdfgen.txt) |
| msdf-atlas-gen | 1.3 | MIT | font atlas import | [msdf-atlas-gen.txt](third_party/licenses/msdf-atlas-gen.txt) |
| zlib | system (1.3.2 here) | zlib | through FreeType | [zlib.txt](third_party/licenses/zlib.txt) |
| bzip2 | system (1.0.8 here) | bzip2 (BSD-style) | through FreeType | [bzip2.txt](third_party/licenses/bzip2.txt) |
| libpng | system (1.6.59 here) | libpng (PNG Reference Library) | through FreeType | [libpng.txt](third_party/licenses/libpng.txt) |
| Brotli | system (1.2.0 here) | MIT | through FreeType | [brotli.txt](third_party/licenses/brotli.txt) |

Portions of this software are copyright © The FreeType Project (www.freetype.org). All
rights reserved. (The credit the FreeType License asks for.)

Stripped players (`cuervo-tools export`, docs/EXPORT.md "Stripped players") leave out what a
game doesn't use: without font import they don't link FreeType or its libraries, and
compatible-tier games ship without the Vulkan backend.

## Development only (not in exported games)

| Component | Version | License | Used by | License text |
|---|---|---|---|---|
| Dear ImGui | 1.92.9b (docking branch) | MIT | the editor (`cuervo-editor`) | [DearImGui.txt](third_party/licenses/DearImGui.txt) |
| Outfit (font) | 1.1 | SIL Open Font License 1.1 | the editor: display type (splash) | [Outfit.txt](third_party/licenses/Outfit.txt) |
| Figtree (font) | 2.0.3 | SIL Open Font License 1.1 | the editor: UI type | [Figtree.txt](third_party/licenses/Figtree.txt) |
| JetBrains Mono (font) | 2.304 | SIL Open Font License 1.1 | the editor: code and console type | [JetBrainsMono.txt](third_party/licenses/JetBrainsMono.txt) |
| doctest | 2.4.12 | MIT | the test suites | [doctest.txt](third_party/licenses/doctest.txt) |
| SPIRV-Cross | vulkan-sdk-1.4.363.0 | Apache-2.0 (some files MIT) | `shader_tool`: GLSL 330 / ES 300 from SPIR-V at build time (not linked into the engine) | [SPIRV-Cross.txt](third_party/licenses/SPIRV-Cross.txt) |
| Tracy | 0.11.1 | BSD-3-Clause | optional profiler bridge (`CUERVO_TRACY`, off by default) | [Tracy.txt](third_party/licenses/Tracy.txt) |
| glslangValidator | the system's / Vulkan SDK | BSD-3-Clause and others | build-time shader compiler (not linked) | see the Khronos glslang repository |
| naga-cli | 29.0.4 | MIT OR Apache-2.0 | build-time SPIR-V -> WGSL translator for the WebGPU backend (not linked) | see the gfx-rs/wgpu repository |
| spirv-opt (SPIRV-Tools) | the system's / Vulkan SDK | Apache-2.0 | build-time: splits combined samplers for WGSL (not linked) | see the Khronos SPIRV-Tools repository |

## Fonts and assets

| Asset | License | License text |
|---|---|---|
| Liberation Sans (`assets/fonts/LiberationSans-Regular.ttf`), the default font | SIL Open Font License 1.1 | [LiberationSans.txt](third_party/licenses/LiberationSans.txt) |
| The Cuervo Mark, wordmark and design tokens (`assets/brand/`, also embedded as the window icon) | the Cuervo brand, synced from the Cuervo design kit (not a third-party component) | `assets/brand/README.md` |
| Example audio (`examples/*/assets`) | made for this repository by `tools/gen_shmup_audio.py` | each example's `assets/AUDIO_LICENSE.txt` |

## VS Code extension (`tools/vscode-pluma`)

The packaged extension (`.vsix`) bundles these npm packages, with their own license files:
vscode-languageclient 9.0.1, vscode-languageserver-protocol 3.17.5,
vscode-languageserver-types 3.17.5, vscode-jsonrpc 8.2.0, balanced-match 1.0.2 and
brace-expansion 2.1.7 (MIT); minimatch 5.1.9 and semver 7.8.5 (ISC).
