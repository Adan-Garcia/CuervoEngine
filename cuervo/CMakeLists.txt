add_library(cuervo STATIC
    core/log.cpp
    core/fixed_timestep.cpp
    core/name.cpp
    core/profiler.cpp
    math/dmath.cpp
    input/input_map.cpp
    input/replay.cpp
    platform/window.cpp
    platform/platform.cpp
    platform/file_watcher.cpp
    platform/main_loop.cpp
    platform/sdl_keymap.cpp
    platform/eula.cpp
    platform/eula_text.cpp
    brand/brand.cpp
    rhi/rhi.cpp
    rhi/gl/gl_loader.cpp
    rhi/gl/gl_device.cpp
    render/camera.cpp
    render/sdf_batcher.cpp
    render/mesh_batcher.cpp
    render/font.cpp
    render/font_import.cpp
    render/text_layout.cpp
    render/text_renderer.cpp
    vector/path.cpp
    vector/flatten.cpp
    vector/tessellate.cpp
    vector/stroke.cpp
    vector/xml.cpp
    vector/svg.cpp
    vector/mesh.cpp
    vector/imported_vector.cpp
    vector/vector_asset.cpp
    jobs/job_system.cpp
    scene/scene_file.cpp
    assets/pack.cpp
    assets/vfs.cpp
    assets/manifest.cpp
    script/features.cpp
    script/project.cpp
    script/script_host.cpp
    script/script_host_scene.cpp
    script/scene_bridge.cpp
    script/game_runner.cpp
    script/game_session.cpp
    script/vector_cache.cpp
    script/tier_check.cpp
    script/project_edit.cpp
)

if(CUERVO_FEATURE_AUDIO)
    target_sources(cuervo PRIVATE
        audio/sound.cpp
        audio/mixer.cpp
        audio/audio_system.cpp
        script/audio_bindings.cpp
    )
    target_link_libraries(cuervo PRIVATE miniaudio)
    target_compile_definitions(cuervo PUBLIC CUERVO_FEATURE_AUDIO=1)
else()
    target_compile_definitions(cuervo PUBLIC CUERVO_FEATURE_AUDIO=0)
endif()

if(CUERVO_FEATURE_PARTICLES)
    target_sources(cuervo PRIVATE
        particles/particle_pool.cpp
        particles/emitter.cpp
        particles/simd_kernels.cpp
        particles/particle_system.cpp
        particles/particle_render.cpp
    )
    target_compile_definitions(cuervo PUBLIC CUERVO_FEATURE_PARTICLES=1)
else()
    target_compile_definitions(cuervo PUBLIC CUERVO_FEATURE_PARTICLES=0)
endif()

if(CUERVO_FEATURE_COLLISION)
    target_sources(cuervo PRIVATE
        collision/collision_world.cpp
    )
    target_compile_definitions(cuervo PUBLIC CUERVO_FEATURE_COLLISION=1)
else()
    target_compile_definitions(cuervo PUBLIC CUERVO_FEATURE_COLLISION=0)
endif()

if(CUERVO_FEATURE_UI)
    target_compile_definitions(cuervo PUBLIC CUERVO_FEATURE_UI=1)
else()
    target_compile_definitions(cuervo PUBLIC CUERVO_FEATURE_UI=0)
endif()

if(CUERVO_PROFILING)
    target_compile_definitions(cuervo PUBLIC CUERVO_PROFILING=1)
else()
    target_compile_definitions(cuervo PUBLIC CUERVO_PROFILING=0)
endif()
if(CUERVO_TRACY)
    target_compile_definitions(cuervo PUBLIC CUERVO_TRACY=1)
    target_link_libraries(cuervo PUBLIC tracy_client)
else()
    target_compile_definitions(cuervo PUBLIC CUERVO_TRACY=0)
endif()

if(CUERVO_FEATURE_DEV)
    target_compile_definitions(cuervo PUBLIC CUERVO_FEATURE_DEV=1)
else()
    target_compile_definitions(cuervo PUBLIC CUERVO_FEATURE_DEV=0)
endif()

# The script API prelude (cuervo/script/prelude/*.plm) is embedded as raw
# string literals; editing a prelude file re-runs this step.
set(CUERVO_PRELUDE_FILES
    script/prelude/core.plm
    script/prelude/scene.plm
    script/prelude/nodes.plm
)
if(CUERVO_FEATURE_COLLISION)
    list(APPEND CUERVO_PRELUDE_FILES script/prelude/collision.plm)
endif()
if(CUERVO_FEATURE_PARTICLES)
    list(APPEND CUERVO_PRELUDE_FILES script/prelude/particles.plm)
endif()
if(CUERVO_FEATURE_UI)
    list(APPEND CUERVO_PRELUDE_FILES script/prelude/ui.plm)
endif()
if(CUERVO_FEATURE_AUDIO)
    list(APPEND CUERVO_PRELUDE_FILES script/prelude/audio.plm)
endif()
set(_prelude_inc "")
# The language's math library (pluma/std/math.plm, LANGUAGE.md 9.20 - 9.24) comes first.
file(READ "${PROJECT_SOURCE_DIR}/pluma/std/math.plm" _text)
string(APPEND _prelude_inc "{\"pluma/math.plm\", R\"__prelude__(${_text})__prelude__\"},\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/pluma/std/math.plm")
foreach(_file IN LISTS CUERVO_PRELUDE_FILES)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${_file}" _text)
    get_filename_component(_name "${_file}" NAME)
    string(APPEND _prelude_inc "{\"cuervo/${_name}\", R\"__prelude__(${_text})__prelude__\"},\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_file}")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/cuervo_script_preludes.inc" CONTENT "${_prelude_inc}"
     @ONLY)
# Every optional prelude, compiled in or not: feature detection
# (script/features.cpp) reports what a project needs even from a stripped engine.
set(_optional_inc "")
foreach(_file IN ITEMS script/prelude/collision.plm script/prelude/particles.plm script/prelude/ui.plm
                      script/prelude/audio.plm)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${_file}" _text)
    get_filename_component(_name "${_file}" NAME)
    string(APPEND _optional_inc "{\"cuervo/${_name}\", R\"__prelude__(${_text})__prelude__\"},\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_file}")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/cuervo_optional_preludes.inc" CONTENT "${_optional_inc}"
     @ONLY)
target_include_directories(cuervo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Brand tokens and the window icon (DESIGN.md 4.16, cmake/Brand.cmake), generated
# from assets/brand/ at configure time. Public: the editor themes itself with them.
include(Brand)
cuervo_brand_generate("${CMAKE_CURRENT_BINARY_DIR}/generated_brand")
target_include_directories(cuervo PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated_brand")

# The EULA's version and text (LICENSE.md, cmake/Eula.cmake): the acceptance gate.
cuervo_eula_embed("${CMAKE_CURRENT_BINARY_DIR}/generated_eula")
target_include_directories(cuervo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated_eula")
target_compile_definitions(cuervo PRIVATE CUERVO_EULA_VERSION="${CUERVO_EULA_VERSION}" CUERVO_EULA_DATE="${CUERVO_EULA_DATE}")

# GLSL 450 -> SPIR-V + GLSL 330 + GLSL ES 300 headers (cmake/Shaders.cmake).
cuervo_add_shaders(cuervo
    render/shaders/sdf.vert
    render/shaders/sdf.frag
    render/shaders/mesh.vert
    render/shaders/mesh.frag
    render/shaders/text.vert
    render/shaders/text.frag
)

find_package(Threads REQUIRED)
target_link_libraries(cuervo
    PUBLIC project::flags pluma Threads::Threads
    PRIVATE SDL3::SDL3 libtess2
)

# Shipping builds load only ImportedVector files, never SVG (DESIGN.md 4.11).
option(CUERVO_SVG_IMPORT "Allow loading .svg files at runtime (off for shipping builds)" ON)
if(NOT CUERVO_SVG_IMPORT)
    target_compile_definitions(cuervo PRIVATE CUERVO_NO_SVG_IMPORT=1)
endif()

# TTF/OTF import (CUERVO_FONT_IMPORT, cmake/Dependencies.cmake). Only
# font_import.cpp sees msdfgen/FreeType; without it fonts load from .efont.
if(CUERVO_FONT_IMPORT)
    target_link_libraries(cuervo PRIVATE msdf_atlas)
    set_source_files_properties(render/font_import.cpp PROPERTIES COMPILE_DEFINITIONS CUERVO_HAS_FONT_IMPORT=1)
endif()

if(CUERVO_VULKAN)
    target_sources(cuervo PRIVATE rhi/vulkan/vk_device.cpp)
    # `VkFoo info{VK_STRUCTURE_TYPE_FOO}` is the idiomatic way to zero the rest
    # of a Vulkan struct; GCC/Clang would warn on every one of them.
    if(NOT MSVC)
        set_source_files_properties(rhi/vulkan/vk_device.cpp PROPERTIES
            COMPILE_OPTIONS -Wno-missing-field-initializers)
    endif()
    target_link_libraries(cuervo PRIVATE vulkan_deps)
    target_compile_definitions(cuervo PRIVATE CUERVO_HAS_VULKAN=1)
endif()
