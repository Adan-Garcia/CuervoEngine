# Windows status

Windows is a target platform (DESIGN.md 2) and is **experimental**
([STATUS.md](STATUS.md)). The code was reviewed for POSIX-only assumptions and the
platform-dependent parts were moved behind portable abstractions. Builds and runs are
validated on a Windows laptop (Intel Core Ultra 9 185H with Intel Arc integrated graphics and
an NVIDIA RTX 4070), on both GPU paths. The Clang debug build passes the whole test suite
there and reaches the Linux state hashes ([Validation results](#validation-results)).
Windows stays experimental until the rest of milestone 10.5a (MSVC and MinGW-w64 builds,
CI, release builds, prebuilt binaries) is done; there are no prebuilt Windows binaries yet.
This page lists what was made portable, the results so far and the checklist that
validation works through.

## Made portable

| Area | Before | Now |
|---|---|---|
| Environment variables | `std::getenv` (deprecated under MSVC, C4996) | `platform::env` / `env_is` (`_dupenv_s` on MSVC) |
| Script hot reload | a watcher inside the game runner | `platform::FileWatcher`: mtime polling + file count, same code on Linux and Windows (no inotify / ReadDirectoryChangesW), off on the web |
| Paths in messages and sound names | `path.string()` (ANSI code page on Windows: non-ASCII names are lost) | `platform::utf8(path)` / `platform::path_from_utf8(text)` |
| Log file names | trimmed only at `cuervo/`-style forward-slash paths | also backslash paths (`__FILE__` under MSVC/MinGW) |
| Frame loop | blocking `while` in the runner | `platform::run_main_loop` (callback style; the same on desktop) |
| GL entry points | `__stdcall` handled (`CUERVO_GL_APIENTRY`) | unchanged; SDL's `SDL_GL_GetProcAddress` falls back to `opengl32.dll` for GL 1.1 functions |

Already portable before: all file access is `std::filesystem` / `std::fstream`
with binary modes where it matters, `project.toml` parsing and the lexer accept
CRLF line endings, threads are `std::thread`/`std::atomic`, audio uses
miniaudio (WASAPI on Windows), Vulkan is loaded at runtime through SDL + volk.

## Validation results

On the laptop above (Windows 11), with LLVM Clang targeting the MSVC runtime and
standard library (Visual Studio 2022 Build Tools), the `debug` preset:

| Check | Result |
|---|---|
| Clean build (`cmake --build --preset debug --clean-first`) | Zero warnings (re-checked after the fixes); the first build showed three: `std::getenv` in `pluma/aot.cpp` (now `_dupenv_s` under the MSVC runtime) and two `-Woverlength-strings` on the embedded `ui.plm` prelude (long preludes are now split into chunks under 64 KB, `cuervo_append_prelude` in `cuervo/CMakeLists.txt`) |
| `ctest --preset debug` | 23/23 pass, including `rhi_parity`, the editor and replay tests and `export_shmup` |
| Shmup state hash at tick 240 | `a3e57e79964421d3` on GL (Intel Arc) and Vulkan (RTX 4070), the same as Linux and the web |
| Windows export of the shmup | `export_shmup` exports with the Windows player, runs `shmup.exe` from another directory, and reaches the Linux hash, with bytecode and AOT scripts |

Two problems the first run found, both fixed:

- **Real devices in capture runs.** SDL's offscreen video driver needs EGL, which Windows
  lacks, so the tests open a real window there. `--screenshot` runs still read the real
  mouse and keyboard, and a cursor over the window changed UI hover and the state hash.
  A capture now reads only the scripted input (`--press`, `--pointer`, `--type`).
- **Offscreen driver forced on Windows.** `run_tutorial`, `ui_menus` and `export_stripped`
  set `SDL_VIDEO_DRIVER=offscreen` without the Windows guard the other tests have.

Commands in `cmd.exe` take backslash paths (`build\debug\runtime\cuervo --game examples\shmup ...`).

Not yet checked: MSVC `cl` and MinGW-w64 builds, a Release build, high-DPI pointer mapping
at 150 % (item 7) and a GUI-subsystem build (item 5).

## Checklist for validation

1. **Compilers.** MSVC (`/W4 /permissive- /fp:precise /utf-8` from
   `cmake/CompilerFlags.cmake`) must build with zero warnings (the zero-warning rule;
   narrowing conversions especially). MinGW-w64 GCC/Clang should be closer to the Linux
   builds.
2. **Determinism flags.** GCC/Clang use `-ffp-contract=off`; MSVC's
   `/fp:precise` must also not contract to FMA (check `/fp:contract` is off in
   the toolset used). Verify with the shmup state hash at tick 240
   (`--screenshot`): Linux and the web give `a3e57e79964421d3`. Clang with the MSVC
   runtime matches it on GL and Vulkan (see the results above).
3. **Dependencies.** SDL3 is fetched and built (no system package); FreeType is
   fetched when not found; `glslangValidator` must be on `PATH` (Vulkan SDK or glslang).
4. **Console output.** Log lines are UTF-8; the Windows console may need
   `chcp 65001` (or `SetConsoleOutputCP(CP_UTF8)` in the runtime) to show
   non-ASCII text.
5. **Subsystem.** `cuervo` is a console program with a plain `main`, so it
   doesn't need `SDL_main.h`; a GUI-subsystem release build would.
6. **Hot reload with editors that lock files** (some IDEs hold a write lock
   briefly): `read_scripts` may fail once; the next poll retries.
7. **High-DPI**: windows request `SDL_WINDOW_HIGH_PIXEL_DENSITY`; check pointer
   mapping at 150 % scaling.
8. **Export (docs/EXPORT.md).** `cuervo-tools export --platform windows`
   refuses to run without `--runtime <cuervo.exe>` from a Windows build. With
   it, it assembles `<game>.exe` + `game.pak` and warns that the result is
   untested. The release paths it relies on are portable (`SDL_GetBasePath` to
   find `game.pak`, `SDL_GetPrefPath` for saves, packs read with `std::ifstream`
   instead of mmap), and a Windows export of the shmup must reach the Linux
   state hash before Windows export counts as supported. It does with the Clang
   debug player (`export_shmup`); release players are still to check.
