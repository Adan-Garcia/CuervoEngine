add_library(cuervo STATIC
    core/log.cpp
    net/loopback.cpp
    net/lockstep.cpp
    net/rollback.cpp
    net/udp.cpp
    net/websocket.cpp
    net/http.cpp
    core/fixed_timestep.cpp
    core/name.cpp
    core/sha256.cpp
    core/profiler.cpp
    profile/trace.cpp
    save/save_store.cpp
    save/settings.cpp
    locale/locale.cpp
    math/dmath.cpp
    input/input_map.cpp
    input/replay.cpp
    platform/window.cpp
    platform/platform.cpp
    platform/file_watcher.cpp
    platform/main_loop.cpp
    platform/sdl_keymap.cpp
    platform/license.cpp
    brand/brand.cpp
    rhi/rhi.cpp
    rhi/gl/gl_loader.cpp
    rhi/gl/gl_device.cpp
    render/camera.cpp
    render/sdf_batcher.cpp
    render/sprite_batcher.cpp
    render/sprite_library.cpp
    render/post_fx.cpp
    render/user_shader.cpp
    render/light_batcher.cpp
    render/mesh_batcher.cpp
    render/font.cpp
    render/font_import.cpp
    render/text_layout.cpp
    text/shaping.cpp
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
    assets/image.cpp
    assets/atlas.cpp
    assets/sprite_sheet.cpp
    assets/svg_raster.cpp
    assets/atlas_file.cpp
    script/features.cpp
    script/dotnet_host.cpp
    script/plugin_host.cpp
    script/project.cpp
    script/script_host.cpp
    script/script_host_scene.cpp
    script/script_host_snapshot.cpp
    script/anim_bindings.cpp
    script/save_bindings.cpp
    script/locale_bindings.cpp
    script/net_bindings.cpp
    script/light_bindings.cpp
    script/sprite_bindings.cpp
    script/scene_bridge.cpp
    script/game_runner.cpp
    script/game_session.cpp
    script/vector_cache.cpp
    script/tier_check.cpp
    script/support.cpp
    script/project_edit.cpp
)
# The engine version packages check their `engine` range against (docs/PACKAGES.md).
target_compile_definitions(cuervo PRIVATE CUERVO_ENGINE_VERSION="${PROJECT_VERSION}")

# docs/support.json compiled in (milestone 8.8): the tier and export checks read the support matrix.
cuervo_generate_support_data(${CMAKE_CURRENT_BINARY_DIR}/generated/support_data.inc)
target_include_directories(cuervo PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

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

if(CUERVO_FEATURE_PHYSICS)
    target_sources(cuervo PRIVATE
        physics/physics_world.cpp
        physics/physics_shapes.cpp
        physics/physics_joints.cpp
        physics/physics_queries.cpp
        physics/physics_character.cpp
        physics/physics_step.cpp
        script/physics_bindings.cpp
    )
    target_link_libraries(cuervo PUBLIC box2d)
    target_compile_definitions(cuervo PUBLIC CUERVO_FEATURE_PHYSICS=1)
else()
    target_compile_definitions(cuervo PUBLIC CUERVO_FEATURE_PHYSICS=0)
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
    script/prelude/save.plm
    script/prelude/locale.plm
    script/prelude/net.plm
    script/prelude/http.plm
    script/prelude/scene.plm
    script/prelude/nodes.plm
    script/prelude/animation.plm
    script/prelude/tilemap.plm
    script/prelude/lighting.plm
    script/prelude/navigation/nodes.plm
    script/prelude/navigation/region.plm
    script/prelude/navigation/dijkstra.plm
    script/prelude/navigation/agent.plm
)
if(CUERVO_FEATURE_COLLISION)
    list(APPEND CUERVO_PRELUDE_FILES script/prelude/collision.plm)
endif()
if(CUERVO_FEATURE_PARTICLES)
    list(APPEND CUERVO_PRELUDE_FILES script/prelude/particles.plm)
endif()
if(CUERVO_FEATURE_UI)
    list(APPEND CUERVO_PRELUDE_FILES
        script/prelude/ui/theme.plm
        script/prelude/ui/control.plm
        script/prelude/ui/containers.plm
        script/prelude/ui/widgets.plm
        script/prelude/ui/popups.plm
        script/prelude/ui/ui.plm
    )
endif()
if(CUERVO_FEATURE_AUDIO)
    list(APPEND CUERVO_PRELUDE_FILES script/prelude/audio.plm)
endif()
if(CUERVO_FEATURE_PHYSICS)
    list(APPEND CUERVO_PRELUDE_FILES
        script/prelude/physics/shapes.plm
        script/prelude/physics/collision_polygon.plm
        script/prelude/physics/bodies.plm
        script/prelude/physics/area_raycast.plm
        script/prelude/physics/joints.plm
        script/prelude/physics/queries.plm
        script/prelude/physics/tilemap_collision.plm
    )
endif()
# One embedded prelude as .inc entries {"path", R"(text)"}. A file longer than
# ~60 KB becomes several consecutive entries with the same path, split at line
# ends: compilers only promise string literals up to 65536 bytes (clang warns,
# -Woverlength-strings), and the consumers join same-path entries.
function(cuervo_append_prelude out path text)
    set(_entries "${${out}}")
    while(TRUE)
        string(LENGTH "${text}" _len)
        if(_len LESS_EQUAL 60000)
            break()
        endif()
        string(SUBSTRING "${text}" 60000 -1 _rest)
        string(FIND "${_rest}" "\n" _nl)
        if(_nl EQUAL -1)
            break()
        endif()
        math(EXPR _cut "60000 + ${_nl} + 1")
        string(SUBSTRING "${text}" 0 ${_cut} _head)
        cuervo_raw_literal(_lit "__prelude__" "${_head}")
        string(APPEND _entries "{\"${path}\", ${_lit}},\n")
        string(SUBSTRING "${text}" ${_cut} -1 text)
    endwhile()
    cuervo_raw_literal(_lit "__prelude__" "${text}")
    string(APPEND _entries "{\"${path}\", ${_lit}},\n")
    set(${out} "${_entries}" PARENT_SCOPE)
endfunction()
set(_prelude_inc "")
# The language's math library (pluma/std/math.plm, LANGUAGE.md 9.20 - 9.24) comes first.
file(READ "${PROJECT_SOURCE_DIR}/pluma/std/math.plm" _text)
cuervo_append_prelude(_prelude_inc "pluma/math.plm" "${_text}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/pluma/std/math.plm")
foreach(_file IN LISTS CUERVO_PRELUDE_FILES)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${_file}" _text)
    file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}/script/prelude" "${CMAKE_CURRENT_SOURCE_DIR}/${_file}")
    cuervo_append_prelude(_prelude_inc "cuervo/${_rel}" "${_text}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_file}")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/cuervo_script_preludes.inc" CONTENT "${_prelude_inc}"
     @ONLY)
# Every optional prelude, compiled in or not: feature detection
# (script/features.cpp) reports what a project needs even from a stripped engine.
set(_optional_inc "")
set(CUERVO_OPTIONAL_PRELUDE_FILES
    script/prelude/collision.plm
    script/prelude/particles.plm
    script/prelude/audio.plm
    script/prelude/ui/theme.plm
    script/prelude/ui/control.plm
    script/prelude/ui/containers.plm
    script/prelude/ui/widgets.plm
    script/prelude/ui/popups.plm
    script/prelude/ui/ui.plm
    script/prelude/physics/shapes.plm
    script/prelude/physics/collision_polygon.plm
    script/prelude/physics/bodies.plm
    script/prelude/physics/area_raycast.plm
    script/prelude/physics/joints.plm
    script/prelude/physics/queries.plm
)
foreach(_file IN LISTS CUERVO_OPTIONAL_PRELUDE_FILES)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${_file}" _text)
    file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}/script/prelude" "${CMAKE_CURRENT_SOURCE_DIR}/${_file}")
    cuervo_append_prelude(_optional_inc "cuervo/${_rel}" "${_text}")
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

# The license and notices text (LICENSE.md, THIRD_PARTY_NOTICES.md, cmake/License.cmake).
cuervo_license_embed("${CMAKE_CURRENT_BINARY_DIR}/generated_license")
target_include_directories(cuervo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated_license")

# GLSL 450 -> SPIR-V + GLSL 330 + GLSL ES 300 headers (cmake/Shaders.cmake).
cuervo_add_shaders(cuervo
    render/shaders/sdf.vert
    render/shaders/sdf.frag
    render/shaders/mesh.vert
    render/shaders/mesh.frag
    render/shaders/text.vert
    render/shaders/text.frag
    render/shaders/light.vert
    render/shaders/light.frag
    render/shaders/sprite.vert
    render/shaders/sprite.frag
    render/shaders/post.vert
    render/shaders/post_down.frag
    render/shaders/post_up.frag
    render/shaders/post_composite.frag
)

find_package(Threads REQUIRED)
if(WIN32)
    target_link_libraries(cuervo PRIVATE ws2_32)
endif()
if(NOT EMSCRIPTEN)
    target_sources(cuervo PRIVATE net/tcp.cpp) # sockets + TLS for ws(s):// and http(s):// (the web uses the browser)
    if(TARGET mbedtls_lib)
        target_link_libraries(cuervo PRIVATE mbedtls_lib)
        target_compile_definitions(cuervo PUBLIC CUERVO_HAS_TLS=1)
    endif()
endif()
if(EMSCRIPTEN)
    target_link_options(cuervo PUBLIC -sFETCH=1) # browser fetch for net/http.cpp
    target_link_libraries(cuervo PUBLIC websocket.js) # browser WebSocket for net/websocket.cpp
endif()
target_link_libraries(cuervo
    PUBLIC project::flags pluma Threads::Threads
    PRIVATE SDL3::SDL3 libtess2 stb_image
)
if(TARGET webp_decoder)
    target_link_libraries(cuervo PRIVATE webp_decoder)
    target_compile_definitions(cuervo PRIVATE CUERVO_HAS_WEBP=1)
endif()

# Shipping builds load only ImportedVector files, never SVG (DESIGN.md 4.11).
option(CUERVO_SVG_IMPORT "Allow loading .svg files at runtime (off for shipping builds)" ON)
if(NOT CUERVO_SVG_IMPORT)
    target_compile_definitions(cuervo PRIVATE CUERVO_NO_SVG_IMPORT=1)
endif()

# HarfBuzz + SheenBidi (CUERVO_TEXT_SHAPING, cmake/Dependencies.cmake). Only
# text/shaping.cpp sees them; without them it falls back to one LTR run.
if(CUERVO_TEXT_SHAPING)
    target_link_libraries(cuervo PRIVATE harfbuzz sheenbidi)
    set_source_files_properties(text/shaping.cpp PROPERTIES COMPILE_DEFINITIONS CUERVO_HAS_TEXT_SHAPING=1)
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

# WebGPU backend (milestone 10.1): wgpu-native on desktop, emdawnwebgpu in the
# browser. The page requests the browser's device before main
# (web/webgpu_init.js, added by cuervo_web_app), so nothing needs JSPI.
if(CUERVO_WEBGPU)
    target_sources(cuervo PRIVATE rhi/webgpu/webgpu_device.cpp)
    target_compile_definitions(cuervo PRIVATE CUERVO_HAS_WEBGPU=1)
    if(EMSCRIPTEN)
        target_compile_options(cuervo PUBLIC --use-port=emdawnwebgpu)
        target_link_options(cuervo PUBLIC --use-port=emdawnwebgpu)
    else()
        target_link_libraries(cuervo PRIVATE wgpu_native)
    endif()
endif()
