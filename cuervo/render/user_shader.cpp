#include "cuervo/render/user_shader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>

namespace cuervo::render {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

int components_of(std::string_view type) {
    if (type == "float") return 1;
    if (type == "vec2") return 2;
    if (type == "vec3") return 3;
    if (type == "vec4") return 4;
    return 0;
}

// Numbers in "vec3(1.0, 0.8, .6)" or "0.5": identifiers are skipped.
std::vector<float> numbers_in(std::string_view text) {
    std::vector<float> out;
    const std::string s(text);
    const char* p = s.c_str();
    while (*p) {
        const bool starts_number = std::isdigit(static_cast<unsigned char>(*p)) ||
                                   ((*p == '-' || *p == '.') && (std::isdigit(static_cast<unsigned char>(p[1])) || p[1] == '.'));
        if (starts_number) {
            char* end = nullptr;
            out.push_back(std::strtof(p, &end));
            p = end > p ? end : p + 1;
        } else if (std::isalpha(static_cast<unsigned char>(*p)) || *p == '_') {
            while (std::isalnum(static_cast<unsigned char>(*p)) || *p == '_') ++p;
        } else {
            ++p;
        }
    }
    return out;
}

// The fixed interface; keep in sync with post.vert and post_composite.frag.
constexpr std::string_view prelude = R"(#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_screen;
layout(std140, set = 0, binding = 0) uniform PostParams {
    vec4 v0;
    vec4 v1;
    vec4 v2;
    vec4 v3; // x: encode the output to sRGB (linear color on a window that does not), w: engine
} params;
layout(set = 1, binding = 0) uniform sampler2D u_src;
layout(set = 1, binding = 1) uniform sampler2D u_aux;
layout(location = 0) out vec4 o_color;
#define main user_main
)";

constexpr std::string_view epilogue = R"(
#undef main
vec3 cuervo_to_srgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}
void main() {
    user_main();
    if (params.v3.x > 0.5) {
        o_color.rgb = cuervo_to_srgb(o_color.rgb);
    }
}
)";

} // namespace

std::optional<WrappedShader> wrap_user_glsl(std::string_view source, std::string& error) {
    WrappedShader out;
    std::array<int, 3> used{}; // components taken in v0, v1, v2
    std::string body;
    std::string defines;
    int line_number = 0;
    std::size_t pos = 0;
    while (pos <= source.size()) {
        const std::size_t end = std::min(source.find('\n', pos), source.size());
        const std::string_view line = source.substr(pos, end - pos);
        ++line_number;
        pos = end + 1;
        const std::string_view t = trim(line);
        if (t.starts_with("#version")) {
            error = std::format("line {}: do not write #version (the engine adds it)", line_number);
            return std::nullopt;
        }
        if (!t.starts_with("@export")) {
            body.append(line);
            body += '\n';
            continue;
        }
        body += '\n'; // keep line numbers of compiler messages
        std::string_view decl = trim(t.substr(7));
        if (decl.ends_with(';')) decl.remove_suffix(1);
        const std::size_t eq = decl.find('=');
        const std::string_view head = trim(decl.substr(0, eq));
        const std::size_t space = head.find_last_of(" \t");
        if (space == std::string_view::npos) {
            error = std::format("line {}: expected '@export <float|vec2|vec3|vec4> <name> [= default];'", line_number);
            return std::nullopt;
        }
        ExportedUniform u;
        u.name = std::string(trim(head.substr(space)));
        u.components = components_of(trim(head.substr(0, space)));
        const bool name_ok = !u.name.empty() && (std::isalpha(static_cast<unsigned char>(u.name[0])) || u.name[0] == '_') &&
                             std::all_of(u.name.begin(), u.name.end(), [](char c) {
                                 return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
                             });
        if (u.components == 0 || !name_ok) {
            error = std::format("line {}: '@export' takes float, vec2, vec3 or vec4 and a plain name", line_number);
            return std::nullopt;
        }
        if (std::any_of(out.exports.begin(), out.exports.end(), [&](const ExportedUniform& e) { return e.name == u.name; })) {
            error = std::format("line {}: '{}' is exported twice", line_number, u.name);
            return std::nullopt;
        }
        if (eq != std::string_view::npos) {
            const std::vector<float> values = numbers_in(decl.substr(eq + 1));
            for (int i = 0; i < u.components; ++i) {
                u.defaults[i] = values.empty() ? 0.0f : values[std::min<std::size_t>(static_cast<std::size_t>(i), values.size() - 1)];
            }
        }
        // First vector with room: a vecN never straddles two vectors.
        int vector_index = -1;
        for (int k = 0; k < 3 && vector_index < 0; ++k) {
            if (used[static_cast<std::size_t>(k)] + u.components <= 4) vector_index = k;
        }
        if (vector_index < 0) {
            error = std::format("line {}: too many exported uniforms (the limit is 12 floats)", line_number);
            return std::nullopt;
        }
        const int first = used[static_cast<std::size_t>(vector_index)];
        used[static_cast<std::size_t>(vector_index)] += u.components;
        u.slot = vector_index * 4 + first;
        static constexpr std::string_view swizzle = "xyzw";
        defines += std::format("#define {} (params.v{}.{})\n", u.name, vector_index, swizzle.substr(static_cast<std::size_t>(first), static_cast<std::size_t>(u.components)));
        out.exports.push_back(std::move(u));
    }
    out.glsl = std::string(prelude) + defines + "#line 1\n" + body + std::string(epilogue);
    return out;
}

// --- .cshader ---------------------------------------------------------------------

namespace {

constexpr std::array<char, 4> magic{'C', 'S', 'H', 'D'};
constexpr std::uint32_t format_version = 1;

struct Writer {
    std::vector<std::byte> out;
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xff));
    }
    void str(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        for (char c : s) out.push_back(static_cast<std::byte>(c));
    }
    void f32(float v) {
        std::uint32_t bits;
        std::memcpy(&bits, &v, 4);
        u32(bits);
    }
};

struct Reader {
    std::span<const std::byte> in;
    std::size_t pos = 0;
    bool ok = true;
    std::uint32_t u32() {
        if (pos + 4 > in.size()) {
            ok = false;
            return 0;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(in[pos + static_cast<std::size_t>(i)]) << (8 * i);
        pos += 4;
        return v;
    }
    float f32() {
        const std::uint32_t bits = u32();
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }
    std::string str() {
        const std::uint32_t n = u32();
        if (!ok || n > in.size() - pos) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(in.data() + pos), n);
        pos += n;
        return s;
    }
};

} // namespace

std::vector<std::byte> encode_cshader(const CompiledShader& shader) {
    Writer w;
    for (char c : magic) w.out.push_back(static_cast<std::byte>(c));
    w.u32(format_version);
    w.u32(static_cast<std::uint32_t>(shader.spirv.size()));
    for (std::uint32_t word : shader.spirv) w.u32(word);
    w.str(shader.glsl330);
    w.str(shader.glsl_es300);
    w.str(shader.wgsl);
    for (const auto* list : {&shader.uniform_blocks, &shader.textures}) {
        w.u32(static_cast<std::uint32_t>(list->size()));
        for (const auto& [name, binding] : *list) {
            w.str(name);
            w.u32(binding);
        }
    }
    w.u32(static_cast<std::uint32_t>(shader.exports.size()));
    for (const ExportedUniform& e : shader.exports) {
        w.str(e.name);
        w.u32(static_cast<std::uint32_t>(e.components));
        w.u32(static_cast<std::uint32_t>(e.slot));
        for (float d : e.defaults) w.f32(d);
    }
    return std::move(w.out);
}

std::optional<CompiledShader> decode_cshader(std::span<const std::byte> bytes) {
    Reader r{bytes};
    if (bytes.size() < 8 || std::memcmp(bytes.data(), magic.data(), 4) != 0) return std::nullopt;
    r.pos = 4;
    if (r.u32() != format_version) return std::nullopt;
    CompiledShader s;
    const std::uint32_t words = r.u32();
    if (!r.ok || words > (bytes.size() - r.pos) / 4) return std::nullopt;
    s.spirv.reserve(words);
    for (std::uint32_t i = 0; i < words; ++i) s.spirv.push_back(r.u32());
    s.glsl330 = r.str();
    s.glsl_es300 = r.str();
    s.wgsl = r.str();
    for (auto* list : {&s.uniform_blocks, &s.textures}) {
        const std::uint32_t n = r.u32();
        for (std::uint32_t i = 0; i < n && r.ok; ++i) {
            std::string name = r.str();
            const std::uint32_t binding = r.u32();
            list->emplace_back(std::move(name), binding);
        }
    }
    const std::uint32_t n = r.u32();
    for (std::uint32_t i = 0; i < n && r.ok; ++i) {
        ExportedUniform e;
        e.name = r.str();
        e.components = static_cast<int>(r.u32());
        e.slot = static_cast<int>(r.u32());
        for (float& d : e.defaults) d = r.f32();
        if (e.components < 1 || e.components > 4 || e.slot < 0 || e.slot + e.components > 12) return std::nullopt;
        s.exports.push_back(std::move(e));
    }
    if (!r.ok || r.pos != bytes.size() || s.spirv.empty()) return std::nullopt;
    return s;
}

UserShader::UserShader(CompiledShader compiled) : compiled_(std::move(compiled)) {
    for (const auto& [name, binding] : compiled_.uniform_blocks) blocks_.push_back({name, binding});
    for (const auto& [name, binding] : compiled_.textures) textures_.push_back({name, binding});
    source_.spirv = compiled_.spirv;
    source_.glsl330 = compiled_.glsl330;
    source_.glsl_es300 = compiled_.glsl_es300;
    source_.wgsl = compiled_.wgsl;
    source_.uniform_blocks = blocks_;
    source_.textures = textures_;
}

PostParams UserShader::defaults() const {
    PostParams p;
    for (const ExportedUniform& e : compiled_.exports) {
        for (int i = 0; i < e.components; ++i) {
            float* vec = e.slot + i < 4 ? p.v0 : e.slot + i < 8 ? p.v1 : p.v2;
            vec[(e.slot + i) % 4] = e.defaults[i];
        }
    }
    return p;
}

bool UserShader::set(PostParams& params, std::string_view name, std::span<const float> values) const {
    for (const ExportedUniform& e : compiled_.exports) {
        if (e.name != name) continue;
        for (int i = 0; i < e.components && static_cast<std::size_t>(i) < values.size(); ++i) {
            float* vec = e.slot + i < 4 ? params.v0 : e.slot + i < 8 ? params.v1 : params.v2;
            vec[(e.slot + i) % 4] = values[static_cast<std::size_t>(i)];
        }
        return true;
    }
    return false;
}

} // namespace cuervo::render
