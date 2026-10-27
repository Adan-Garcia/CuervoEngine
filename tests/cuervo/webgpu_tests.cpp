// WebGPU backend (DESIGN.md 6, milestones 10.1-10.2): backend selection and the
// WGSL the build generates (cmake/Shaders.cmake, shader_tool). Rendering
// itself is checked against GL and Vulkan by the rhi_parity test.

#include <doctest.h>

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "cuervo/platform/window.hpp"
#include "cuervo/rhi/rhi.hpp"
#include "shaders/light_frag.hpp"
#include "shaders/light_vert.hpp"
#include "shaders/mesh_frag.hpp"
#include "shaders/mesh_vert.hpp"
#include "shaders/sdf_frag.hpp"
#include "shaders/sdf_vert.hpp"
#include "shaders/text_frag.hpp"
#include "shaders/text_vert.hpp"

using namespace cuervo;

namespace {

// The binding of the WGSL global `declaration` ("var u_atlas_image",
// "var<uniform> globals"): naga writes `@group(G) @binding(B)` right above it.
std::string group_and_binding(std::string_view wgsl, std::string_view declaration) {
    const std::size_t at = wgsl.find(declaration);
    if (at == std::string_view::npos) return "missing";
    const std::size_t group = wgsl.rfind("@group(", at);
    if (group == std::string_view::npos) return "unbound";
    const std::string_view attributes = wgsl.substr(group, at - group);
    return std::string(attributes.substr(0, attributes.find_last_not_of(" \n") + 1));
}

} // namespace

TEST_CASE("--backend spellings and the window each backend needs") {
    rhi::Backend b = rhi::Backend::OpenGL;
    CHECK(rhi::parse_backend("webgpu", b));
    CHECK(b == rhi::Backend::WebGPU);
    CHECK(rhi::parse_backend("wgpu", b));
    CHECK(b == rhi::Backend::WebGPU);
    CHECK(rhi::parse_backend("vk", b));
    CHECK(b == rhi::Backend::Vulkan);
    CHECK(rhi::parse_backend("opengl", b));
    CHECK(b == rhi::Backend::OpenGL);
    CHECK_FALSE(rhi::parse_backend("metal", b));
    CHECK(b == rhi::Backend::OpenGL); // untouched on failure

    CHECK(rhi::backend_name(rhi::Backend::WebGPU) == "WebGPU");
    CHECK(rhi::graphics_api(rhi::Backend::WebGPU) == GraphicsApi::WebGPU);
    CHECK(rhi::graphics_api(rhi::Backend::Vulkan) == GraphicsApi::Vulkan);
    CHECK(rhi::graphics_api(rhi::Backend::OpenGL) == GraphicsApi::OpenGL);
}

TEST_CASE("generated WGSL splits texture binding N into texture 2N and sampler 2N + 1") {
    if (!rhi::backend_compiled_in(rhi::Backend::WebGPU)) {
        // Without the backend the build skips naga entirely.
        CHECK(shaders::text_frag.wgsl.empty());
        return;
    }
    const std::string_view text = shaders::text_frag.wgsl;
    REQUIRE_FALSE(text.empty());
    REQUIRE(shaders::text_frag.textures.size() == 1);
    CHECK(shaders::text_frag.textures[0].binding == 0);
    CHECK(group_and_binding(text, "var u_atlas_image: texture_2d<f32>") == "@group(1) @binding(0)");
    CHECK(group_and_binding(text, "var u_atlas_sampler: sampler") == "@group(1) @binding(1)");
    CHECK(text.find("@fragment") != std::string_view::npos);

    // Uniform blocks keep their set-0 binding.
    CHECK(group_and_binding(shaders::sdf_vert.wgsl, "var<uniform> globals") == "@group(0) @binding(0)");
    // Entry points are "main" (what the device asks for).
    CHECK(shaders::sdf_vert.wgsl.find("fn main(") != std::string_view::npos);
}

TEST_CASE("every engine shader has generated WGSL with its bindings") {
    // Milestone 10.2: no hand-written WGSL; each engine shader's WGSL comes
    // from the same GLSL 450 and binds what its reflection tables say.
    const std::pair<const char*, const rhi::ShaderSource*> all[] = {
        {"light_vert", &shaders::light_vert}, {"light_frag", &shaders::light_frag},
        {"mesh_vert", &shaders::mesh_vert},   {"mesh_frag", &shaders::mesh_frag},
        {"sdf_vert", &shaders::sdf_vert},     {"sdf_frag", &shaders::sdf_frag},
        {"text_vert", &shaders::text_vert},   {"text_frag", &shaders::text_frag},
    };
    const bool compiled = rhi::backend_compiled_in(rhi::Backend::WebGPU);
    for (const auto& [name, shader] : all) {
        CAPTURE(name);
        const std::string_view wgsl = shader->wgsl;
        if (!compiled) {
            CHECK(wgsl.empty());
            continue;
        }
        REQUIRE_FALSE(wgsl.empty());
        const bool vertex = std::string_view(name).ends_with("_vert");
        CHECK(wgsl.find(vertex ? "@vertex" : "@fragment") != std::string_view::npos);
        CHECK(wgsl.find("fn main(") != std::string_view::npos);
        for (const rhi::UniformBlockBinding& block : shader->uniform_blocks) {
            CHECK(wgsl.find(std::format("@group(0) @binding({})", block.binding)) != std::string_view::npos);
        }
        for (const rhi::TextureBinding& texture : shader->textures) {
            CHECK(wgsl.find(std::format("@group(1) @binding({})", 2 * texture.binding)) != std::string_view::npos);
            CHECK(wgsl.find(std::format("@group(1) @binding({})", 2 * texture.binding + 1)) != std::string_view::npos);
        }
    }
}
