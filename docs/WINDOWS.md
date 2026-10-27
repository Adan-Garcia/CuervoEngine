# Windows status

Windows is a target platform (DESIGN.md 2). Milestone 10.5a verified it: the engine builds
with zero warnings using MSVC, MinGW-w64 GCC and Clang. The test suite passes and the
shmup reaches the Linux state hash. Exports ship a GUI-subsystem player, and the release
workflow publishes prebuilt editor, tools and export templates. Validation ran on a
Windows 11 laptop (Intel Core Ultra 9 185H, Intel Arc integrated graphics, NVIDIA RTX 4070,
desktop at 150 % scaling). [STATUS.md](STATUS.md) gives the support level.

## Building

| Toolchain | Preset | Notes |
|---|---|---|
| MSVC (`cl`, VS 2022 Build Tools 17.14 or newer) | `msvc-debug`, `msvc-release` | Run from a VS developer prompt (`vcvars64.bat`). AOT scripts need LLVM's `clang` too (found under `C:/Program Files/LLVM` or `$LLVM_DIR`), because the generated C uses GNU extensions. |
| MinGW-w64 GCC (UCRT, GCC 13 or newer; checked with 16.2) | `mingw-debug`, `mingw-release` | `gcc`/`g++` on `PATH`. Use the system or MSYS2 CMake: some MinGW bundles ship a `cmake.exe` whose TLS certificates are broken, and FetchContent downloads then fail. |
| Clang targeting the MSVC runtime | `debug`, `release` | LLVM plus the VS Build Tools (libraries and `rc`). |

All of them need `glslangValidator` on `PATH` (Vulkan SDK or glslang). SDL3 and FreeType
are fetched. The Rust example plugin needs `cargo`, and the C# one needs the .NET SDK.
Both are optional. MinGW-w64 builds link the Rust plugin into AOT modules only when Rust's
GNU target is installed (`rustup target add x86_64-pc-windows-gnu`).

```bat
cmake --preset msvc-release
cmake --build --preset msvc-release
ctest --preset msvc-release
```

MSVC's Debug standard library (iterator debugging) makes the suite about three times
slower than Release. CI therefore runs MSVC in Release and MinGW-w64 in Debug
(`.github/workflows/ci.yml`).

## What every Windows program gets

`cuervo_windows_app` (cmake/Windows.cmake) sets up `cuervo`, `cuervo-player`,
`cuervo-editor`, `cuervo-tools`, `plm` and the test programs:

- **A manifest** (cmake/windows/cuervo.manifest). It sets the UTF-8 active code page, so
  `argv`, narrow file names and `path::string()` are UTF-8 instead of the ANSI code page. It
  also sets PerMonitorV2 DPI awareness and long paths.
- **UTF-8 console output** (cmake/windows/console.cpp). The console's output code page is
  UTF-8 while the program runs and is restored at exit.
- **GUI subsystem** for `cuervo-player.exe` (what `export --platform windows` ships) and
  for `cuervo-editor.exe`. Neither opens a console window, and both still enter through a
  plain `main`. Their output goes wherever the parent redirected it (pipes, files). Started
  from a terminal without redirection, they attach to that terminal. `cuervo.exe` stays a
  console program for development.

## Made portable

| Area | Before | Now |
|---|---|---|
| Environment variables | `std::getenv` (deprecated under MSVC, C4996) | `platform::env` / `env_is` (`_dupenv_s` on MSVC) |
| Script hot reload | a watcher inside the game runner | `platform::FileWatcher`: mtime polling plus a file count, the same code on Linux and Windows, off on the web |
| Paths in messages and sound names | `path.string()` (ANSI code page) | `platform::utf8(path)` / `platform::path_from_utf8(text)` |
| Log file names | trimmed only at forward-slash paths | also backslash paths (`__FILE__` under MSVC/MinGW) |
| Frame loop | blocking `while` in the runner | `platform::run_main_loop` |
| GL entry points | `__stdcall` handled (`CUERVO_GL_APIENTRY`) | unchanged |
| Embedded text (preludes, shaders, `std/math.plm`) | one raw string literal per file | adjacent literals of at most 8 KB (`cuervo_raw_literal`, shader_tool): MSVC rejects a literal over 16 KB (C2026) |
| Main thread stack | 1 MB (MSVC) or 2 MB (MinGW) | 8 MB on every toolchain, as on Linux |
| MinGW-w64 runtime DLLs | `libstdc++-6.dll` etc. found on `PATH` | linked statically (`-static`), in programs and AOT modules: MinGW players need only system DLLs |
| AOT modules under MinGW-w64 | link.exe flags | GNU ld flags, with a fixed image base: ld otherwise bases a DLL at a hash of its (per-process temp) path, and two exports differed |
| AVX2 code under MinGW-w64 GCC | Box2D's AVX2 solver crashed (GCC bug 54412: unaligned 32-byte stack spills on Win64) | `-Wa,-muse-unaligned-vector-move` for Box2D (and for the engine with `CUERVO_SIMD=avx2`); same results |
| Visual C++ runtime | needed on the player's machine | `export --platform windows` ships the `vcruntime140*.dll` / `msvcp140.dll` an MSVC player imports next to `<game>.exe` (app-local; from beside the player or the system directory) |
| Static plugin libraries (AOT) | `lib<name>.a` | `windows_static` (MSVC runtime: `<name>_static.lib` for C++, `<name>.lib` for Rust) and `windows_gnu_static` (MinGW-w64: `lib<name>_gnu.a`) in package.toml. AOT modules also link the Windows system libraries that Rust's std needs |
| Stripped players | refused: `run_process` was POSIX-only | `CreateProcessW` in a job object (a timeout ends the whole build tree), `LockFileEx` for the cache entry, cache in `%LOCALAPPDATA%\cuervo\players`, the `player` target built |
| `@extern("", ...)` | the `.exe`'s own exports only | every loaded module, as `dlopen(NULL)` gives on Linux (LANGUAGE.md 9.28) |

## Validation results

Windows 11. The suites ran with GL and Vulkan on the Intel Arc. A later MSVC Release run of
`rhi_parity` and `lantern_cave`, with the RTX 4070 enabled, put Vulkan on the NVIDIA GPU
(GL stays on the Intel Arc, Windows' default for GL). All 14 RHI scenes match, and the shmup
and lit cave reach the same state hashes on all three paths. The lit cave differs from GL on
1 interior pixel, inside the allowance below.

| Check | MSVC 19.44 (`msvc-release`) | MinGW-w64 GCC 16.2 (Debug) | Clang 23 + MSVC runtime (`debug`) |
|---|---|---|---|
| Clean build, first-party warnings | 0 | 0 | 0 |
| `ctest` | 47/47 | 47/47 | 47/47 |
| Shmup state hash at tick 240 (GL and Vulkan) | `a3e57e79964421d3` | `a3e57e79964421d3` | `a3e57e79964421d3` |
| Lit cave (`lantern_cave`) state hash and frame | `2dd8beccd71bc67e`, byte-identical frame | same | same |
| `export_shmup`, `export_tutorial` (source and AOT, run from another directory) | pass | pass | pass |

MSVC Debug (`cl` 19.44 Debug) also builds clean with zero warnings and passes 47/47, in about
40 minutes against 12 for Release (`cuervo_tests` alone takes about 7). Its slow Debug STL is
why CI tests MSVC in Release.

Problems these runs found, all fixed (see "Made portable"):

- **Box2D's AVX2 solver crashed under MinGW GCC** (GCC bug 54412).
- **MinGW AOT modules were not reproducible**: two exports differed, because ld bases a DLL
  at a hash of its path.
- **MinGW players needed MinGW's DLLs** on `PATH`, and MSVC players needed the Visual C++
  runtime installed.
- **Static plugin libraries were per toolchain**: a MinGW build linked the MSVC
  `orbit_static.lib`.
- **`@extern("")` found nothing on Windows**, because a PE exports only its own symbols.
- **Intel Arc's GL and Vulkan drivers differ on 38 of 921,600 pixels** of the lit cave,
  along 32 px tile seams (max channel difference 13, same state hash), and Intel GL
  against NVIDIA Vulkan on 1. The GL-vs-Vulkan lit comparison in `rhi_parity` allows
  0.01 % of pixels. NVIDIA GL and Vulkan match exactly on Linux.
- **The test timeouts were too short** for MinGW Debug's AOT builds (GCC compiles the
  generated C in about a minute) and for MSVC Debug.
- **Earlier, with the Clang Debug build**: capture runs read the real mouse, and three
  tests forced SDL's offscreen driver, which Windows lacks.

Tests that open windows stall if the display sleeps or the session locks during a run;
keep the machine awake for long suites.

## Checklist (all checked)

1. **Compilers.** MSVC (`/W4 /permissive- /fp:precise /utf-8`), MinGW-w64 GCC and Clang
   build with zero warnings in first-party code. Third-party code is built with its warnings
   off, or with the one SDL trips (C5286) disabled.
2. **Determinism flags.** `/fp:precise` doesn't contract to FMA (VS 2022 makes
   `/fp:contract` opt-in), and GCC/Clang use `-ffp-contract=off`. `platform_tests`
   checks it on every toolchain: `a * b + c` with an exactly representable 2⁻⁵⁴ product
   error must round twice. The shmup state hash at tick 240 is `a3e57e79964421d3` on every
   Windows toolchain, as on Linux and the web.
3. **Dependencies.** SDL3 is fetched and built, and FreeType is fetched when not found.
4. **Console output.** UTF-8, see above. `platform_tests` checks the code page and that a
   UTF-8 file name reaches the file system intact.
5. **Subsystem.** GUI-subsystem player and editor, see above.
6. **Hot reload with editors that lock files.** `read_scripts` may fail once, and the next
   poll retries.
7. **High-DPI.** `platform_tests` opens a window on the real desktop at 150 % and warps the
   cursor. It checks that the framebuffer is the window size times the pixel density and that
   the pointer position the game sees is the pixel under the cursor (Win32
   `GetCursorPos` in physical pixels).
8. **Export (docs/EXPORT.md).** On a Windows host, `export --platform windows` ships the
   build tree's `cuervo-player.exe`. From a prebuilt package or another host, it ships
   `templates/windows/cuervo-player.exe`. `export_shmup` and `export_tutorial` run the
   exported games from another directory and reach their dev state hashes with bytecode
   and AOT scripts. The release workflow repeats this from the unpacked archive.
   Stripped players (`--engine stripped`, and `auto` when a toolchain is there) build on a
   Windows host with the tools' own compiler; MSVC needs `cuervo-tools` run from a VS developer
   prompt. `export_stripped_tictak` (with `CUERVO_SLOW_TESTS=ON`) passes with MSVC and MinGW-w64:
   the stripped tictak reaches `0b1d5dd886e8c072`, the full player's and Linux's hash. Its MSVC
   player is 4,807,680 bytes against the full Release player's 6,965,248 (-31 %).

## Prebuilt binaries

`.github/workflows/release.yml` builds MSVC Release binaries on a `v*` tag. It packs
`cuervo-windows-x86_64.zip` (`bin/`: cuervo, cuervo-editor, cuervo-tools, plm, and
`templates/windows/cuervo-player.exe`) and `cuervo-export-templates.zip` (the Linux and
Windows players). Every release file gets a `SHA256SUMS.txt` line and, while
the repository is public, a Sigstore build attestation (GitHub offers none for private
repositories owned by a user). When the repository has the `WINDOWS_SIGNING_CERT` (a base64 .pfx)
and `WINDOWS_SIGNING_PASSWORD` secrets, the executables are also Authenticode-signed
(SHA-256, RFC 3161 timestamp) before packing. Without a certificate the binaries are
attested but unsigned, and SmartScreen warns on first run.

Commands in `cmd.exe` take backslash paths
(`build\msvc-release\runtime\cuervo --game examples\shmup ...`).
