cmake_minimum_required(VERSION 3.24)
project(cuervo VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CUERVO_BUILD_TESTS "Build Cuervo Engine and Pluma tests" ON)

# Vulkan is the desktop backend (DESIGN.md 4.4); the web build is GL-only.
if(EMSCRIPTEN)
    set(CUERVO_VULKAN OFF CACHE BOOL "" FORCE)
else()
    option(CUERVO_VULKAN "Build the Vulkan render backend" ON)
endif()

# Modular footprint feature options (Milestone 7.5, DESIGN.md 6).
option(CUERVO_FEATURE_AUDIO "Build audio subsystem" ON)
option(CUERVO_FEATURE_UI "Build UI subsystem" ON)
option(CUERVO_FEATURE_PARTICLES "Build particle system" ON)
option(CUERVO_FEATURE_COLLISION "Build collision world" ON)
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
include(Dependencies)
include(Shaders)
include(Eula)
# The EULA version (LICENSE.md) the programs ask users to accept: cuervo/platform/eula.hpp,
# and pluma/repl's own copy of the gate (pluma never depends on cuervo/).
cuervo_eula_version()

add_subdirectory(pluma)
add_subdirectory(cuervo)
add_subdirectory(runtime)
add_subdirectory(tools/cuervo_tools)
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
