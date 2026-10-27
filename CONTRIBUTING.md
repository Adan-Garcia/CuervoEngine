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
4. **Zero Warnings:**
   * The build has no warnings on GCC and Clang (`-Wall -Wextra -Wpedantic -Wshadow`). Third-party code goes in `cmake/Dependencies.cmake`, pinned to an exact tag and included as SYSTEM, with its license in `THIRD_PARTY_NOTICES.md` (the `third_party_notices` test checks this).
5. **Code Style:**
   * Headers sit next to their sources and are included from the repository root (`#include "cuervo/core/log.hpp"`). Functions and variables are `snake_case`, types `PascalCase`, private members end in `_`, namespaces are `cuervo::<area>` and `pluma`.
   * Comments explain *why*, and cite the DESIGN.md section when a design rule drives the code. `cuervo-tools fmt` formats Pluma.
   * `pluma/` never depends on `cuervo/`: the engine binds into the language, not the reverse.
6. **Clean Commits:**
   * Use descriptive commit messages following conventional guidelines (e.g., `feat(ui): add Slider snap increments` or `fix(vm): handle negative integer modulo`).

---

## 5. Adding a Prelude Node or a Native

Most engine features reach scripts as **prelude** code: Pluma sources in `cuervo/script/prelude/` that are compiled before every game.

1. Declare the API in Pluma. Write the node as an ordinary class (`class Spinner extends Node2D { ... }`), and give every public symbol a doc comment (`api_docs_coverage` checks this).
2. If part of it must be C++, declare that function or method `@native` with an empty body, and register the host function under its qualified name with `Bindings::add_native("Spinner.spin", ...)` in a `cuervo/script/*_bindings.cpp` file (`audio_bindings.cpp` is a small example). The checker reports a `@native` declaration that no host function is bound to.
3. Keep simulation natives deterministic: use `cuervo/math/dmath` rather than libm, iterate containers in a fixed order, and take randomness only from the seeded PCG32.
4. Add doctest tests in `tests/cuervo/` (engine) or `tests/pluma/` (language), and regenerate the API reference with `cuervo-tools docs -o docs/API.md`.

Code that doesn't belong in the engine can ship as a package or a native plugin instead ([docs/PACKAGES.md](docs/PACKAGES.md), [docs/PLUGINS.md](docs/PLUGINS.md)).

## 6. Submitting a Pull Request

1. Fork the repository and create your branch from `main`.
2. Make your modifications, adhering to existing code formatting and naming conventions.
3. Add unit tests for your changes in `tests/`.
4. Ensure `ctest --preset debug` passes with zero regressions.
5. Sign off every commit (see below).
6. Submit a Pull Request describing your changes and referencing any related issue.

### Sign-off (Developer Certificate of Origin)

Contributions are accepted under the project's MIT License ([LICENSE.md](LICENSE.md)). There is no contributor license agreement. Instead, every commit carries a `Signed-off-by` line certifying the [Developer Certificate of Origin 1.1](https://developercertificate.org/): you wrote the change, or otherwise have the right to submit it under the project's license. Add the line with `git commit -s`. It must use your real name or the name you are known by, and an email address that reaches you. A pull request with unsigned commits is asked to sign them (`git rebase --signoff main`) before it is merged.

Code you didn't write keeps its own license. Vendor third-party code only through `cmake/Dependencies.cmake`, and give it a notice and its license text.

## 7. Design Changes

Decisions recorded in [docs/DESIGN.md](docs/DESIGN.md) are settled, so a change that contradicts one starts as a proposal rather than a pull request:

1. Open a feature request that names the section (for example, "DESIGN.md §4.18") and explains what the current decision prevents and what should replace it.
2. Once it is agreed, the pull request that implements it also edits DESIGN.md. Settled choices become a **Decision:** note, and rules found or refined while implementing become a *Clarified* note under the milestone. Notes give the reason and say what changed, with neutral wording and no dates.
3. A change to the milestones table (DESIGN.md §6) also regenerates the public roadmap ([docs/ROADMAP.md](docs/ROADMAP.md)) with `python3 -I tools/gen_roadmap.py --write`. The `roadmap_up_to_date` test checks it, and `tools/support_matrix.py --write` does the same for a change in a feature's support status.

Small clarifications that don't change a decision (a missing edge case, a wording fix) go straight into a pull request.

## 8. Sharing Packages and Plugins

Packages and plugins don't need to live in this repository. Publish them to a registry with `cuervo-tools plugin publish`, which checks the license declaration, the engine and ABI ranges and, for sim-safe plugins, a conformance replay. See [docs/PLUGINS.md](docs/PLUGINS.md#sharing-plugins).

## 9. Conduct and Security

Everyone taking part follows the [Code of Conduct](CODE_OF_CONDUCT.md). Report security problems privately, as [SECURITY.md](SECURITY.md) describes, and never in a public issue.
