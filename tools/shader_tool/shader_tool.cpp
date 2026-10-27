// Build-time shader tool (DESIGN.md 4.4): turns one SPIR-V module (compiled by
// glslangValidator from the GLSL 450 source) into a C++ header holding
//   - the SPIR-V words (Vulkan backend),
//   - GLSL 330 core and GLSL ES 300 cross-compiled by SPIRV-Cross (GL backend),
//   - the uniform block and sampler name -> binding tables, because neither
//     GLSL 330 nor ES 300 can declare `layout(binding = N)`; the GL backend
//     applies them after linking (glUniformBlockBinding, glUniform1i),
//   - optionally the WGSL for the WebGPU backend (DESIGN.md 6, milestone 10:
//     naga translates SPIR-V to WGSL; cmake/Shaders.cmake runs it).
//
// Resource conventions (rhi.hpp): uniform blocks in set 0, combined samplers
// (sampler2D) in set 1; anything else is rejected here, at build time.
//
//   shader_tool <input.spv> <symbol> <output.hpp> [<input.wgsl>]
//   shader_tool --webgpu-bindings <split.spv> <output.spv>
//
// WGSL has no combined samplers, so the WebGPU path first splits them
// (spirv-opt --split-combined-image-sampler leaves the image and the sampler
// on the same binding); --webgpu-bindings then renumbers set 1 so that
// texture binding N becomes image 2N and sampler 2N + 1, the layout the
// WebGPU backend builds its bind groups for.

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

std::string read_text(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(std::string("cannot open ") + path);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_raw_string(std::ostream& out, const std::string& text, const std::string& delimiter = "glsl") {
    if (text.find(")" + delimiter + "\"") != std::string::npos) {
        throw std::runtime_error("generated " + delimiter + " contains the raw-string delimiter");
    }
    // Adjacent literals of at most ~8 KB: MSVC rejects a single one over ~16 KB (C2026)
    // and joins adjacent ones, as every other compiler does.
    std::size_t pos = 0;
    while (true) {
        std::size_t end = text.size();
        if (end - pos > 8000) {
            const std::size_t nl = text.find('\n', pos + 8000);
            if (nl != std::string::npos) end = nl + 1;
        }
        out << "R\"" << delimiter << "(" << text.substr(pos, end - pos) << ")" << delimiter << "\"";
        pos = end;
        if (pos >= text.size()) break;
        out << "\n";
    }
}

// --webgpu-bindings: patches the Binding decorations of the set-1 images and
// samplers in place (texture binding N -> image 2N, sampler 2N + 1).
void renumber_webgpu_bindings(std::vector<std::uint32_t>& words) {
    constexpr std::uint32_t op_type_image = 25, op_type_sampler = 26, op_type_pointer = 32, op_variable = 59,
                            op_decorate = 71;
    constexpr std::uint32_t decoration_binding = 33, decoration_set = 34;
    const std::uint32_t bound = words[3];
    std::vector<std::uint32_t> type_op(bound, 0);  // type id -> opcode that defined it
    std::vector<std::uint32_t> pointee(bound, 0);  // pointer type id -> pointee type id
    std::vector<std::uint32_t> var_type(bound, 0); // variable id -> its pointer type id
    std::vector<std::uint32_t> set(bound, ~0u);
    std::vector<std::size_t> binding_word(bound, 0);
    auto id = [&](std::uint32_t v) {
        if (v >= bound) throw std::runtime_error("SPIR-V id out of bounds");
        return v;
    };
    for (std::size_t i = 5; i < words.size();) {
        const std::uint32_t count = words[i] >> 16, op = words[i] & 0xffffu;
        if (count == 0 || i + count > words.size()) {
            throw std::runtime_error("malformed SPIR-V instruction stream");
        }
        if ((op == op_type_image || op == op_type_sampler) && count >= 2) type_op[id(words[i + 1])] = op;
        if (op == op_type_pointer && count >= 4) pointee[id(words[i + 1])] = id(words[i + 3]);
        if (op == op_variable && count >= 4) var_type[id(words[i + 2])] = id(words[i + 1]);
        if (op == op_decorate && count >= 4 && words[i + 2] == decoration_set) set[id(words[i + 1])] = words[i + 3];
        if (op == op_decorate && count >= 4 && words[i + 2] == decoration_binding) binding_word[id(words[i + 1])] = i + 3;
        i += count;
    }
    for (std::uint32_t v = 0; v < bound; ++v) {
        if (set[v] != 1 || binding_word[v] == 0 || var_type[v] == 0) continue;
        const std::uint32_t kind = type_op[pointee[var_type[v]]];
        std::uint32_t& binding = words[binding_word[v]];
        if (binding >= max_texture_bindings) {
            throw std::runtime_error("texture binding " + std::to_string(binding) + " out of range");
        }
        if (kind == op_type_image) {
            binding = binding * 2;
        } else if (kind == op_type_sampler) {
            binding = binding * 2 + 1;
        } else {
            throw std::runtime_error("set 1 holds something other than split images and samplers");
        }
    }
}

int webgpu_bindings(const char* input, const char* output) {
    try {
        std::vector<std::uint32_t> words = read_spirv(input);
        renumber_webgpu_bindings(words);
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error(std::string("cannot write ") + output);
        }
        for (const std::uint32_t w : words) {
            const char bytes[4] = {static_cast<char>(w & 0xffu), static_cast<char>((w >> 8) & 0xffu),
                                   static_cast<char>((w >> 16) & 0xffu), static_cast<char>(w >> 24)};
            file.write(bytes, 4);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "shader_tool: %s: %s\n", input, e.what());
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--webgpu-bindings") {
        return webgpu_bindings(argv[2], argv[3]);
    }
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: shader_tool <input.spv> <symbol> <output.hpp> [<input.wgsl>]\n"
                             "       shader_tool --webgpu-bindings <split.spv> <output.spv>\n");
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
        const std::string wgsl = argc == 5 ? read_text(argv[4]) : std::string();

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
        // Always named, even empty: GCC warns about a skipped designated member
        // (-Wmissing-field-initializers) in builds without naga.
        out << ",\n    .wgsl = ";
        if (wgsl.empty()) {
            out << "{}";
        } else {
            write_raw_string(out, wgsl, "wgsl");
        }
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
