#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cuervo/render/post_fx.hpp"
#include "cuervo/rhi/rhi.hpp"

// User shaders (DESIGN.md 4.19, 11.2b): a project's `.glsl` is a full-screen
// fragment shader for PostFx, written against a small fixed interface and
// precompiled by `cuervo-tools shader build` (SPIR-V, GLSL 330 / ES 300, WGSL
// from the one source, through the engine's glslang / SPIRV-Cross / naga
// pipeline) into a `.cshader` file the game loads. There is no runtime
// compiler: web builds can only run precompiled shaders.
//
//   @export float strength = 0.5;            // shows up as an inspector field
//   @export vec3 tint = vec3(1.0, 0.8, 0.6);
//   void main() {
//       vec4 c = texture(u_src, v_uv);        // u_src, u_aux: samplers 0 and 1
//       o_color = vec4(mix(c.rgb, c.rgb * tint, strength), 1.0);
//   }
//
// Inputs: v_uv (sample coordinate of render targets), v_screen (0..1, y down),
// u_src, u_aux, o_color. Exports live in the PostParams block (v0..v2).
namespace cuervo::render {

struct ExportedUniform {
    std::string name;
    int components = 1;      // float = 1 ... vec4 = 4
    int slot = 0;            // index of the first component in v0..v2 (0..11)
    float defaults[4] = {};
    friend bool operator==(const ExportedUniform&, const ExportedUniform&) = default;
};

struct WrappedShader {
    std::string glsl;                    // complete GLSL 450 fragment shader
    std::vector<ExportedUniform> exports;
};

// Finds the `@export` declarations (removed from the source, replaced by macros
// onto the PostParams block) and wraps the user's code with the fixed interface.
// Errors name the line.
std::optional<WrappedShader> wrap_user_glsl(std::string_view source, std::string& error);

// What `cuervo-tools shader build` stores and the runtime loads (".cshader").
struct CompiledShader {
    std::vector<std::uint32_t> spirv;
    std::string glsl330, glsl_es300, wgsl; // wgsl empty in builds without naga
    std::vector<std::pair<std::string, std::uint32_t>> uniform_blocks;
    std::vector<std::pair<std::string, std::uint32_t>> textures;
    std::vector<ExportedUniform> exports;
};

std::vector<std::byte> encode_cshader(const CompiledShader& shader);
std::optional<CompiledShader> decode_cshader(std::span<const std::byte> bytes);

// A loaded user shader: owns the strings the rhi::ShaderSource points into.
class UserShader {
public:
    explicit UserShader(CompiledShader compiled);
    UserShader(const UserShader&) = delete;
    UserShader& operator=(const UserShader&) = delete;

    const rhi::ShaderSource& source() const { return source_; }
    const std::vector<ExportedUniform>& exports() const { return compiled_.exports; }

    // PostParams with every export at its default (v3.x = 0: no output encode).
    PostParams defaults() const;
    // Writes `values` (up to the export's component count) for export `name`.
    bool set(PostParams& params, std::string_view name, std::span<const float> values) const;

private:
    CompiledShader compiled_;
    std::vector<rhi::UniformBlockBinding> blocks_;
    std::vector<rhi::TextureBinding> textures_;
    rhi::ShaderSource source_{};
};

} // namespace cuervo::render
