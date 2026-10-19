// Build-time shader tool (DESIGN.md 4.4): turns one SPIR-V module (compiled by
// glslangValidator from the GLSL 450 source) into a C++ header holding
//   - the SPIR-V words (Vulkan backend),
//   - GLSL 330 core and GLSL ES 300 cross-compiled by SPIRV-Cross (GL backend),
//   - the uniform block and sampler name -> binding tables, because neither
//     GLSL 330 nor ES 300 can declare `layout(binding = N)`; the GL backend
//     applies them after linking (glUniformBlockBinding, glUniform1i).
//
// Resource conventions (rhi.hpp): uniform blocks in set 0, combined samplers
// (sampler2D) in set 1; anything else is rejected here, at build time.
//
//   shader_tool <input.spv> <symbol> <output.hpp>

#include <spirv_glsl.hpp>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct UniformBlock {
    std::string name;
    std::uint32_t binding = 0;
};

struct Reflection {
    std::vector<UniformBlock> blocks;
    std::vector<UniformBlock> textures;
};

// Must match rhi::max_uniform_bindings / rhi::max_texture_bindings.
constexpr std::uint32_t max_uniform_bindings = 4;
constexpr std::uint32_t max_texture_bindings = 4;

std::vector<std::uint32_t> read_spirv(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(std::string("cannot open ") + path);
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() % 4 != 0) {
        throw std::runtime_error(std::string(path) + " is not a SPIR-V module");
    }
    std::vector<std::uint32_t> words(bytes.size() / 4);
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto* b = reinterpret_cast<const unsigned char*>(bytes.data() + i * 4);
        words[i] = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
                   (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
    }
    if (words[0] != 0x07230203u) {
        throw std::runtime_error(std::string(path) + " has no SPIR-V magic number");
    }
    return words;
}

std::string cross_compile(const std::vector<std::uint32_t>& spirv, bool es, Reflection* reflection) {
    spirv_cross::CompilerGLSL compiler(spirv);
    spirv_cross::CompilerGLSL::Options options;
    options.version = es ? 300 : 330;
    options.es = es;
    // Keep the generated source identical in spirit for 330 and ES 300: no
    // `binding =` layouts (ES 300 can't have them), bindings come from the table.
    options.enable_420pack_extension = false;
    // SDF anti-aliasing needs full precision on mobile/web GPUs.
    options.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Highp;
    options.fragment.default_int_precision = spirv_cross::CompilerGLSL::Options::Highp;
    compiler.set_common_options(options);

    std::string source = compiler.compile();

    if (reflection) {
        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
        for (const spirv_cross::Resource& ubo : resources.uniform_buffers) {
            const std::uint32_t set = compiler.get_decoration(ubo.id, spv::DecorationDescriptorSet);
            const std::uint32_t binding = compiler.get_decoration(ubo.id, spv::DecorationBinding);
            if (set != 0 || binding >= max_uniform_bindings) {
                throw std::runtime_error("uniform block '" + ubo.name + "' must use set 0, binding < " +
                                         std::to_string(max_uniform_bindings));
            }
            reflection->blocks.push_back({compiler.get_remapped_declared_block_name(ubo.id), binding});
        }
        for (const spirv_cross::Resource& texture : resources.sampled_images) {
            const std::uint32_t set = compiler.get_decoration(texture.id, spv::DecorationDescriptorSet);
            const std::uint32_t binding = compiler.get_decoration(texture.id, spv::DecorationBinding);
            if (set != 1 || binding >= max_texture_bindings) {
                throw std::runtime_error("sampler '" + texture.name + "' must use set 1, binding < " +
                                         std::to_string(max_texture_bindings));
            }
            reflection->textures.push_back({compiler.get_name(texture.id), binding});
        }
        if (!resources.separate_images.empty() || !resources.separate_samplers.empty() ||
            !resources.storage_buffers.empty() || !resources.storage_images.empty()) {
            throw std::runtime_error("only uniform blocks and combined samplers (sampler2D) are supported");
        }
    }
    return source;
}

void write_raw_string(std::ostream& out, const std::string& text) {
    if (text.find(")glsl\"") != std::string::npos) {
        throw std::runtime_error("generated GLSL contains the raw-string delimiter");
    }
    out << "R\"glsl(" << text << ")glsl\"";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: shader_tool <input.spv> <symbol> <output.hpp>\n");
        return 2;
    }
    const std::string symbol = argv[2];
    try {
        const std::vector<std::uint32_t> spirv = read_spirv(argv[1]);
        Reflection reflection;
        const std::string glsl330 = cross_compile(spirv, false, &reflection);
        const std::vector<UniformBlock>& blocks = reflection.blocks;
        const std::vector<UniformBlock>& textures = reflection.textures;
        const std::string glsl_es300 = cross_compile(spirv, true, nullptr);

        std::ostringstream out;
        out << "// Generated by shader_tool from " << argv[1] << ". Do not edit.\n"
            << "#pragma once\n\n"
            << "#include <cstdint>\n\n"
            << "#include \"cuervo/rhi/rhi.hpp\"\n\n"
            << "namespace cuervo::shaders {\n\n"
            << "namespace detail_" << symbol << " {\n"
            << "inline constexpr std::uint32_t spirv[] = {";
        for (std::size_t i = 0; i < spirv.size(); ++i) {
            out << (i % 8 == 0 ? "\n    " : " ") << "0x" << std::hex << spirv[i] << std::dec << "u,";
        }
        out << "\n};\n";
        if (!blocks.empty()) {
            out << "inline constexpr rhi::UniformBlockBinding uniform_blocks[] = {\n";
            for (const UniformBlock& block : blocks) {
                out << "    {\"" << block.name << "\", " << block.binding << "},\n";
            }
            out << "};\n";
        }
        if (!textures.empty()) {
            out << "inline constexpr rhi::TextureBinding textures[] = {\n";
            for (const UniformBlock& texture : textures) {
                out << "    {\"" << texture.name << "\", " << texture.binding << "},\n";
            }
            out << "};\n";
        }
        out << "} // namespace detail_" << symbol << "\n\n"
            << "inline constexpr rhi::ShaderSource " << symbol << "{\n"
            << "    .spirv = detail_" << symbol << "::spirv,\n"
            << "    .glsl330 = ";
        write_raw_string(out, glsl330);
        out << ",\n    .glsl_es300 = ";
        write_raw_string(out, glsl_es300);
        out << ",\n    .uniform_blocks = ";
        if (blocks.empty()) {
            out << "{}";
        } else {
            out << "detail_" << symbol << "::uniform_blocks";
        }
        out << ",\n    .textures = ";
        if (textures.empty()) {
            out << "{}";
        } else {
            out << "detail_" << symbol << "::textures";
        }
        out << ",\n};\n\n} // namespace cuervo::shaders\n";

        std::ofstream file(argv[3], std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error(std::string("cannot write ") + argv[3]);
        }
        file << out.str();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "shader_tool: %s: %s\n", argv[1], e.what());
        return 1;
    }
    return 0;
}
