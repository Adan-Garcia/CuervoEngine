# WebGL2 / WASM build settings (DESIGN.md 2 "Platforms: Web"; docs/WEB.md).
# Included from the top-level CMakeLists.txt before any target exists when the
# Emscripten toolchain is active (`cmake --preset web`, or `emcmake cmake`).
#
# Decisions (docs/WEB.md "Decisions"):
#  - Single-threaded by default (no -pthread): no SharedArrayBuffer, so no
#    COOP/COEP headers are needed to host the page; the job system runs inline
#    (DESIGN.md 4.14) and miniaudio mixes on the main thread through Web Audio.
#    CUERVO_WEB_THREADS=ON (presets web-threads, web-pack-threads; Phase 10)
#    builds with WASM threads instead: the job system gets real workers, and
#    the page must be served cross-origin isolated (docs/WEB.md "Threads").
#  - Native WASM exceptions (-fwasm-exceptions) everywhere: the language
#    compiler reports its limits with C++ exceptions, and the default
#    (exceptions abort) would turn a script error into a crash.
#  - No Vulkan (CUERVO_VULKAN forced off); WebGL2 = the GL backend's GLES 3.0 path.
#    With CUERVO_WEBGPU (the default when naga is found) the same build also has
#    the WebGPU backend and uses it when the page got a device (web/webgpu_init.js).
#  - No runtime font import (FreeType + msdf-atlas-gen are threaded and big):
#    fonts ship as .efont atlases converted by the host build at build time.
#  - Release-only in practice: a Debug WASM build works but runs the VM ~10x slower.

# Host tools: shader_tool (GLSL -> GLSL ES 300) and the desktop runtime (font
# import) come from a native build of this repository (`cmake --preset host-tools`).
set(CUERVO_HOST_BUILD "" CACHE PATH "Native build directory providing shader_tool and runtime (web builds)")
if(NOT CUERVO_SHADER_TOOL AND CUERVO_HOST_BUILD)
    set(CUERVO_SHADER_TOOL "${CUERVO_HOST_BUILD}/shader_tool" CACHE FILEPATH "" FORCE)
endif()
if(NOT CUERVO_SHADER_TOOL OR NOT EXISTS "${CUERVO_SHADER_TOOL}")
    message(FATAL_ERROR "Web build: set CUERVO_HOST_BUILD to a native build dir containing shader_tool "
                        "(cmake --preset host-tools && cmake --build --preset host-tools); see docs/WEB.md")
endif()

set(CUERVO_FONT_IMPORT OFF CACHE BOOL "" FORCE)
set(CUERVO_BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_compile_options(-fwasm-exceptions)
add_link_options(-fwasm-exceptions)

# WASM threads (SharedArrayBuffer). Set before any target, so the fetched
# dependencies (SDL3, ...) are built with atomics too: a module mixes them
# only if every object has them.
option(CUERVO_WEB_THREADS "Web build with WASM threads (needs a cross-origin isolated page: COOP/COEP)" OFF)
if(CUERVO_WEB_THREADS)
    add_compile_options(-pthread)
    # A pool started before main(): creating a std::thread then only hands
    # it a ready worker. Without one, the browser's main thread would block
    # waiting for a worker that can only load once it yields. 8 covers the
    # job system's automatic maximum (jobs::auto_thread_count, DESIGN.md 4.15).
    add_link_options(-pthread -sPTHREAD_POOL_SIZE=8)
endif()

# The project shipped in the page and its font (docs/WEB.md "Asset preload").
set(CUERVO_WEB_GAME "${PROJECT_SOURCE_DIR}/examples/shmup" CACHE PATH "Project directory preloaded into the web build")
set(CUERVO_WEB_FONT_SOURCE "${PROJECT_SOURCE_DIR}/assets/fonts/LiberationSans-Regular.ttf" CACHE FILEPATH
    "TTF/OTF converted to .efont for the web build (or an .efont used as is)")

# Release (docs/EXPORT.md "Web"): instead of a raw project directory the page
# can run an exported pack (cuervo-tools pack/export):
#   ""        (default) preload CUERVO_WEB_GAME + the font, as above
#   fetch     a game-independent runtime: the page fetches `game.pak` next to
#             index.html into MEMFS before main() (web/pack_loader.js); this is
#             what `cuervo-tools export --platform web` ships
#   <file>    preload that .pak as /game.pak (one self-contained build)
set(CUERVO_WEB_PACK "" CACHE STRING "Web build runs a pack: empty (raw project), 'fetch', or a .pak path")

# With the WebGPU backend the page requests the browser's device before main
# (web/webgpu_init.js, milestone 10.3). Called after LINK_DEPENDS is set, which it extends.
function(_cuervo_web_webgpu target)
    if(CUERVO_WEBGPU)
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${PROJECT_SOURCE_DIR}/web/webgpu_init.js")
        target_link_options(${target} PRIVATE "--pre-js=${PROJECT_SOURCE_DIR}/web/webgpu_init.js")
    endif()
endfunction()

# cuervo_web_app(<target>): turns an executable into index.html + .js + .wasm
# + .data (the preloaded project and font, or the pack).
function(cuervo_web_app target)
    if(CUERVO_WEB_PACK)
        set_target_properties(${target} PROPERTIES OUTPUT_NAME index SUFFIX ".html"
            LINK_DEPENDS "${PROJECT_SOURCE_DIR}/web/shell.html;${PROJECT_SOURCE_DIR}/web/pack_loader.js")
        target_compile_definitions(${target} PRIVATE CUERVO_WEB_PACK="/game.pak")
        target_link_options(${target} PRIVATE
            -sENVIRONMENT=web
            -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
            -sGL_ENABLE_GET_PROC_ADDRESS=1
            -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864
            -sSTACK_SIZE=2097152
            -sEXIT_RUNTIME=0
            # FS: smoke_test.mjs --pull reads captures out of MEMFS (docs/WEB.md "Verifying").
            -sEXPORTED_RUNTIME_METHODS=callMain,FS
            # The runtime opens /game.pak; the file system must exist even
            # with nothing preloaded.
            -sFORCE_FILESYSTEM=1
            "--shell-file=${PROJECT_SOURCE_DIR}/web/shell.html")
        if(CUERVO_WEB_PACK STREQUAL "fetch")
            target_link_options(${target} PRIVATE "--pre-js=${PROJECT_SOURCE_DIR}/web/pack_loader.js")
        else()
            if(NOT EXISTS "${CUERVO_WEB_PACK}")
                message(FATAL_ERROR "CUERVO_WEB_PACK: '${CUERVO_WEB_PACK}' not found (a .pak from cuervo-tools pack --platform web)")
            endif()
            set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${CUERVO_WEB_PACK}")
            target_link_options(${target} PRIVATE "--preload-file=${CUERVO_WEB_PACK}@/game.pak")
        endif()
        _cuervo_web_webgpu(${target})
        return()
    endif()
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}")
    set(efont "${CMAKE_BINARY_DIR}/web_assets/default.efont")
    get_filename_component(font_ext "${CUERVO_WEB_FONT_SOURCE}" LAST_EXT)
    if(font_ext STREQUAL ".efont")
        configure_file("${CUERVO_WEB_FONT_SOURCE}" "${efont}" COPYONLY)
    else()
        set(host_runtime "${CUERVO_HOST_BUILD}/runtime/cuervo")
        if(NOT EXISTS "${host_runtime}")
            message(FATAL_ERROR "Web build: ${host_runtime} not found (build the host-tools preset's runtime target (cuervo))")
        endif()
        add_custom_command(OUTPUT "${efont}"
            COMMAND "${host_runtime}" --font "${CUERVO_WEB_FONT_SOURCE}" --export "${efont}"
            DEPENDS "${CUERVO_WEB_FONT_SOURCE}" "${host_runtime}"
            COMMENT "Importing ${CUERVO_WEB_FONT_SOURCE} -> default.efont for the web build"
            VERBATIM)
    endif()
    add_custom_target(${target}_web_font DEPENDS "${efont}")
    add_dependencies(${target} ${target}_web_font)

    # Every file of the project, so editing a script re-packs the .data file.
    file(GLOB_RECURSE game_files CONFIGURE_DEPENDS "${CUERVO_WEB_GAME}/*")
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME index
        SUFFIX ".html"
        LINK_DEPENDS "${PROJECT_SOURCE_DIR}/web/shell.html;${efont};${game_files}")
    target_compile_definitions(${target} PRIVATE
        CUERVO_WEB_GAME_DIR="/game" CUERVO_WEB_FONT="/fonts/default.efont")
    target_link_options(${target} PRIVATE
        -sENVIRONMENT=web
        -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
        # SDL_GL_GetProcAddress resolves the GL backend's entry points.
        -sGL_ENABLE_GET_PROC_ADDRESS=1
        -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864
        # The parser/checker recurse; Emscripten's default stack is 64 KB.
        -sSTACK_SIZE=2097152
        # The page outlives main() (emscripten_set_main_loop); see main_loop.hpp.
        -sEXIT_RUNTIME=0
        # FS: smoke_test.mjs --pull reads captures out of MEMFS (docs/WEB.md "Verifying").
        -sEXPORTED_RUNTIME_METHODS=callMain,FS
        "--shell-file=${PROJECT_SOURCE_DIR}/web/shell.html"
        "--preload-file=${CUERVO_WEB_GAME}@/game"
        "--preload-file=${efont}@/fonts/default.efont"
        # Development-only files never ship.
        "--exclude-file=*.import*")
    _cuervo_web_webgpu(${target})
endfunction()
