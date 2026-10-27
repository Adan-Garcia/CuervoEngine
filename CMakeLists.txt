cmake_minimum_required(VERSION 3.24)
project(cuervo VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Use ccache if available to accelerate builds across worktrees
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
endif()

option(CUERVO_BUILD_TESTS "Build Cuervo Engine and Pluma tests" ON)

# Vulkan is the desktop backend (DESIGN.md 4.4); the web build is GL-only.
if(EMSCRIPTEN)
    set(CUERVO_VULKAN OFF CACHE BOOL "" FORCE)
else()
    option(CUERVO_VULKAN "Build the Vulkan render backend" ON)
endif()

# WebGPU (DESIGN.md 6, milestone 10; experimental). Shaders are translated
# SPIR-V -> WGSL by naga, a pinned host tool (`cargo install naga-cli --version
# 29.0.4 --locked`), after spirv-opt splits combined samplers. On by default
# for web builds and native Linux x86_64 builds that have both tools; a web
# build with it runs WebGPU where the browser has it and WebGL2 otherwise
# (milestone 10.3, docs/WEB.md "WebGPU").
set(CUERVO_NAGA_VERSION 29.0.4)
find_program(CUERVO_NAGA NAMES naga HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin"
    DOC "naga-cli ${CUERVO_NAGA_VERSION} (SPIR-V -> WGSL for the WebGPU backend)")
find_program(CUERVO_SPIRV_OPT NAMES spirv-opt HINTS "$ENV{VULKAN_SDK}/bin"
    DOC "spirv-opt from SPIRV-Tools (splits combined samplers for WGSL)")
set(_cuervo_webgpu_default OFF)
if(CUERVO_NAGA AND CUERVO_SPIRV_OPT AND (EMSCRIPTEN OR (CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")))
    execute_process(COMMAND "${CUERVO_NAGA}" --version OUTPUT_VARIABLE _cuervo_naga_version
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_cuervo_naga_version STREQUAL CUERVO_NAGA_VERSION)
        set(_cuervo_webgpu_default ON)
    endif()
endif()
option(CUERVO_WEBGPU "Build the WebGPU render backend (experimental; needs naga-cli ${CUERVO_NAGA_VERSION} and spirv-opt)"
    ${_cuervo_webgpu_default})

# Modular footprint feature options (Milestone 7.5, DESIGN.md 6).
option(CUERVO_FEATURE_AUDIO "Build audio subsystem" ON)
option(CUERVO_FEATURE_UI "Build UI subsystem" ON)
option(CUERVO_FEATURE_PARTICLES "Build particle system" ON)
option(CUERVO_FEATURE_COLLISION "Build collision world" ON)
option(CUERVO_FEATURE_PHYSICS "Build physics subsystem" ON)
option(CUERVO_FEATURE_DEV "Build dev tools into runtime (demos, viewer, benchmarks)" ON)

# Profiling (DESIGN.md 5.14 "Clarified (Phase 8 -- Profiler)", 7.2): engine
# zones (cuervo/core/profiler.hpp); OFF compiles the zone macros to nothing
# (shipping builds). CUERVO_TRACY also maps zones and frame marks to Tracy.
option(CUERVO_PROFILING "Compile engine profiling zones in (cuervo --profile, the editor's Profiler panel)" ON)
option(CUERVO_TRACY "Send engine zones and frame marks to the Tracy profiler" OFF)
if(CUERVO_TRACY AND NOT CUERVO_PROFILING)
    message(FATAL_ERROR "CUERVO_TRACY requires CUERVO_PROFILING")
endif()

if(CUERVO_FEATURE_PARTICLES AND NOT CUERVO_FEATURE_COLLISION)
    message(FATAL_ERROR "CUERVO_FEATURE_PARTICLES requires CUERVO_FEATURE_COLLISION")
endif()
if(CUERVO_FEATURE_DEV AND NOT CUERVO_FEATURE_PARTICLES)
    message(FATAL_ERROR "CUERVO_FEATURE_DEV requires CUERVO_FEATURE_PARTICLES (the particles demo)")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
if(EMSCRIPTEN)
    include(Web) # before any target: global WASM flags, host tools (docs/WEB.md)
endif()
include(CompilerFlags)
include(RawLiteral)
include(Windows)
include(Dependencies)
include(Shaders)
include(License)
include(SupportData)

add_subdirectory(pluma)
add_subdirectory(cuervo)
add_subdirectory(runtime)
add_subdirectory(tools/cuervo_tools)
if(NOT EMSCRIPTEN)
    add_subdirectory(examples/plugins) # milestone 10.5c, docs/PLUGINS.md
endif()
# The editor is desktop-only (DESIGN.md 4.13); the web build never sees it.
if(CUERVO_BUILD_EDITOR AND NOT EMSCRIPTEN)
    add_subdirectory(editor)
endif()
if(NOT EMSCRIPTEN)
    add_subdirectory(tools/rhi_parity)
endif()

if(CUERVO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
