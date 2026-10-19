# Windows readiness

Windows is a target platform (DESIGN.md 2) but no Windows toolchain (MSVC or
MinGW) has been available yet, so **nothing here has been compiled or run on
Windows**. Phase 5 reviewed the code for POSIX-only assumptions and moved the
platform-dependent parts behind portable abstractions; this page lists what was
done and what remains to verify.

## Made portable (Phase 5)

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

## Untested / to verify on the first Windows build

1. **Compilers.** MSVC (`/W4 /permissive- /fp:precise /utf-8` from
   `cmake/CompilerFlags.cmake`) has never seen the code: expect a round of
   warnings to fix for the zero-warning rule (narrowing conversions especially).
   MinGW-w64 GCC/Clang should be closer to the Linux builds.
2. **Determinism flags.** GCC/Clang use `-ffp-contract=off`; MSVC's
   `/fp:precise` must also not contract to FMA (check `/fp:contract` is off in
   the toolset used). Verify with the shmup state hash at tick 240
   (`--screenshot`): Linux/web give `13cd5d06434bf435`.
3. **Dependencies.** SDL3 is fetched and built (no system package); FreeType is
   fetched when not found; `glslangValidator` must be on `PATH` (Vulkan SDK).
4. **Console output.** Log lines are UTF-8; the Windows console may need
   `chcp 65001` (or `SetConsoleOutputCP(CP_UTF8)` in the runtime) to show
   non-ASCII text.
5. **Subsystem.** `cuervo` is a console program with a plain `main`, so it
   doesn't need `SDL_main.h`; a GUI-subsystem release build would.
6. **Hot reload with editors that lock files** (some IDEs hold a write lock
   briefly): `read_scripts` may fail once; the next poll retries.
7. **High-DPI**: windows request `SDL_WINDOW_HIGH_PIXEL_DENSITY`; check pointer
   mapping at 150 % scaling.
8. **Export (Phase 7C, docs/EXPORT.md).** `cuervo-tools export --platform windows`
   refuses to run without `--runtime <cuervo.exe>` from a Windows build. With
   it, it assembles `<game>.exe` + `game.pak` but stays untested. The release
   paths it relies on are portable (`SDL_GetBasePath` to find `game.pak`,
   `SDL_GetPrefPath` for saves, packs read with `std::ifstream` instead of mmap)
   but have never run on Windows.
