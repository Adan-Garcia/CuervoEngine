include(FetchContent)

# --- SDL3 -------------------------------------------------------------------
# Prefer a system install (e.g. Arch's `sdl3` package); otherwise build the
# pinned release from source.
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
    message(STATUS "Using system SDL3 ${SDL3_VERSION}")
else()
    message(STATUS "System SDL3 not found, fetching release-3.2.10")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.10
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(SDL3)
    # Third-party code: its warnings aren't ours to fix (zero-warning rule).
    if(TARGET SDL3-static AND NOT MSVC)
        target_compile_options(SDL3-static PRIVATE -w)
    elseif(TARGET SDL3-static)
        # SDL sets /W3 itself, and /w on top of it would warn (D9025): drop the one it trips.
        target_compile_options(SDL3-static PRIVATE /wd5286)
    endif()
endif()

# --- doctest ----------------------------------------------------------------
# Only the single header is needed; fetching it directly avoids doctest's own
# CMake project (which targets very old CMake versions).
if(CUERVO_BUILD_TESTS)
    FetchContent_Declare(doctest
        URL https://raw.githubusercontent.com/doctest/doctest/v2.4.12/doctest/doctest.h
        DOWNLOAD_NO_EXTRACT TRUE
    )
    FetchContent_MakeAvailable(doctest)
    add_library(doctest INTERFACE)
    target_include_directories(doctest SYSTEM INTERFACE "${doctest_SOURCE_DIR}")
    target_compile_definitions(doctest INTERFACE DOCTEST_CONFIG_USE_STD_HEADERS)
    # doctest 2.4.12 includes <ciso646>, which libstdc++ 16 flags with a
    # #warning under clang in C++20 mode.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        target_compile_options(doctest INTERFACE -Wno-\#warnings)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
        target_compile_options(doctest INTERFACE -Wno-\#warnings)
    endif()
endif()

# --- Vulkan (headers, volk loader, VMA) --------------------------------------
# Only headers + sources are fetched; nothing links against a Vulkan SDK.
# volk loads entry points at runtime through SDL, so a machine without Vulkan
# still runs the OpenGL backend. Version matches Arch's vulkan-icd-loader.
if(CUERVO_VULKAN)
    set(CUERVO_VULKAN_SDK_TAG vulkan-sdk-1.4.363.0)

    # SOURCE_SUBDIR points at a missing directory so MakeAvailable only
    # downloads; we build what we need ourselves below.
    FetchContent_Declare(vulkan_headers
        URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/${CUERVO_VULKAN_SDK_TAG}.tar.gz
        SOURCE_SUBDIR _none
    )
    FetchContent_Declare(volk
        URL https://github.com/zeux/volk/archive/refs/tags/${CUERVO_VULKAN_SDK_TAG}.tar.gz
        SOURCE_SUBDIR _none
    )
    FetchContent_Declare(vma
        URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz
        SOURCE_SUBDIR _none
    )
    FetchContent_MakeAvailable(vulkan_headers volk vma)

    # Third-party code: no project warnings, included as SYSTEM headers.
    add_library(vulkan_deps STATIC
        "${volk_SOURCE_DIR}/volk.c"
        "${PROJECT_SOURCE_DIR}/cuervo/rhi/vulkan/vma_impl.cpp"
    )
    target_include_directories(vulkan_deps SYSTEM PUBLIC
        "${vulkan_headers_SOURCE_DIR}/include"
        "${volk_SOURCE_DIR}"
        "${vma_SOURCE_DIR}/include"
    )
    target_compile_definitions(vulkan_deps PUBLIC VK_NO_PROTOTYPES)
    set_target_properties(vulkan_deps PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(vulkan_deps PRIVATE ${CMAKE_DL_LIBS})
endif()

# --- WebGPU (wgpu-native; the web build uses Emscripten's emdawnwebgpu port) ---
# Milestone 10.1 (DESIGN.md 4.4): native WebGPU is wgpu-native, the same
# wgpu/naga 29 release line as the pinned naga-cli, so the WGSL the build
# generates is parsed by the naga it came from. Prebuilt release archives,
# pinned by tag and hash; linked statically.
if(CUERVO_WEBGPU)
    execute_process(COMMAND "${CUERVO_NAGA}" --version OUTPUT_VARIABLE _naga_version
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT CUERVO_NAGA OR NOT CUERVO_SPIRV_OPT OR NOT _naga_version STREQUAL CUERVO_NAGA_VERSION)
        message(FATAL_ERROR
            "CUERVO_WEBGPU needs naga-cli ${CUERVO_NAGA_VERSION} and spirv-opt (found naga: "
            "'${CUERVO_NAGA}' ${_naga_version}, spirv-opt: '${CUERVO_SPIRV_OPT}').\n"
            "Install them with `cargo install naga-cli --version ${CUERVO_NAGA_VERSION} --locked` and "
            "SPIRV-Tools (Arch: pacman -S spirv-tools), or configure with -DCUERVO_WEBGPU=OFF.")
    endif()
endif()
if(CUERVO_WEBGPU AND NOT EMSCRIPTEN)
    set(CUERVO_WGPU_NATIVE_TAG v29.0.1.1)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(_wgpu_archive wgpu-linux-x86_64-release.zip)
        set(_wgpu_hash 95a4d90c071005a98d03eab348beaa6b07e16eb00d1dcdb9f8348f75eb97ec5a)
        set(_wgpu_lib lib/libwgpu_native.a)
        set(_wgpu_system_libs ${CMAKE_DL_LIBS} pthread m)
    elseif(WIN32 AND MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        # Not validated yet (milestone 10.5a).
        set(_wgpu_archive wgpu-windows-x86_64-msvc-release.zip)
        set(_wgpu_hash 7e67d7445c42aeb85e30f88930fd8d7d83ee769e3390aeb1ada75ebf3cf78132)
        set(_wgpu_lib lib/wgpu_native.lib)
        set(_wgpu_system_libs d3dcompiler ws2_32 userenv bcrypt ntdll opengl32 propsys runtimeobject)
    else()
        message(FATAL_ERROR "CUERVO_WEBGPU: no pinned wgpu-native build for ${CMAKE_SYSTEM_NAME} "
            "${CMAKE_SYSTEM_PROCESSOR}; configure with -DCUERVO_WEBGPU=OFF")
    endif()
    FetchContent_Declare(wgpu_native
        URL https://github.com/gfx-rs/wgpu-native/releases/download/${CUERVO_WGPU_NATIVE_TAG}/${_wgpu_archive}
        URL_HASH SHA256=${_wgpu_hash}
        SOURCE_SUBDIR _none
    )
    FetchContent_MakeAvailable(wgpu_native)
    add_library(wgpu_native STATIC IMPORTED GLOBAL)
    set_target_properties(wgpu_native PROPERTIES
        IMPORTED_LOCATION "${wgpu_native_SOURCE_DIR}/${_wgpu_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${wgpu_native_SOURCE_DIR}/include" # SYSTEM: imported
        INTERFACE_LINK_LIBRARIES "${_wgpu_system_libs}")
endif()

# --- SPIRV-Cross (build-time shader tool only) --------------------------------
# Cross-compiles the SPIR-V that glslangValidator produces into GLSL 330 core and
# GLSL ES 300 for the GL backend (DESIGN.md 4.4). Only the host tool
# `shader_tool` links it; nothing ships in the engine binary. Same SDK tag as the
# Vulkan headers. Like volk/VMA we compile the handful of sources ourselves
# instead of running SPIRV-Cross's own CMake project.
# Cross builds (web) take a prebuilt host shader_tool (cmake/Shaders.cmake), so
# SPIRV-Cross is only fetched for native builds.
if(NOT CMAKE_CROSSCOMPILING)
FetchContent_Declare(spirv_cross
    URL https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
    SOURCE_SUBDIR _none
)
FetchContent_MakeAvailable(spirv_cross)

add_library(spirv_cross_glsl STATIC
    "${spirv_cross_SOURCE_DIR}/spirv_cross.cpp"
    "${spirv_cross_SOURCE_DIR}/spirv_parser.cpp"
    "${spirv_cross_SOURCE_DIR}/spirv_cross_parsed_ir.cpp"
    "${spirv_cross_SOURCE_DIR}/spirv_cfg.cpp"
    "${spirv_cross_SOURCE_DIR}/spirv_glsl.cpp"
)
target_include_directories(spirv_cross_glsl SYSTEM PUBLIC "${spirv_cross_SOURCE_DIR}")
# Third-party code: silence its warnings so ours stay at zero.
if(NOT MSVC)
    target_compile_options(spirv_cross_glsl PRIVATE -w)
endif()
endif()

# --- libtess2 (fill tessellation for SVG / vector meshes) ---------------------
# SGI Free Software License B (permissive). Polygon tessellator with nonzero /
# evenodd / abs>=2 winding rules and boundary-contour output, used by
# cuervo/vector for fills, stroke outline unions and clip-path intersection
# (DESIGN.md 4.5). Compiled as third-party code: no project warnings.
FetchContent_Declare(libtess2
    # Pinned to a master commit (2024+), not the v1.0.2 tag: master carries
    # sweep robustness fixes (libtess2 issue #22 and coincident-edge handling)
    # without which unions of stroke pieces meeting at T-junctions drop regions.
    URL https://github.com/memononen/libtess2/archive/8dbd6483e920311a58c9af10a10beb278efebc36.tar.gz
    SOURCE_SUBDIR _none
)
FetchContent_MakeAvailable(libtess2)

add_library(libtess2 STATIC
    "${libtess2_SOURCE_DIR}/Source/bucketalloc.c"
    "${libtess2_SOURCE_DIR}/Source/dict.c"
    "${libtess2_SOURCE_DIR}/Source/geom.c"
    "${libtess2_SOURCE_DIR}/Source/mesh.c"
    "${libtess2_SOURCE_DIR}/Source/priorityq.c"
    "${libtess2_SOURCE_DIR}/Source/sweep.c"
    "${libtess2_SOURCE_DIR}/Source/tess.c"
)
target_include_directories(libtess2 SYSTEM PUBLIC "${libtess2_SOURCE_DIR}/Include")
set_target_properties(libtess2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(libtess2 PRIVATE -w)
endif()

# --- Fonts: FreeType + msdfgen + msdf-atlas-gen (font import only) -------------
# TTF/OTF -> MSDF atlas import (DESIGN.md 7.2 "Fonts", 4.11). Only the importer
# (cuervo/render/font_import.cpp) links these; shipping builds can turn import
# off and load prebuilt .efont atlases only, like CUERVO_SVG_IMPORT.
option(CUERVO_FONT_IMPORT "Allow importing .ttf/.otf fonts at runtime (off for shipping builds)" ON)

if(CUERVO_FONT_IMPORT)
    # FreeType (FreeType License / GPLv2, we use the FTL): prefer the system
    # package; otherwise build the pinned release with every optional codec off.
    option(CUERVO_FETCH_FREETYPE "Always build FreeType from source instead of using a system install" OFF)
    if(NOT CUERVO_FETCH_FREETYPE)
        find_package(Freetype QUIET)
    endif()
    if(FREETYPE_FOUND)
        message(STATUS "Using system FreeType ${FREETYPE_VERSION_STRING}")
    else()
        message(STATUS "System FreeType not used, fetching VER-2-13-3")
        set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
        set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
        set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
        set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
        set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
        set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
        # FreeType's ftver.rc includes <windows.h>. rc.exe finds it through the
        # developer prompt's INCLUDE and windres has MinGW's own; only clang's
        # llvm-rc (Clang targeting the MSVC runtime, the `debug` preset) needs
        # the SDK path.
        if(WIN32 AND NOT MSVC AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
            find_path(WINDOWS_SDK_UM_INCLUDE "windows.h"
                PATHS
                "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/um"
                "C:/Program Files (x86)/Windows Kits/10/Include/10.0.22621.0/um"
                "C:/Program Files (x86)/Windows Kits/10/Include/10.0.22000.0/um"
                "C:/Program Files (x86)/Windows Kits/10/Include/10.0.19041.0/um"
            )
            if(WINDOWS_SDK_UM_INCLUDE)
                cmake_path(GET WINDOWS_SDK_UM_INCLUDE PARENT_PATH _sdk_inc_root)
                # Not cached: appending to the cache entry grew it on every reconfigure.
                string(APPEND CMAKE_RC_FLAGS " -I \"${WINDOWS_SDK_UM_INCLUDE}\" -I \"${_sdk_inc_root}/shared\"")
            endif()
        endif()
        FetchContent_Declare(freetype
            URL https://gitlab.freedesktop.org/freetype/freetype/-/archive/VER-2-13-3/freetype-VER-2-13-3.tar.gz
        )
        # FreeType 2.13.3 declares cmake_minimum_required(2.8.12...); CMake 4
        # warns about (and will drop) < 3.10 compatibility. Scoped to this fetch.
        set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
        FetchContent_MakeAvailable(freetype)
        unset(CMAKE_POLICY_VERSION_MINIMUM)
        if(NOT MSVC)
            target_compile_options(freetype PRIVATE -w)
        endif()
        set_target_properties(freetype PROPERTIES POSITION_INDEPENDENT_CODE ON)
        add_library(Freetype::Freetype ALIAS freetype)
    endif()

    # msdfgen (MIT): distance field generation. msdf-atlas-gen (MIT): glyph
    # geometry, kerning, atlas packing and parallel generation. v1.3 of the
    # atlas generator is built against msdfgen v1.12 (its pinned submodule).
    # Like volk/VMA we compile the needed sources ourselves: no PNG/SVG/Skia,
    # no artery-font, no CLI.
    FetchContent_Declare(msdfgen
        URL https://github.com/Chlumsky/msdfgen/archive/refs/tags/v1.12.tar.gz
        SOURCE_SUBDIR _none
    )
    FetchContent_Declare(msdf_atlas_gen
        URL https://github.com/Chlumsky/msdf-atlas-gen/archive/refs/tags/v1.3.tar.gz
        SOURCE_SUBDIR _none
    )
    FetchContent_MakeAvailable(msdfgen msdf_atlas_gen)

    find_package(Threads REQUIRED)
    file(GLOB _msdfgen_core_sources "${msdfgen_SOURCE_DIR}/core/*.cpp")
    set(_msdf_atlas_dir "${msdf_atlas_gen_SOURCE_DIR}/msdf-atlas-gen")
    add_library(msdf_atlas STATIC
        ${_msdfgen_core_sources}
        "${msdfgen_SOURCE_DIR}/ext/import-font.cpp"
        "${_msdf_atlas_dir}/bitmap-blit.cpp"
        "${_msdf_atlas_dir}/Charset.cpp"
        "${_msdf_atlas_dir}/charset-parser.cpp"
        "${_msdf_atlas_dir}/FontGeometry.cpp"
        "${_msdf_atlas_dir}/glyph-generators.cpp"
        "${_msdf_atlas_dir}/GlyphGeometry.cpp"
        "${_msdf_atlas_dir}/GridAtlasPacker.cpp"
        "${_msdf_atlas_dir}/Padding.cpp"
        "${_msdf_atlas_dir}/RectanglePacker.cpp"
        "${_msdf_atlas_dir}/size-selectors.cpp"
        "${_msdf_atlas_dir}/TightAtlasPacker.cpp"
        "${_msdf_atlas_dir}/utf8.cpp"
        "${_msdf_atlas_dir}/Workload.cpp"
    )
    target_include_directories(msdf_atlas SYSTEM PUBLIC
        "${msdfgen_SOURCE_DIR}"
        "${msdf_atlas_gen_SOURCE_DIR}"
    )
    # Static library: the export macros are empty, which also skips msdfgen's
    # CMake-generated config header.
    target_compile_definitions(msdf_atlas PUBLIC
        MSDFGEN_PUBLIC= MSDFGEN_EXT_PUBLIC= MSDF_ATLAS_PUBLIC= MSDF_ATLAS_NO_ARTERY_FONT)
    target_link_libraries(msdf_atlas PUBLIC Freetype::Freetype Threads::Threads)
    set_target_properties(msdf_atlas PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(NOT MSVC)
        # Optimized even in Debug builds: unoptimized msdfgen makes importing a
        # font at startup take seconds, and nobody steps through it.
        target_compile_options(msdf_atlas PRIVATE -w -O2)
    endif()
endif()

# --- Text shaping and bidi (HarfBuzz + SheenBidi, DESIGN.md 4.19 "11.3") ----------
# HarfBuzz (Old MIT): complex-script shaping; its single-file build
# (src/harfbuzz.cc) has no dependencies, so it also builds under Emscripten.
# SheenBidi (Apache-2.0): the Unicode Bidirectional Algorithm and script runs;
# chosen over FriBidi (LGPL) so the static link needs no relinking exception.
# Optional: with CUERVO_TEXT_SHAPING=OFF text keeps the per-codepoint layout.
option(CUERVO_TEXT_SHAPING "Shape and reorder text with HarfBuzz and SheenBidi" ON)
if(CUERVO_TEXT_SHAPING)
    FetchContent_Declare(harfbuzz
        URL https://github.com/harfbuzz/harfbuzz/releases/download/12.3.2/harfbuzz-12.3.2.tar.xz
        URL_HASH SHA256=6f6db164359a2da5a84ef826615b448b33e6306067ad829d85d5b0bf936f1bb8
        SOURCE_SUBDIR _none
    )
    FetchContent_Declare(sheenbidi
        URL https://github.com/Tehreer/SheenBidi/archive/refs/tags/v3.0.0.tar.gz
        URL_HASH SHA256=86c56014034739ba39a24c23eb00323b0bf6f737354f665786015fca842af786
        SOURCE_SUBDIR _none
    )
    FetchContent_MakeAvailable(harfbuzz sheenbidi)

    add_library(harfbuzz STATIC "${harfbuzz_SOURCE_DIR}/src/harfbuzz.cc")
    target_include_directories(harfbuzz SYSTEM PUBLIC "${harfbuzz_SOURCE_DIR}/src")
    # No FreeType/ICU/GLib glue: fonts are read from memory with hb_ot only.
    target_compile_definitions(harfbuzz PUBLIC HB_NO_PRAGMA_GCC_DIAGNOSTIC_ERROR=1)
    set_target_properties(harfbuzz PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(NOT MSVC)
        target_compile_options(harfbuzz PRIVATE -w -O2 -fno-exceptions -fno-rtti)
    endif()

    add_library(sheenbidi STATIC "${sheenbidi_SOURCE_DIR}/Source/SheenBidi.c")
    target_include_directories(sheenbidi SYSTEM PUBLIC "${sheenbidi_SOURCE_DIR}/Headers")
    target_include_directories(sheenbidi PRIVATE "${sheenbidi_SOURCE_DIR}/Source")
    target_compile_definitions(sheenbidi PRIVATE SB_CONFIG_UNITY)
    set_target_properties(sheenbidi PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(NOT MSVC)
        target_compile_options(sheenbidi PRIVATE -w -O2)
    endif()

    # Test font for shaping goldens (SIL OFL 1.1, pinned by commit and content
    # hash). Tests only; exported games bring their own fonts.
    if(CUERVO_BUILD_TESTS)
        FetchContent_Declare(noto_sans_arabic
            URL https://raw.githubusercontent.com/notofonts/notofonts.github.io/e2ec4a11da38a23b1ecebfcb233833a153ecb841/fonts/NotoSansArabic/hinted/ttf/NotoSansArabic-Regular.ttf
            URL_HASH SHA256=bdff3e5659d67e67def05b33f749683b9376ae819d65d3dd62ac4640b3aaef48
            DOWNLOAD_NO_EXTRACT TRUE
        )
        FetchContent_MakeAvailable(noto_sans_arabic)
        set(CUERVO_TEST_FONT_ARABIC "${noto_sans_arabic_SOURCE_DIR}/NotoSansArabic-Regular.ttf")

        # Japanese for the fallback chain (Latin -> Arabic -> Japanese) tests and the
        # EN/AR/JA UI parity test: M+ 1p (SIL OFL 1.1), pinned by the google/fonts
        # commit and the content hash. Tests only.
        FetchContent_Declare(mplus1p
            URL https://raw.githubusercontent.com/google/fonts/6c1dbe27dcf7d817d4b62cec033ba77fa8e2a5ec/ofl/mplus1p/MPLUS1p-Regular.ttf
            URL_HASH SHA256=2f294ad496432b1608f070d310e3aa2adcf1de4af429f4901df97ec4bd361ed1
            DOWNLOAD_NO_EXTRACT TRUE
        )
        FetchContent_MakeAvailable(mplus1p)
        set(CUERVO_TEST_FONT_JAPANESE "${mplus1p_SOURCE_DIR}/MPLUS1p-Regular.ttf")
    endif()
endif()

# --- miniaudio (audio decoding + output device) -------------------------------
# MIT-0 / public domain (DESIGN.md 3, 4.9, 7.2 "Audio formats"). Decoders for
# WAV, FLAC, MP3 (built in) and OGG Vorbis (stb_vorbis from miniaudio's
# extras), plus the playback device (PulseAudio/ALSA/JACK loaded at runtime on
# Linux). Only cuervo/audio sees it. The engine mixes itself
# (cuervo/audio/mixer.hpp), so miniaudio's engine/node graph is compiled out.
if(CUERVO_FEATURE_AUDIO)
    FetchContent_Declare(miniaudio
        URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.25.tar.gz
        URL_HASH SHA256=b900edcffe979816e2560a0580b9b1216d674b4f17fbadeca8f777a7f8ab0274
        SOURCE_SUBDIR _none
    )
    FetchContent_MakeAvailable(miniaudio)

    add_library(miniaudio STATIC "${PROJECT_SOURCE_DIR}/cuervo/audio/miniaudio_impl.c")
    target_include_directories(miniaudio SYSTEM PUBLIC "${miniaudio_SOURCE_DIR}")
    target_compile_definitions(miniaudio PUBLIC
        MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION MA_NO_ENCODING)
    set_target_properties(miniaudio PROPERTIES POSITION_INDEPENDENT_CODE ON)
    find_package(Threads REQUIRED)
    target_link_libraries(miniaudio PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(NOT MSVC)
        # Optimized even in Debug builds: unoptimized MP3/Vorbis decoding makes
        # loading a music track noticeably slow.
        target_compile_options(miniaudio PRIVATE -w -O2)
    endif()
    if(NOT WIN32)
        target_link_libraries(miniaudio PRIVATE m)
    endif()
endif()

# --- Mbed TLS (wss:// and https:// on desktop, DESIGN.md 4.19, milestone 11.1) -
# Apache-2.0 OR GPL-2.0-or-later (we take Apache-2.0). Small, portable C that builds the same on
# Linux, MSVC and MinGW. The web uses the browser's WebSocket/fetch, so wasm never links a TLS
# library. Release tarball pinned by tag and hash; only the library sources are compiled (no
# programs, no tests, no net_sockets: cuervo/net/tcp.cpp owns the sockets).
option(CUERVO_TLS "Build TLS (wss://, https://) into the desktop networking layer with Mbed TLS" ON)
if(CUERVO_TLS AND NOT EMSCRIPTEN)
    FetchContent_Declare(mbedtls
        URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.5/mbedtls-3.6.5.tar.bz2
        URL_HASH SHA256=4a11f1777bb95bf4ad96721cac945a26e04bf19f57d905f241fe77ebeddf46d8
        SOURCE_SUBDIR _none
    )
    FetchContent_MakeAvailable(mbedtls)
    file(GLOB _mbedtls_sources CONFIGURE_DEPENDS "${mbedtls_SOURCE_DIR}/library/*.c")
    list(FILTER _mbedtls_sources EXCLUDE REGEX "net_sockets\\.c$")
    add_library(mbedtls_lib STATIC ${_mbedtls_sources})
    target_include_directories(mbedtls_lib SYSTEM PUBLIC "${mbedtls_SOURCE_DIR}/include")
    target_include_directories(mbedtls_lib PRIVATE "${mbedtls_SOURCE_DIR}/library")
    set_target_properties(mbedtls_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(mbedtls_lib PRIVATE /wd4244 /wd4267 /wd4996 /wd4146 /wd4018 /wd4310
            /wd4127 /wd4389 /wd4701 /wd4703 /wd4090 /wd4132 /wd4100 /wd4702 /wd4706)
    else()
        target_compile_options(mbedtls_lib PRIVATE -w)
    endif()
    if(WIN32)
        target_link_libraries(mbedtls_lib PUBLIC bcrypt crypt32) # entropy + the Windows root store
    endif()
endif()

# --- stb_image (PNG/JPEG decoding for raster import, DESIGN.md 4.19 "11.2a") --
# Public domain / MIT. stb has no release tags, so the header is pinned by commit
# and content hash. Only cuervo/assets sees it (import-time decoding; shipping
# builds read the imported CTEX). QOI has its own decoder, WebP is deferred.
FetchContent_Declare(stb_image
    URL https://raw.githubusercontent.com/nothings/stb/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5/stb_image.h
    URL_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(stb_image)
add_library(stb_image STATIC "${PROJECT_SOURCE_DIR}/cuervo/assets/stb_image_impl.c")
target_include_directories(stb_image SYSTEM PUBLIC "${stb_image_SOURCE_DIR}")
set_target_properties(stb_image PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    # Third-party code, optimized even in Debug (decoding large sheets).
    target_compile_options(stb_image PRIVATE -w -O2)
endif()

# --- libwebp decoder (WebP import, DESIGN.md 4.19 "11.2a") -------------------
# BSD-3-Clause. Only the decoder subset (src/dec, the decode half of src/dsp and
# src/utils) is built: lossy VP8, lossless VP8L and alpha. Import-time only, like
# stb_image. Optional: with CUERVO_WEBP=OFF (or no network) WebP import reports
# that it is unsupported and everything else is unchanged.
option(CUERVO_WEBP "Decode WebP images on import with libwebp" ON)
if(CUERVO_WEBP)
    FetchContent_Declare(libwebp
        URL https://github.com/webmproject/libwebp/archive/refs/tags/v1.5.0.tar.gz
        URL_HASH SHA256=668c9aba45565e24c27e17f7aaf7060a399f7f31dba6c97a044e1feacb930f37
        SOURCE_SUBDIR _none
    )
    FetchContent_MakeAvailable(libwebp)
    set(_webp "${libwebp_SOURCE_DIR}/src")
    file(GLOB _webp_srcs "${_webp}/dec/*.c" "${_webp}/dsp/*.c" "${_webp}/utils/*.c")
    list(FILTER _webp_srcs EXCLUDE REGEX "(_enc|/enc|/cost|/ssim|bit_writer|huffman_encode|quant_levels_utils|filters_utils)")
    add_library(webp_decoder STATIC ${_webp_srcs})
    target_include_directories(webp_decoder SYSTEM PUBLIC "${libwebp_SOURCE_DIR}/src")
    target_include_directories(webp_decoder PRIVATE "${libwebp_SOURCE_DIR}")
    set_target_properties(webp_decoder PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
    if(NOT MSVC)
        target_compile_options(webp_decoder PRIVATE -w -O2)
    endif()
    if(NOT WIN32)
        target_link_libraries(webp_decoder PRIVATE m)
    endif()
endif()

# --- Box2D (2D rigid-body physics, DESIGN.md 4.17, 7.2, milestone 9.6) -------
if(CUERVO_FEATURE_PHYSICS)
    set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
    set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(BOX2D_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(BOX2D_DOCS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(box2d
        URL https://github.com/erincatto/box2d/archive/refs/tags/v3.2.0.tar.gz
        URL_HASH SHA256=ffec747b27a2267016f8d0240d658c82d187fb2271fa0f69fcbe53fbc067180d
    )
    FetchContent_MakeAvailable(box2d)
    if(TARGET box2d AND NOT MSVC)
        target_compile_options(box2d PRIVATE -w)
    endif()
    if(TARGET box2d AND MINGW AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
        # Box2D picks its AVX2 solver at run time. GCC on Win64 doesn't align the
        # stack for 32-byte spills (GCC bug 54412), so its vmovaps faults; have the
        # assembler emit unaligned moves instead (same results, docs/WINDOWS.md).
        target_compile_options(box2d PRIVATE -Wa,-muse-unaligned-vector-move)
    endif()
    if(TARGET box2d)
        set_target_properties(box2d PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endif()

# --- Tracy (optional profiler bridge, DESIGN.md 7.2 "Profiling") --------------
# BSD-3. Only with -DCUERVO_TRACY=ON: engine zones (cuervo/core/profiler.hpp)
# and frame marks also go to Tracy. On demand: nothing is collected until the
# Tracy viewer connects. The client is one translation unit, compiled here.
if(CUERVO_TRACY)
    FetchContent_Declare(tracy
        URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.11.1.tar.gz
        URL_HASH SHA256=2c11ca816f2b756be2730f86b0092920419f3dabc7a7173829ffd897d91888a1
        SOURCE_SUBDIR _none
    )
    FetchContent_MakeAvailable(tracy)
    add_library(tracy_client STATIC "${tracy_SOURCE_DIR}/public/TracyClient.cpp")
    target_include_directories(tracy_client SYSTEM PUBLIC "${tracy_SOURCE_DIR}/public")
    target_compile_definitions(tracy_client PUBLIC TRACY_ENABLE TRACY_ON_DEMAND)
    set_target_properties(tracy_client PROPERTIES POSITION_INDEPENDENT_CODE ON)
    find_package(Threads REQUIRED)
    target_link_libraries(tracy_client PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    if(NOT MSVC)
        target_compile_options(tracy_client PRIVATE -w)
    endif()
endif()

# --- Dear ImGui (the editor's UI, DESIGN.md 4.13) ------------------------------
# MIT. Only the editor (editor/, never the web build) links it. Pinned to the
# last 1.92 release: 1.93.0 shipped two days before Phase 6A with a reworked
# ImDrawList renderer, and nothing here needs it. Not the docking branch: the
# Godot-like layout (DESIGN.md 7.2) is a fixed arrangement of panels. Rendering
# goes through our RHI (editor/imgui_rhi.cpp), so only the core and the SDL3
# platform backend (input, clipboard, cursors) are compiled.
# The editor needs every engine feature. A stripped configuration hides the
# option (OFF) without overwriting the cached choice, so re-enabling the
# features brings the editor back.
include(CMakeDependentOption)
cmake_dependent_option(CUERVO_BUILD_EDITOR "Build the editor (cuervo-editor)" ON
    "CUERVO_FEATURE_DEV;CUERVO_FEATURE_UI;CUERVO_FEATURE_PARTICLES;CUERVO_FEATURE_COLLISION;CUERVO_FEATURE_AUDIO" OFF)
# The ImGui editor (`cuervo-editor --imgui`) is deprecated and will be removed (DESIGN.md 4.13);
# the RmlUi editor is the default and needs none of it. OFF: Dear ImGui is not fetched or built
# and the ImGui-only editor sources, shaders and tests are left out.
cmake_dependent_option(CUERVO_EDITOR_IMGUI "Build the deprecated ImGui editor (cuervo-editor --imgui)" ON
    "CUERVO_BUILD_EDITOR" OFF)
if(CUERVO_BUILD_EDITOR AND NOT EMSCRIPTEN)
  if(CUERVO_EDITOR_IMGUI)
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b-docking.tar.gz
        URL_HASH SHA256=90ded916bd57db2e0e171b6b098940a47c6f5042725dcdc67fb19940ca8bfdcc
        SOURCE_SUBDIR _none
    )
    FetchContent_MakeAvailable(imgui)
    add_library(imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/imgui_demo.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
        "${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
    )
    target_include_directories(imgui SYSTEM PUBLIC
        "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends" "${imgui_SOURCE_DIR}/misc/cpp")
    # No obsolete API, so code written against it can't creep in.
    target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_link_libraries(imgui PUBLIC SDL3::SDL3)
    if(NOT MSVC)
        target_compile_options(imgui PRIVATE -w)
    endif()
  endif()

    # --- RmlUi (editor UI feasibility spike, `--rml-ui`) --------------------------
    # MIT. Retained HTML/CSS-style UI. Only the core and its default FreeType font engine are
    # built; rendering goes through our RHI (editor/rml/rml_rhi.cpp) and input through the SDL3
    # platform file from the same tarball (compiled by the editor target, not edited).
    # FreeType comes from the font-import block above.
    FetchContent_Declare(rmlui
        URL https://github.com/mikke89/RmlUi/archive/refs/tags/6.3.tar.gz
        URL_HASH SHA256=d977298bb6147610e5984d5db85ddf284020d655a8713913f6982074f1dbdede
        SYSTEM
    )
    set(_cuervo_saved_shared ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
    set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
    set(RMLUI_COMPILER_OPTIONS OFF CACHE BOOL "" FORCE)
    set(RMLUI_INSTALL_LICENSES_AND_BUILD_INFO OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(rmlui)
    set(BUILD_SHARED_LIBS ${_cuervo_saved_shared})
    if(TARGET rmlui_core AND NOT MSVC)
        target_compile_options(rmlui_core PRIVATE -w)
    endif()

    # --- Brand fonts (DESIGN.md 4.16, milestone 8.1) -----------------------------
    # SIL OFL 1.1. The design kit ships only .woff2, which Dear ImGui's
    # stb_truetype can't read, so the static TTFs come from each family's own
    # tagged release. Only the editor uses them (UI: Figtree, code/console:
    # JetBrains Mono, splash: Outfit); exported games never contain them.
    FetchContent_Declare(outfit
        URL https://github.com/Outfitio/Outfit-Fonts/archive/refs/tags/1.1.tar.gz
        URL_HASH SHA256=fcf014f408292b0282e78ac49165ad7bc56eb1bb1fe5ec471ef65e0ac4b4bbfa
        SOURCE_SUBDIR _none
    )
    FetchContent_Declare(figtree
        URL https://github.com/erikdkennedy/figtree/archive/refs/tags/v2.0.3.tar.gz
        URL_HASH SHA256=0338c574a26a0f0ba0b796c0c205a4174d09399cf7deed2e4c4123bae789d7f3
        SOURCE_SUBDIR _none
    )
    FetchContent_Declare(jetbrains_mono
        URL https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip
        URL_HASH SHA256=6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf
        SOURCE_SUBDIR _none
    )
    FetchContent_MakeAvailable(outfit figtree jetbrains_mono)
    set(CUERVO_FONT_DISPLAY "${outfit_SOURCE_DIR}/fonts/ttf/Outfit-Medium.ttf")
    set(CUERVO_FONT_UI "${figtree_SOURCE_DIR}/fonts/ttf/Figtree-Regular.ttf")
    set(CUERVO_FONT_UI_BOLD "${figtree_SOURCE_DIR}/fonts/ttf/Figtree-SemiBold.ttf")
    set(CUERVO_FONT_MONO "${jetbrains_mono_SOURCE_DIR}/fonts/ttf/JetBrainsMono-Regular.ttf")
    foreach(_font IN ITEMS CUERVO_FONT_DISPLAY CUERVO_FONT_UI CUERVO_FONT_UI_BOLD CUERVO_FONT_MONO)
        if(NOT EXISTS "${${_font}}")
            message(FATAL_ERROR "Brand font missing from its release archive: ${${_font}}")
        endif()
    endforeach()
endif()
