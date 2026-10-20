# Contributing to Cuervo Engine

Thank you for your interest in contributing to Cuervo Engine and the Pluma programming language! We welcome bug reports, documentation improvements, new UI controls, language features, and engine optimizations.

---

## 1. Quick Start: Building from Source

### Requirements
- **OS:** Linux (x86_64) or Windows (WSL2 / Clang)
- **Compiler:** GCC 13+ or Clang 17+
- **Build System:** CMake ≥ 3.24 and Ninja
- **Shaders:** `glslangValidator` (Khronos `glslang`)
- **Windowing & Audio:** SDL3 (if not installed system-wide, CMake fetches and builds it automatically)
- **Graphics:** Vulkan 1.3 driver and/or OpenGL 3.3 Core (or GLES 3.0 / WebGL2)

### Building on Arch Linux
```sh
sudo pacman -S cmake ninja glslang sdl3
cmake --preset debug
cmake --build --preset debug
```

### Building on Ubuntu 24.04+ / Debian
```sh
sudo apt update && sudo apt install cmake ninja-build glslang-tools libsdl3-dev
cmake --preset debug
cmake --build --preset debug
```

---

## 2. Running Tests

Before submitting any code changes or pull requests, ensure that the test suite passes cleanly:

```sh
# Run the entire test suite headlessly (offscreen)
SDL_VIDEO_DRIVER=offscreen ctest --preset debug --output-on-failure
```

You can also run individual test suites directly:
```sh
./build/debug/tests/cuervo_tests        # Engine core, RHI, audio, collision, scenes
./build/debug/tests/pluma_tests         # Pluma compiler, VM, AOT, semantics
./build/debug/tests/editor_tests        # Editor commands and serialization
```

---

## 3. Architecture & Where to Contribute

The repository is organized into distinct, modular subsystems:

| Directory | Area | Description |
|---|---|---|
| `pluma/` | **Programming Language** | Completely standalone compiler (lexer, recursive-descent parser, type inference checker, bytecode VM, AOT-to-C compiler, LSP server, DAP debugger). Has **zero dependencies on `cuervo/`**. |
| `cuervo/script/prelude/` | **Engine Prelude** | Node hierarchy, UI widgets (`ui.plm`), and animation routines (`animation.plm`). Written in Pluma. **Best place to add high-level features without touching C++.** |
| `cuervo/rhi/` | **Render Hardware Interface** | Low-level abstraction with dual backends: Vulkan 1.3 (`cuervo/rhi/vulkan/`) and OpenGL 3.3/WebGL2 (`cuervo/rhi/gl/`). |
| `cuervo/render/` | **2D Rendering** | GPU-instanced 2D SDF batching, MSDF font atlas rendering, and vector path tessellation. |
| `editor/` | **Desktop Editor** | Standalone Dear ImGui application (`cuervo-editor`) providing the scene tree, inspector, and live play-in-editor. |
| `tools/` | **Developer Tooling** | `cuervo-tools` (exporting, asset packing, docs generation), `bench.py`, static site builder, and the VS Code extension (`tools/vscode-pluma/`). |
| `docs/` | **Documentation** | Architectural decision records (`DESIGN.md`), language guide (`PLUMA_GUIDE.md`), and engine reference. |

---

## 4. Development Principles

1. **Simulation Determinism:**
   * Cuervo is strictly deterministic: the same input stream on the same seed must produce the exact same state hash.
   * Never use unseeded random numbers, non-deterministic container iteration (e.g., iterating pointers directly in `std::unordered_map`), or wall-clock timing in simulation loops.
2. **RHI Feature Parity:**
   * Any new rendering feature must be implemented for **both** the Vulkan 1.3 and OpenGL 3.3/WebGL2 backends and verified by `tools/rhi_parity`.
3. **Documentation Coverage:**
   * Every public symbol added to `cuervo/script/prelude/*.plm` must have a doc comment. `cuervo-tools docs --check-coverage` enforces this in CI.
4. **Clean Commits:**
   * Use descriptive commit messages following conventional guidelines (e.g., `feat(ui): add Slider snap increments` or `fix(vm): handle negative integer modulo`).

---

## 5. Submitting a Pull Request

1. Fork the repository and create your branch from `main`.
2. Make your modifications, adhering to existing code formatting and naming conventions.
3. Add unit tests for your changes in `tests/`.
4. Ensure `ctest --preset debug` passes with zero regressions.
5. Submit a Pull Request describing your changes and referencing any related issue.
