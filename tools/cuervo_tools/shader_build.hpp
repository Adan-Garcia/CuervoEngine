#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// User shader import (DESIGN.md 4.19, 11.2b): GLSL -> `.cshader` through the
// engine's own shader pipeline, run at import/export time so no compiler ships
// in the game (and the web build has none).
namespace cuervo::tools {

// Wraps `glsl_source` (render/user_shader.hpp), compiles it with glslangValidator,
// and translates it with shader_tool (SPIRV-Cross; WGSL through spirv-opt + naga
// when both are available). The programs come from CUERVO_GLSLANG,
// CUERVO_SHADER_TOOL, CUERVO_NAGA, CUERVO_SPIRV_OPT or this build tree. On
// failure returns nullopt with the compiler's message in `error`; `notes`
// collects non-fatal remarks (e.g. no WGSL: the shader won't run on WebGPU).
std::optional<std::vector<std::byte>> build_cshader(std::string_view glsl_source, std::string_view label,
                                                    std::string& error, std::vector<std::string>* notes = nullptr);

// Builds every `.glsl` under `project` into a sibling `.cshader`; returns the
// number built, or -1 with `error` set.
int build_project_shaders(const std::filesystem::path& project, std::string& error);

} // namespace cuervo::tools
