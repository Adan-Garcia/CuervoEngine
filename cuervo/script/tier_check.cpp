#include "cuervo/script/tier_check.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <fstream>
#include <sstream>
#include <tuple>

#include "cuervo/platform/platform.hpp"
#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/project.hpp"
#include "pluma/lexer.hpp"

namespace cuervo::script {

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t large_download_bytes = 100ull * 1024 * 1024;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool read_file(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// --- shaders --------------------------------------------------------------------

struct ShaderRule {
    std::string_view word; // identifier (or prefix when it ends with '*')
    std::string_view what;
};

// GLSL constructs with no GLES 3.0 / WebGL2 equivalent (DESIGN.md 4.4). The
// GL backend cross-compiles GLSL 450 to GLSL ES 300 with SPIRV-Cross, which
// fails on all of these; on Vulkan they would work, hence Full-only.
constexpr ShaderRule shader_rules[] = {
    {"buffer", "storage buffers (SSBOs) need GLES 3.1"},
    {"std430", "storage buffers (SSBOs) need GLES 3.1"},
    {"image1D*", "image load/store needs GLES 3.1"},
    {"image2D*", "image load/store needs GLES 3.1"},
    {"image3D*", "image load/store needs GLES 3.1"},
    {"imageCube*", "image load/store needs GLES 3.1"},
    {"iimage*", "image load/store needs GLES 3.1"},
    {"uimage*", "image load/store needs GLES 3.1"},
    {"imageLoad", "image load/store needs GLES 3.1"},
    {"imageStore", "image load/store needs GLES 3.1"},
    {"imageSize", "image load/store needs GLES 3.1"},
    {"imageAtomic*", "image atomics need GLES 3.1"},
    {"atomicAdd", "atomics need storage buffers (GLES 3.1)"},
    {"atomicMin", "atomics need storage buffers (GLES 3.1)"},
    {"atomicMax", "atomics need storage buffers (GLES 3.1)"},
    {"atomicAnd", "atomics need storage buffers (GLES 3.1)"},
    {"atomicOr", "atomics need storage buffers (GLES 3.1)"},
    {"atomicXor", "atomics need storage buffers (GLES 3.1)"},
    {"atomicExchange", "atomics need storage buffers (GLES 3.1)"},
    {"atomicCompSwap", "atomics need storage buffers (GLES 3.1)"},
    {"atomic_uint", "atomic counters need GLES 3.1"},
    {"shared", "compute shared memory (no compute in GLES 3.0)"},
    {"barrier", "compute barriers (no compute in GLES 3.0)"},
    {"memoryBarrier*", "memory barriers need GLES 3.1"},
    {"groupMemoryBarrier", "memory barriers need GLES 3.1"},
    {"gl_GlobalInvocationID", "compute built-ins (no compute in GLES 3.0)"},
    {"gl_LocalInvocationID", "compute built-ins (no compute in GLES 3.0)"},
    {"gl_LocalInvocationIndex", "compute built-ins (no compute in GLES 3.0)"},
    {"gl_WorkGroupID", "compute built-ins (no compute in GLES 3.0)"},
    {"gl_NumWorkGroups", "compute built-ins (no compute in GLES 3.0)"},
    {"nonuniformEXT", "descriptor indexing (bindless) has no GL equivalent"},
    {"GL_EXT_nonuniform_qualifier", "descriptor indexing (bindless) has no GL equivalent"},
    {"gl_BaseVertex*", "draw parameters (GLES 3.0 has no base vertex/instance)"},
    {"gl_BaseInstance*", "draw parameters (GLES 3.0 has no base vertex/instance)"},
    {"gl_DrawID*", "draw parameters (no multi-draw in GLES 3.0)"},
    {"subgroup*", "subgroup operations have no GLES 3.0 equivalent"},
    {"GL_KHR_shader_subgroup*", "subgroup operations have no GLES 3.0 equivalent"},
    {"double", "64-bit floats are not in GLSL ES"},
    {"dvec2", "64-bit floats are not in GLSL ES"},
    {"dvec3", "64-bit floats are not in GLSL ES"},
    {"dvec4", "64-bit floats are not in GLSL ES"},
    {"dmat*", "64-bit floats are not in GLSL ES"},
    {"push_constant", "push constants are not part of the RHI (use a uniform block)"},
    {"textureGather*", "textureGather needs GLES 3.1"},
    {"samplerCubeArray*", "cube map arrays need GLES 3.2"},
    {"samplerBuffer", "texture buffers need GLES 3.2"},
    {"isamplerBuffer", "texture buffers need GLES 3.2"},
    {"usamplerBuffer", "texture buffers need GLES 3.2"},
    {"sampler2DMS*", "multisample textures need GLES 3.1"},
    {"gl_PrimitiveID", "gl_PrimitiveID needs GLES 3.2 (geometry shaders)"},
    {"gl_Layer", "layered rendering needs GLES 3.2"},
    {"gl_ViewportIndex", "multiple viewports are not in GLES"},
};

bool word_matches(std::string_view rule, std::string_view word) {
    if (rule.ends_with('*')) return word.starts_with(rule.substr(0, rule.size() - 1));
    return word == rule;
}

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

TierIssue issue(Severity severity, std::string file, std::uint32_t line, std::uint32_t column, std::string message) {
    return {severity, std::move(file), line, column, std::move(message)};
}

// --- assets ---------------------------------------------------------------------

std::uint32_t be32(std::span<const std::byte> d, std::size_t at) {
    return (std::to_integer<std::uint32_t>(d[at]) << 24) | (std::to_integer<std::uint32_t>(d[at + 1]) << 16) |
           (std::to_integer<std::uint32_t>(d[at + 2]) << 8) | std::to_integer<std::uint32_t>(d[at + 3]);
}
std::uint32_t be16(std::span<const std::byte> d, std::size_t at) {
    return (std::to_integer<std::uint32_t>(d[at]) << 8) | std::to_integer<std::uint32_t>(d[at + 1]);
}

bool has_ext(const fs::path& p, std::initializer_list<std::string_view> exts) {
    const std::string e = lower(p.extension().string());
    return std::find(exts.begin(), exts.end(), e) != exts.end();
}

std::string rel(const fs::path& root, const fs::path& p) { return platform::utf8(p.lexically_relative(root)); }

// Every regular file under `dir`, sorted (deterministic report order).
std::vector<fs::path> files_under(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void scan_scene_value(const scene::Value& v, const std::string& file, std::uint32_t node, rhi::Tier target,
                      std::span<const ApiRule> rules, std::vector<TierIssue>& out) {
    if (v.kind == scene::ValueKind::Enum) {
        const std::string full = v.type + "." + v.text;
        for (const ApiRule& r : rules) {
            if (target < r.tier && r.pattern == full) {
                out.push_back(issue(r.severity, file, 0, 0, std::format("node #{}: {} ({})", node, r.message, full)));
            }
        }
    }
    for (const scene::Field& f : v.fields) scan_scene_value(f.value, file, node, target, rules, out);
    for (const scene::Value& item : v.items) scan_scene_value(item, file, node, target, rules, out);
}

} // namespace

std::string_view severity_name(Severity severity) {
    switch (severity) {
        case Severity::Error:   return "error";
        case Severity::Warning: return "warning";
        case Severity::Note:    return "note";
    }
    return "note";
}

const TierRules& default_tier_rules() {
    static const TierRules rules{
        .script_api =
            {
                // Web caveat: Compatible is the tier that exports to the web,
                // where the file system is an in-memory copy of the project.
                {"Scene.save", rhi::Tier::Full, Severity::Note,
                 "on the web export Scene.save writes to the in-memory file system: the file is gone when the "
                 "page reloads"},
            },
        .scene_api = {},
    };
    return rules;
}

std::size_t TierCheckReport::count(Severity s) const {
    return static_cast<std::size_t>(std::count_if(issues.begin(), issues.end(), [s](const TierIssue& i) { return i.severity == s; }));
}

std::vector<TierIssue> scan_shader_source(std::string_view file, std::string_view text, rhi::Tier target) {
    std::vector<TierIssue> out;
    if (target == rhi::Tier::Full) return out;
    std::uint32_t line = 1;
    std::size_t line_start = 0;
    std::uint32_t last_line = 0;
    std::string_view last_what;
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '\n') {
            ++line;
            line_start = ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
                if (text[i] == '\n') {
                    ++line;
                    line_start = i + 1;
                }
                ++i;
            }
            i += 2;
        } else if (ident_start(c)) {
            const std::size_t start = i;
            while (i < text.size() && ident_char(text[i])) ++i;
            const std::string_view word = text.substr(start, i - start);
            for (const ShaderRule& r : shader_rules) {
                if (!word_matches(r.word, word)) continue;
                // One report per construct per line.
                if (line != last_line || r.what != last_what) {
                    out.push_back(issue(Severity::Error, std::string(file), line,
                                        static_cast<std::uint32_t>(start - line_start + 1),
                                        std::format("'{}': {} (needs the full tier)", word, r.what)));
                    last_line = line;
                    last_what = r.what;
                }
                break;
            }
        } else {
            ++i;
        }
    }
    return out;
}

std::vector<TierIssue> scan_script_source(std::string_view file, std::string_view text, rhi::Tier target,
                                          std::span<const ApiRule> rules) {
    std::vector<TierIssue> out;
    const std::vector<pluma::Token> tokens = pluma::Lexer::tokenize(text);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (!tokens[i].is(pluma::TokenKind::Identifier)) continue;
        // Previous token a dot: this identifier is a member, not a path start.
        if (i > 0 && (tokens[i - 1].is(pluma::TokenKind::Dot) || tokens[i - 1].is(pluma::TokenKind::QuestionDot))) continue;
        for (const ApiRule& r : rules) {
            if (target >= r.tier) continue;
            // Match "A.b.c" against Identifier (Dot Identifier)*.
            std::size_t t = i;
            std::string_view rest = r.pattern;
            bool matched = true;
            while (true) {
                const std::size_t dot = rest.find('.');
                const std::string_view part = rest.substr(0, dot);
                if (t >= tokens.size() || !tokens[t].is(pluma::TokenKind::Identifier) || tokens[t].text != part) {
                    matched = false;
                    break;
                }
                if (dot == std::string_view::npos) break;
                rest = rest.substr(dot + 1);
                if (t + 1 >= tokens.size() || !tokens[t + 1].is(pluma::TokenKind::Dot)) {
                    matched = false;
                    break;
                }
                t += 2;
            }
            if (matched) {
                out.push_back(issue(r.severity, std::string(file), tokens[i].line, tokens[i].column,
                                    std::format("{}: {}", r.pattern, r.message)));
            }
        }
    }
    return out;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> image_size(std::span<const std::byte> d) {
    // PNG: signature, then the IHDR chunk (width, height big-endian).
    static constexpr std::array<std::uint8_t, 8> png_sig = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (d.size() >= 24 && std::equal(png_sig.begin(), png_sig.end(), d.begin(),
                                     [](std::uint8_t a, std::byte b) { return a == std::to_integer<std::uint8_t>(b); })) {
        return std::pair{be32(d, 16), be32(d, 20)};
    }
    // JPEG: walk the segments to the first start-of-frame (SOF0..SOF15 minus DHT/JPG/DAC).
    if (d.size() >= 4 && d[0] == std::byte{0xff} && d[1] == std::byte{0xd8}) {
        std::size_t at = 2;
        while (at + 9 < d.size()) {
            if (d[at] != std::byte{0xff}) return std::nullopt;
            const auto marker = std::to_integer<std::uint32_t>(d[at + 1]);
            if (marker == 0xff) {
                ++at;
                continue;
            }
            const std::uint32_t length = be16(d, at + 2);
            if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
                return std::pair{be16(d, at + 7), be16(d, at + 5)};
            }
            at += 2 + length;
        }
    }
    return std::nullopt;
}

std::optional<TierCheckReport> check_project_tier(const fs::path& dir, std::optional<rhi::Tier> target,
                                                  std::string& error, const TierRules& rules,
                                                  std::optional<support::Target> platform, bool native_scripts) {
    std::optional<Project> project = load_project(dir, error);
    if (!project) return std::nullopt;
    TierCheckReport report;
    report.project = dir;
    report.declared = project->render_tier;
    report.target = target.value_or(project->render_tier);
    const rhi::Tier to = report.target;
    auto& out = report.issues;

    // project.toml
    if (report.declared != to) {
        out.push_back(issue(Severity::Note, "project.toml", static_cast<std::uint32_t>(project->render_tier_line), 0,
                            std::format("render_tier changes from \"{}\" to \"{}\" (cuervo-tools tier set <project> {})",
                                        rhi::tier_name(report.declared), rhi::tier_name(to), rhi::tier_name(to))));
    }
    if (to == rhi::Tier::Full) {
        out.push_back(issue(Severity::Note, "project.toml", static_cast<std::uint32_t>(project->render_tier_line), 0,
                            "the full tier runs on desktop Vulkan only: no web export, and the OpenGL backend refuses "
                            "to run the project"));
    }

    // The platform must provide what the project claims (docs/support.json).
    if (platform) {
        for (TierIssue& i : support::check_claims({to, native_scripts}, *platform)) {
            i.line = static_cast<std::uint32_t>(project->render_tier_line);
            out.push_back(std::move(i));
        }
    }

    // shaders/
    for (const fs::path& p : files_under(dir / "shaders")) {
        const std::string name = rel(dir, p);
        if (to == rhi::Tier::Compatible && has_ext(p, {".comp", ".geom", ".tesc", ".tese", ".mesh", ".task"})) {
            out.push_back(issue(Severity::Error, name, 0, 0,
                                std::format("{} shaders don't exist in GLES 3.0 / WebGL2 (needs the full tier)",
                                            has_ext(p, {".comp"})   ? "compute"
                                            : has_ext(p, {".geom"}) ? "geometry"
                                            : has_ext(p, {".mesh", ".task"}) ? "mesh/task"
                                                                    : "tessellation")));
            continue;
        }
        if (!has_ext(p, {".vert", ".frag", ".glsl"})) continue;
        std::string text;
        if (!read_file(p, text)) {
            out.push_back(issue(Severity::Error, name, 0, 0, "can't be read"));
            continue;
        }
        for (TierIssue& i : scan_shader_source(name, text, to)) out.push_back(std::move(i));
    }

    // scripts/
    std::uint64_t bytes = 0;
    std::error_code ec;
    for (const fs::path& p : files_under(dir / "scripts")) {
        bytes += fs::file_size(p, ec);
        if (!has_ext(p, {".plm"})) continue;
        std::string text;
        const std::string name = rel(dir, p);
        if (!read_file(p, text)) {
            out.push_back(issue(Severity::Error, name, 0, 0, "can't be read"));
            continue;
        }
        for (TierIssue& i : scan_script_source(name, text, to, rules.script_api)) out.push_back(std::move(i));
    }

    // scenes/
    for (const fs::path& p : files_under(dir / "scenes")) {
        bytes += fs::file_size(p, ec);
        if (!has_ext(p, {".scene"})) continue;
        const std::string name = rel(dir, p);
        std::string scene_error;
        std::optional<scene::SceneDoc> doc = scene::load_file(p, scene_error);
        if (!doc) {
            out.push_back(issue(Severity::Error, name, 0, 0, "can't be loaded: " + scene_error));
            continue;
        }
        for (std::size_t n = 0; n < doc->nodes.size(); ++n) {
            const scene::Node& node = doc->nodes[n];
            const auto index = static_cast<std::uint32_t>(n);
            for (const ApiRule& r : rules.scene_api) {
                if (to < r.tier && r.pattern == node.type) {
                    out.push_back(issue(r.severity, name, 0, 0,
                                        std::format("node #{} '{}' ({}): {}", index, node.name, node.type, r.message)));
                }
            }
            for (const scene::Field& f : node.fields) scan_scene_value(f.value, name, index, to, rules.scene_api, out);
        }
    }

    // assets/
    const rhi::Limits compatible = rhi::compatible_limits();
    for (const fs::path& p : files_under(dir / "assets")) {
        bytes += fs::file_size(p, ec);
        const std::string name = rel(dir, p);
        if (has_ext(p, {".png", ".jpg", ".jpeg"})) {
            std::ifstream in(p, std::ios::binary);
            std::vector<std::byte> head(64 * 1024);
            in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
            head.resize(static_cast<std::size_t>(in.gcount()));
            const auto size = image_size(head);
            if (!size) {
                out.push_back(issue(Severity::Warning, name, 0, 0, "unrecognized image header"));
            } else if (to == rhi::Tier::Compatible &&
                       (size->first > compatible.max_texture_size || size->second > compatible.max_texture_size)) {
                out.push_back(issue(Severity::Error, name, 0, 0,
                                    std::format("{}x{} texture exceeds the compatible tier's {} px limit (split it "
                                                "into an atlas or downscale it)",
                                                size->first, size->second, compatible.max_texture_size)));
            }
        } else if (has_ext(p, {".ttf", ".otf"}) && to == rhi::Tier::Compatible) {
            out.push_back(issue(Severity::Note, name, 0, 0,
                                "web builds can't import TTF/OTF at runtime (no FreeType): ship an .efont atlas "
                                "(cuervo --font <file> --export <file.efont>)"));
        } else if (has_ext(p, {".opus", ".aif", ".aiff", ".m4a", ".aac", ".wma", ".mid", ".midi", ".mod", ".xm", ".it",
                               ".s3m"})) {
            out.push_back(issue(Severity::Warning, name, 0, 0,
                                "audio format not decoded by the engine (WAV, OGG Vorbis, FLAC, MP3)"));
        } else if (has_ext(p, {".dds", ".ktx", ".ktx2", ".basis", ".exr", ".hdr", ".tga", ".psd"})) {
            out.push_back(issue(Severity::Warning, name, 0, 0, "image format not loaded by the engine"));
        }
    }

    bytes += fs::file_size(dir / "project.toml", ec);
    report.download_bytes = bytes;
    if (to == rhi::Tier::Compatible && bytes > large_download_bytes) {
        out.push_back(issue(Severity::Warning, "", 0, 0,
                            std::format("the web export preloads {:.1f} MB before the game starts",
                                        static_cast<double>(bytes) / (1024.0 * 1024.0))));
    }

    std::stable_sort(out.begin(), out.end(), [](const TierIssue& a, const TierIssue& b) {
        return std::tie(a.file, a.line, a.column) < std::tie(b.file, b.line, b.column);
    });
    return report;
}

std::string format_tier_report(const TierCheckReport& r) {
    std::string s = std::format("Tier check: {} (render_tier = \"{}\", checking for \"{}\")\n",
                                platform::utf8(r.project), rhi::tier_name(r.declared), rhi::tier_name(r.target));
    for (const TierIssue& i : r.issues) {
        std::string where = i.file.empty() ? std::string("(project)") : i.file;
        if (i.line > 0) where += std::format(":{}", i.line);
        if (i.column > 0) where += std::format(":{}", i.column);
        s += std::format("  {:<8}{}: {}\n", severity_name(i.severity), where, i.message);
    }
    if (r.target == rhi::Tier::Compatible) {
        s += std::format("Web export download: {:.1f} MB (project.toml, scripts, scenes, assets)\n",
                         static_cast<double>(r.download_bytes) / (1024.0 * 1024.0));
    }
    const std::size_t e = r.count(Severity::Error), w = r.count(Severity::Warning), n = r.count(Severity::Note);
    s += std::format("Result: {} error{}, {} warning{}, {} note{} -- {}\n", e, e == 1 ? "" : "s", w, w == 1 ? "" : "s", n,
                     n == 1 ? "" : "s",
                     e == 0 ? std::format("the project fits the {} tier", rhi::tier_name(r.target))
                            : std::format("the project does NOT fit the {} tier", rhi::tier_name(r.target)));
    return s;
}

bool write_project_tier(const fs::path& dir, rhi::Tier tier, std::string& error) {
    const fs::path file = dir / "project.toml";
    std::string text;
    if (!read_file(file, text)) {
        error = std::format("cannot read '{}'", platform::utf8(file));
        return false;
    }
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines;
    for (std::size_t pos = 0; pos < text.size();) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        pos = end + 1;
    }
    const std::string setting = std::format("render_tier = \"{}\"", rhi::tier_name(tier));
    std::size_t last_key = std::string::npos;
    bool done = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string_view l = lines[i];
        const std::size_t first = l.find_first_not_of(" \t");
        if (first == std::string_view::npos || l[first] == '#') continue;
        if (l[first] == '[') break; // top-level keys end at the first section
        const std::size_t eq = l.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view key = l.substr(first, eq - first);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.remove_suffix(1);
        last_key = i;
        if (key == "render_tier") {
            // Keep a trailing comment.
            std::string comment;
            bool in_str = false;
            for (std::size_t k = eq; k < l.size(); ++k) {
                if (l[k] == '"') in_str = !in_str;
                if (l[k] == '#' && !in_str) {
                    comment = "  " + std::string(l.substr(k));
                    break;
                }
            }
            lines[i] = setting + comment;
            done = true;
            break;
        }
    }
    if (!done) {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(last_key == std::string::npos ? 0 : last_key + 1),
                     setting);
    }
    std::string result;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        result += lines[i];
        if (i + 1 < lines.size() || text.ends_with('\n')) result += eol;
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(result.data(), static_cast<std::streamsize>(result.size()))) {
        error = std::format("cannot write '{}'", platform::utf8(file));
        return false;
    }
    return true;
}

} // namespace cuervo::script
