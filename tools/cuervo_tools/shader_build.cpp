#include "tools/cuervo_tools/shader_build.hpp"

#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>

#include "cuervo/core/log.hpp"
#include "cuervo/render/user_shader.hpp"

namespace cuervo::tools {

namespace fs = std::filesystem;

namespace {

std::string program(const char* env, const char* built_in, const char* fallback) {
    if (const char* value = std::getenv(env); value && *value) return value;
    return built_in && *built_in ? built_in : fallback;
}

std::string quoted(const fs::path& p) { return "\"" + p.string() + "\""; }

bool run(const std::string& command, std::string& error, std::string_view what) {
#ifdef _WIN32
    const std::string full = "\"" + command + "\"";
#else
    const std::string& full = command;
#endif
    if (std::system(full.c_str()) != 0) {
        error = std::format("{} failed (command: {})", what, command);
        return false;
    }
    return true;
}

bool write_text(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

#ifndef CUERVO_TOOLS_SHADER_TOOL
#define CUERVO_TOOLS_SHADER_TOOL ""
#endif
#ifndef CUERVO_TOOLS_NAGA
#define CUERVO_TOOLS_NAGA ""
#endif
#ifndef CUERVO_TOOLS_SPIRV_OPT
#define CUERVO_TOOLS_SPIRV_OPT ""
#endif

} // namespace

std::optional<std::vector<std::byte>> build_cshader(std::string_view glsl_source, std::string_view label,
                                                    std::string& error, std::vector<std::string>* notes) {
    std::string wrap_error;
    auto wrapped = render::wrap_user_glsl(glsl_source, wrap_error);
    if (!wrapped) {
        error = std::format("{}: {}", label, wrap_error);
        return std::nullopt;
    }
    const std::string glslang = program("CUERVO_GLSLANG", "", "glslangValidator");
    const std::string shader_tool = program("CUERVO_SHADER_TOOL", CUERVO_TOOLS_SHADER_TOOL, "shader_tool");
    const std::string naga = program("CUERVO_NAGA", CUERVO_TOOLS_NAGA, "");
    const std::string spirv_opt = program("CUERVO_SPIRV_OPT", CUERVO_TOOLS_SPIRV_OPT, "");

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / std::format("cuervo_shader_{}", std::rand());
    fs::create_directories(dir, ec);
    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(dir, e);
        }
    } cleanup{dir};

    const fs::path frag = dir / "user.frag", spv = dir / "user.spv", out = dir / "user.cshader";
    if (!write_text(frag, wrapped->glsl)) {
        error = "can't write the temporary shader";
        return std::nullopt;
    }
    // The same invocation as cmake/Shaders.cmake (SPIR-V 1.0 for SPIRV-Cross).
    const std::string glslang_args = " -V --target-env vulkan1.0 --quiet ";
    if (!run(glslang + glslang_args + "-o " + quoted(spv) + " " + quoted(frag), error,
             std::format("compiling '{}' with {}", label, glslang))) {
        return std::nullopt;
    }
    std::string wgsl_arg;
    if (!naga.empty() && !spirv_opt.empty()) {
        const fs::path wspv = dir / "user.webgpu.spv", wgsl = dir / "user.wgsl";
        std::string ignored;
        if (run(glslang + glslang_args + "-DCUERVO_WEBGPU -o " + quoted(wspv) + " " + quoted(frag), ignored, "") &&
            run(spirv_opt + " --split-combined-image-sampler -o " + quoted(wspv) + " " + quoted(wspv), ignored, "") &&
            run(shader_tool + " --webgpu-bindings " + quoted(wspv) + " " + quoted(wspv), ignored, "") &&
            run(naga + " --keep-coordinate-space " + quoted(wspv) + " " + quoted(wgsl), ignored, "")) {
            wgsl_arg = " " + quoted(wgsl);
        }
    }
    if (wgsl_arg.empty() && notes) {
        notes->push_back(std::format("{}: no WGSL (naga-cli and spirv-opt not configured): it will not run on WebGPU", label));
    }
    if (!run(shader_tool + " --container " + quoted(spv) + " " + quoted(out) + wgsl_arg, error,
             std::format("translating '{}'", label))) {
        return std::nullopt;
    }
    std::ifstream in(out, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto compiled = render::decode_cshader(std::as_bytes(std::span(raw)));
    if (!compiled) {
        error = std::format("'{}': shader_tool wrote an invalid container", label);
        return std::nullopt;
    }
    compiled->exports = std::move(wrapped->exports);
    return render::encode_cshader(*compiled);
}

int build_project_shaders(const fs::path& project, std::string& error) {
    int built = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(project, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file() || it->path().extension() != ".glsl") continue;
        std::ifstream in(it->path(), std::ios::binary);
        const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string label = fs::relative(it->path(), project).generic_string();
        std::vector<std::string> notes;
        auto bytes = build_cshader(source, label, error, &notes);
        if (!bytes) return -1;
        for (const std::string& note : notes) LOG_WARN("{}", note);
        fs::path target = it->path();
        target.replace_extension(".cshader");
        std::ofstream out(target, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        if (!out) {
            error = std::format("can't write '{}'", target.string());
            return -1;
        }
        ++built;
    }
    return built;
}

} // namespace cuervo::tools
