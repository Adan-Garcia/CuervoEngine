# Shader build step (DESIGN.md 4.4): GLSL 450 is authored once, compiled to
# SPIR-V by glslangValidator, then cross-compiled to GLSL 330 / ES 300 by the
# host tool `shader_tool` (SPIRV-Cross). Everything lands in generated headers,
# so the runtime never compiles shader source except GL's own glCompileShader
# on the generated GLSL.

find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang
    HINTS "$ENV{VULKAN_SDK}/bin"
    DOC "glslangValidator from Khronos glslang (GLSL -> SPIR-V)")
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR
        "glslangValidator not found; it is needed to compile the engine's shaders.\n"
        "Install glslang (Arch: pacman -S glslang, Debian/Ubuntu: apt install glslang-tools, "
        "or the LunarG Vulkan SDK) or pass -DGLSLANG_VALIDATOR=/path/to/glslangValidator.")
endif()

# The tool must run on the build machine. Cross builds (e.g. Emscripten) pass a
# host-built executable via CUERVO_SHADER_TOOL.
set(CUERVO_SHADER_TOOL "" CACHE FILEPATH "Prebuilt host shader_tool (for cross-compiling builds)")
if(CUERVO_SHADER_TOOL)
    set(_cuervo_shader_tool "${CUERVO_SHADER_TOOL}")
    set(_cuervo_shader_tool_dep "${CUERVO_SHADER_TOOL}")
elseif(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Cross-compiling: build tools/shader_tool for the host and set CUERVO_SHADER_TOOL")
else()
    add_executable(shader_tool "${PROJECT_SOURCE_DIR}/tools/shader_tool/shader_tool.cpp")
    target_link_libraries(shader_tool PRIVATE spirv_cross_glsl project::flags)
    set(_cuervo_shader_tool "$<TARGET_FILE:shader_tool>")
    set(_cuervo_shader_tool_dep shader_tool)
endif()

# cuervo_add_shaders(<target> <shader files...>)
# Each `name.stage` (stage = vert|frag) becomes "<build>/generated/shaders/name_stage.hpp",
# defining `cuervo::shaders::name_stage` (an rhi::ShaderSource). The include
# path `shaders/name_stage.hpp` is added to <target> (PUBLIC).
function(cuervo_add_shaders target)
    set(out_dir "${CMAKE_BINARY_DIR}/generated/shaders")
    file(MAKE_DIRECTORY "${out_dir}")
    set(headers)
    foreach(source IN LISTS ARGN)
        get_filename_component(abs "${source}" ABSOLUTE)
        get_filename_component(name "${source}" NAME_WE)
        get_filename_component(ext "${source}" LAST_EXT)
        string(SUBSTRING "${ext}" 1 -1 stage)
        set(symbol "${name}_${stage}")
        set(spv "${out_dir}/${symbol}.spv")
        set(header "${out_dir}/${symbol}.hpp")
        # WebGPU (milestone 10.1): the same source compiled with CUERVO_WEBGPU
        # defined (WGSL has no point size, so shaders skip gl_PointSize), its
        # combined samplers split and renumbered, then naga writes the WGSL.
        set(wgsl_commands)
        set(wgsl_byproducts)
        set(wgsl_arg)
        if(CUERVO_WEBGPU)
            set(wgpu_spv "${out_dir}/${symbol}.webgpu.spv")
            set(wgsl "${out_dir}/${symbol}.wgsl")
            set(wgsl_commands
                COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.0 --quiet -DCUERVO_WEBGPU
                    -o "${wgpu_spv}" "${abs}"
                COMMAND "${CUERVO_SPIRV_OPT}" --split-combined-image-sampler -o "${wgpu_spv}" "${wgpu_spv}"
                COMMAND "${_cuervo_shader_tool}" --webgpu-bindings "${wgpu_spv}" "${wgpu_spv}"
                # The sources put NDC y = +1 at the top (GL convention, rhi.hpp),
                # which is WebGPU's own: naga must not flip y as it would for
                # Vulkan-convention SPIR-V.
                COMMAND "${CUERVO_NAGA}" --keep-coordinate-space "${wgpu_spv}" "${wgsl}")
            set(wgsl_byproducts "${wgpu_spv}" "${wgsl}")
            set(wgsl_arg "${wgsl}")
        endif()
        add_custom_command(
            OUTPUT "${header}"
            BYPRODUCTS "${spv}" ${wgsl_byproducts}
            # SPIR-V 1.0 (vulkan1.0 env): valid on our Vulkan 1.3 device, and it keeps
            # glslang from emitting 1.6-only ops (e.g. discard -> demote) that
            # SPIRV-Cross can't express in GLSL 330 / ES 300.
            COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.0 --quiet -o "${spv}" "${abs}"
            ${wgsl_commands}
            COMMAND "${_cuervo_shader_tool}" "${spv}" "${symbol}" "${header}" ${wgsl_arg}
            DEPENDS "${abs}" ${_cuervo_shader_tool_dep}
            COMMENT "Compiling shader ${name}.${stage}"
            VERBATIM)
        list(APPEND headers "${header}")
    endforeach()
    target_sources(${target} PRIVATE ${headers})
    target_include_directories(${target} PUBLIC "${CMAKE_BINARY_DIR}/generated")
endfunction()
