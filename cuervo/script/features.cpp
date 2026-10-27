#include "cuervo/script/features.hpp"

#include <algorithm>
#include <set>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/assets/vfs.hpp"
#include "cuervo/rhi/rhi.hpp"
#include "cuervo/scene/scene_file.hpp"
#include "cuervo/script/project.hpp"
#include "pluma/lexer.hpp"

namespace cuervo::script {

bool FeatureSet::enabled(std::string_view name) const {
    if (name == "audio") return audio;
    if (name == "ui") return ui;
    if (name == "particles") return particles;
    if (name == "collision") return collision;
    if (name == "physics") return physics;
    if (name == "vulkan") return vulkan;
    if (name == "svg_import") return svg_import;
    if (name == "font_import") return font_import;
    if (name == "dev") return dev;
    return false;
}

void FeatureSet::set(std::string_view name, bool value) {
    if (name == "audio") audio = value;
    else if (name == "ui") ui = value;
    else if (name == "particles") particles = value;
    else if (name == "collision") collision = value;
    else if (name == "physics") physics = value;
    else if (name == "vulkan") vulkan = value;
    else if (name == "svg_import") svg_import = value;
    else if (name == "font_import") font_import = value;
    else if (name == "dev") dev = value;
}

std::vector<std::string> FeatureSet::active_list() const {
    std::vector<std::string> list;
    if (audio) list.push_back("audio");
    if (collision) list.push_back("collision");
    if (dev) list.push_back("dev");
    if (font_import) list.push_back("font_import");
    if (particles) list.push_back("particles");
    if (physics) list.push_back("physics");
    if (svg_import) list.push_back("svg_import");
    if (ui) list.push_back("ui");
    if (vulkan) list.push_back("vulkan");
    std::sort(list.begin(), list.end());
    return list;
}

std::vector<std::string> FeatureSet::stripped_list() const {
    std::vector<std::string> list;
    if (!audio) list.push_back("audio");
    if (!collision) list.push_back("collision");
    if (!dev) list.push_back("dev");
    if (!font_import) list.push_back("font_import");
    if (!particles) list.push_back("particles");
    if (!physics) list.push_back("physics");
    if (!svg_import) list.push_back("svg_import");
    if (!ui) list.push_back("ui");
    if (!vulkan) list.push_back("vulkan");
    std::sort(list.begin(), list.end());
    return list;
}

bool FeatureOverrides::empty() const {
    return !audio && !ui && !particles && !collision && !physics && !vulkan && !svg_import && !font_import && !dev;
}

namespace {

bool has_audio_assets(const assets::Vfs& vfs) {
    const auto assets = vfs.list("assets", "");
    for (const auto& path : assets) {
        std::string_view p = path;
        for (std::string_view ext : {".wav", ".ogg", ".flac", ".mp3"}) {
            if (p.size() >= ext.size() && p.substr(p.size() - ext.size()) == ext) {
                return true;
            }
        }
    }
    return false;
}

bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

// Every identifier-shaped word of `text` (used inside `{...}` string
// interpolations and on scene type names such as `List<Color>`).
void add_words(std::string_view text, std::set<std::string, std::less<>>& out) {
    for (std::size_t i = 0; i < text.size();) {
        if (!is_ident_start(text[i])) {
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        while (j < text.size() && is_ident_char(text[j])) ++j;
        out.emplace(text.substr(i, j - i));
        i = j;
    }
}

// Identifiers a script names, from Pluma's own lexer: comments and string
// text never count, so a comment saying "Button" doesn't pull in the UI.
void add_script_identifiers(std::string_view source, std::set<std::string, std::less<>>& out) {
    for (const pluma::Token& t : pluma::Lexer::tokenize(source)) {
        if (t.kind == pluma::TokenKind::Identifier) {
            out.emplace(t.text);
        } else if (t.kind == pluma::TokenKind::String && t.text.find('{') != std::string_view::npos) {
            // Interpolations ("{Audio.volume()}") are lexed by the parser later.
            std::string_view s = t.text;
            for (std::size_t open = s.find('{'); open != std::string_view::npos; open = s.find('{', open + 1)) {
                const std::size_t close = s.find('}', open);
                if (close == std::string_view::npos) break;
                add_words(s.substr(open + 1, close - open - 1), out);
            }
        }
    }
}

// Top-level declarations of a prelude file (class/struct/enum/trait/fn/const
// names at brace depth 0): the names a script must use to need that feature.
std::vector<std::string> prelude_declarations(std::string_view text) {
    std::vector<std::string> names;
    int depth = 0;
    bool want_name = false;
    for (const pluma::Token& t : pluma::Lexer::tokenize(text)) {
        if (t.kind == pluma::TokenKind::LeftBrace) ++depth;
        if (t.kind == pluma::TokenKind::RightBrace) --depth;
        if (depth != 0) continue;
        if (t.kind == pluma::TokenKind::Keyword) {
            want_name = t.text == "class" || t.text == "struct" || t.text == "enum" || t.text == "trait" ||
                        t.text == "fn" || t.text == "const";
        } else if (want_name && t.kind == pluma::TokenKind::Identifier) {
            names.emplace_back(t.text);
            want_name = false;
        }
    }
    return names;
}

struct OptionalPrelude {
    const char* path;
    const char* text;
};
// All of them, whether or not this engine was built with the feature. A long
// file spans consecutive entries with the same path (cuervo_append_prelude).
constexpr OptionalPrelude optional_preludes[] = {
#include "cuervo_optional_preludes.inc"
};

// The declarations of each optional prelude, by feature.
struct FeatureNames {
    std::vector<std::string> audio, ui, particles, collision, physics;
};

FeatureNames feature_names() {
    FeatureNames f;
    for (std::size_t i = 0; i < std::size(optional_preludes);) {
        const std::string_view path = optional_preludes[i].path;
        std::string text;
        for (; i < std::size(optional_preludes) && optional_preludes[i].path == path; ++i) {
            text += optional_preludes[i].text;
        }
        std::vector<std::string> names = prelude_declarations(text);
        if (path == "cuervo/audio.plm") {
            f.audio = std::move(names);
        } else if (path == "cuervo/ui.plm" || path.starts_with("cuervo/ui/")) {
            f.ui.insert(f.ui.end(), names.begin(), names.end());
        } else if (path == "cuervo/collision.plm") {
            // Hitboxes and areas work without particles.
            f.collision = std::move(names);
        } else if (path == "cuervo/particles.plm") {
            f.particles = std::move(names);
        } else if (path.starts_with("cuervo/physics/")) {
            f.physics.insert(f.physics.end(), names.begin(), names.end());
        }
    }
    return f;
}

bool uses_any(const std::set<std::string, std::less<>>& used, const std::vector<std::string>& names) {
    return std::any_of(names.begin(), names.end(), [&](const std::string& n) { return used.contains(n); });
}

} // namespace

FeatureSet detect_features(const assets::Vfs& vfs, const Project& project, const FeatureOverrides& overrides) {
    FeatureSet result;

    // Default flags for exports:
    result.dev = false;
    result.svg_import = false;
    result.font_import = false;

    // Every name the game's scripts and scenes refer to.
    std::set<std::string, std::less<>> used;
    for (const auto& rel : vfs.list("scripts", ".plm")) {
        std::string text;
        std::string err;
        if (vfs.read_text(rel, text, err)) add_script_identifiers(text, used);
    }
    for (const auto& rel : vfs.list("scenes", ".scene")) {
        std::string err;
        auto file = vfs.open(rel, err);
        if (!file) continue;
        auto doc = scene::read_binary(file->bytes(), err);
        if (!doc) doc = scene::parse_text(file->text(), err);
        if (!doc) continue;
        for (const scene::Node& node : doc->nodes) {
            add_words(node.type, used);
            for (const scene::Field& field : node.fields) add_words(field.type, used);
        }
    }

    const FeatureNames names = feature_names();
    bool detected_audio = has_audio_assets(vfs) || uses_any(used, names.audio);
    bool detected_particles = uses_any(used, names.particles);
    bool detected_collision = uses_any(used, names.collision);
    bool detected_ui = uses_any(used, names.ui);
    bool detected_physics = uses_any(used, names.physics);

    if (detected_particles) {
        detected_collision = true;
    }

    result.audio = detected_audio;
    result.particles = detected_particles;
    result.collision = detected_collision;
    result.physics = detected_physics;
    result.ui = detected_ui;

    // Vulkan backend: required if project uses full tier
    result.vulkan = (project.render_tier == rhi::Tier::Full);

    // Apply explicit overrides
    if (overrides.audio.has_value()) result.audio = *overrides.audio;
    if (overrides.ui.has_value()) result.ui = *overrides.ui;
    if (overrides.particles.has_value()) result.particles = *overrides.particles;
    if (overrides.collision.has_value()) result.collision = *overrides.collision;
    if (overrides.physics.has_value()) result.physics = *overrides.physics;
    if (overrides.vulkan.has_value()) result.vulkan = *overrides.vulkan;
    if (overrides.svg_import.has_value()) result.svg_import = *overrides.svg_import;
    if (overrides.font_import.has_value()) result.font_import = *overrides.font_import;
    if (overrides.dev.has_value()) result.dev = *overrides.dev;

    // Enforce dependency: particles require collision world
    if (result.particles && !result.collision) {
        result.collision = true;
    }

    return result;
}

} // namespace cuervo::script
