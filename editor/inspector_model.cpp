#include "editor/inspector_model.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>

#include "editor/actions.hpp"

namespace cuervo::editor::ve {

namespace {

bool is_transform(std::string_view f) { return f == "position" || f == "rotation" || f == "scale"; }

std::vector<std::string> chain_of(const ClassRegistry& classes, std::string_view cls) {
    std::vector<std::string> chain; // most derived first
    std::string cur(cls);
    for (int depth = 0; depth < 32 && !cur.empty(); ++depth) {
        const ClassSpec* s = classes.find(cur);
        if (s == nullptr) break;
        chain.push_back(cur);
        cur = s->base;
    }
    return chain;
}

} // namespace

std::vector<Section> group_fields(const ClassRegistry& classes, std::string_view class_name, const std::vector<std::string>& stored_extra) {
    std::vector<Section> out;
    const std::vector<std::string> chain = chain_of(classes, class_name);
    std::vector<std::string> seen;
    const auto declared = [&](std::string_view f) { return std::find(seen.begin(), seen.end(), f) != seen.end(); };

    Section transform{"Transform", "Transform", {}, true};
    Section advanced{"Advanced", "Advanced", {}, false};
    std::vector<Section> own;
    // Ownership: the topmost class that declares a field owns it (specs list inherited fields too).
    std::vector<std::vector<std::string>> owned(chain.size());
    for (std::size_t i = chain.size(); i-- > 0;) {
        const ClassSpec* s = classes.find(chain[i]);
        for (const FieldSpec& f : s->fields) {
            if (declared(f.name)) continue;
            seen.push_back(f.name);
            owned[i].push_back(f.name);
        }
    }
    for (std::size_t i = 0; i < chain.size(); ++i) {
        Section s{chain[i], chain[i], {}, i == 0};
        for (const std::string& f : owned[i]) {
            if (is_transform(f)) transform.fields.push_back(f);
            else if (chain[i] == "Node" || chain[i] == "Node2D") {
                if (i == 0) s.fields.push_back(f);
                else advanced.fields.push_back(f);
            } else s.fields.push_back(f);
        }
        if (!s.fields.empty()) own.push_back(std::move(s));
    }
    // Transform keeps its natural order position, rotation, scale.
    std::sort(transform.fields.begin(), transform.fields.end(), [](const std::string& a, const std::string& b) {
        const auto rank = [](const std::string& s) { return s == "position" ? 0 : s == "rotation" ? 1 : 2; };
        return rank(a) < rank(b);
    });
    if (!transform.fields.empty()) out.push_back(std::move(transform));
    for (Section& s : own) out.push_back(std::move(s));
    if (!advanced.fields.empty()) out.push_back(std::move(advanced));
    Section extra{"Stored", "Stored in the scene, not in the class", {}, true};
    for (const std::string& f : stored_extra) if (!declared(f)) extra.fields.push_back(f);
    if (!extra.fields.empty()) out.push_back(std::move(extra));
    return out;
}

namespace {

bool listed(std::string_view list, std::string_view token) {
    std::size_t at = 0;
    while (at <= list.size()) {
        std::size_t end = list.find(';', at);
        if (end == std::string_view::npos) end = list.size();
        if (list.substr(at, end - at) == token) return true;
        at = end + 1;
    }
    return false;
}

std::string without(std::string_view list, std::string_view token) {
    std::string out;
    std::size_t at = 0;
    while (at <= list.size()) {
        std::size_t end = list.find(';', at);
        if (end == std::string_view::npos) end = list.size();
        const std::string_view t = list.substr(at, end - at);
        if (!t.empty() && t != token) out += (out.empty() ? "" : ";") + std::string(t);
        at = end + 1;
    }
    return out;
}

} // namespace

bool section_open(std::string_view list, const Section& s) {
    return s.default_open ? !listed(list, s.key) : listed(list, "+" + s.key);
}

std::string toggle_section(std::string_view list, const Section& s) {
    const std::string closed_token = s.key, open_token = "+" + s.key;
    std::string out = without(without(list, closed_token), open_token);
    const bool now_open = section_open(list, s);
    // Flip: record the override only when it differs from the default.
    if (now_open && s.default_open) out += (out.empty() ? "" : ";") + closed_token;
    else if (!now_open && !s.default_open) out += (out.empty() ? "" : ";") + open_token;
    return out;
}

std::vector<Section> filter_sections(const std::vector<Section>& sections, std::string_view query) {
    if (query.empty()) return sections;
    std::vector<Section> out;
    for (const Section& s : sections) {
        Section m = s;
        m.fields.clear();
        for (const std::string& f : s.fields) {
            FuzzyMatch fm;
            if (fuzzy_match(query, f, fm)) m.fields.push_back(f);
        }
        if (!m.fields.empty()) out.push_back(std::move(m));
    }
    return out;
}

std::string format_number(const scene::Value& v) {
    if (v.kind == scene::ValueKind::Int) return std::to_string(v.i);
    std::string s = v.num == scene::NumKind::F64 ? std::format("{}", v.f) : std::format("{}", static_cast<float>(v.f));
    if (s.find_first_of(".eEin") == std::string::npos) s += ".0";
    return s;
}

std::pair<std::int64_t, std::int64_t> int_range(scene::NumKind k) {
    using NK = scene::NumKind;
    switch (k) {
    case NK::I8: return {-128, 127};
    case NK::I16: return {-32768, 32767};
    case NK::I32: return {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()};
    case NK::U8: return {0, 255};
    case NK::U16: return {0, 65535};
    case NK::U32: return {0, std::numeric_limits<std::uint32_t>::max()};
    case NK::U64: return {0, std::numeric_limits<std::int64_t>::max()};
    default: return {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()};
    }
}

bool parse_number(std::string_view text, scene::Value& v) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    if (text.empty()) return false;
    const std::string s(text);
    char* end = nullptr;
    if (v.kind == scene::ValueKind::Int) {
        const double d = std::strtod(s.c_str(), &end); // accepts "12.0" typed into an int field
        if (end == s.c_str() || *end != '\0' || !std::isfinite(d)) return false;
        const auto [lo, hi] = int_range(v.num);
        v.i = static_cast<std::int64_t>(std::clamp(static_cast<std::int64_t>(std::llround(d)), lo, hi));
        return true;
    }
    if (v.kind == scene::ValueKind::Float) {
        const double d = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || *end != '\0' || !std::isfinite(d)) return false;
        v.f = v.num == scene::NumKind::F64 ? d : static_cast<double>(static_cast<float>(d));
        return true;
    }
    return false;
}

double scrub_speed(const scene::Value& v, bool shift, bool alt) {
    double base = v.kind == scene::ValueKind::Int ? 0.2 : 0.1; // the ImGui inspector's drag speeds
    if (shift) base *= 10.0;
    if (alt) base *= 0.01;
    return base;
}

void apply_scrub(scene::Value& v, const scene::Value& start, double delta) {
    if (v.kind == scene::ValueKind::Int) {
        const auto [lo, hi] = int_range(v.num);
        v.i = std::clamp(static_cast<std::int64_t>(std::llround(static_cast<double>(start.i) + delta)), lo, hi);
    } else if (v.kind == scene::ValueKind::Float) {
        const double d = start.f + delta;
        v.f = v.num == scene::NumKind::F64 ? d : static_cast<double>(static_cast<float>(d));
    }
}

scene::Value* at_path(scene::Value& root, std::string_view path) {
    scene::Value* cur = &root;
    while (!path.empty() && cur != nullptr) {
        const std::size_t slash = path.find('/');
        const std::string_view seg = path.substr(0, slash);
        path = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
        if (cur->kind == scene::ValueKind::Struct) {
            auto it = std::find_if(cur->fields.begin(), cur->fields.end(), [&](const scene::Field& f) { return f.name == seg; });
            cur = it == cur->fields.end() ? nullptr : &it->value;
        } else if (cur->kind == scene::ValueKind::List) {
            std::size_t i = 0;
            const auto r = std::from_chars(seg.data(), seg.data() + seg.size(), i);
            cur = r.ec == std::errc() && i < cur->items.size() ? &cur->items[i] : nullptr;
        } else {
            return nullptr;
        }
    }
    return cur;
}

std::string join_path(std::string_view base, std::string_view segment) { return base.empty() ? std::string(segment) : std::string(base) + "/" + std::string(segment); }

bool is_color(const scene::Value& v) {
    return v.kind == scene::ValueKind::Struct && v.type == "Color" && v.fields.size() == 4 &&
           std::all_of(v.fields.begin(), v.fields.end(), [](const scene::Field& f) { return f.value.kind == scene::ValueKind::Float; });
}

namespace {
int byte_of(double c) { return static_cast<int>(std::lround(std::clamp(c, 0.0, 1.0) * 255.0)); }
}

std::string color_hex(const scene::Value& v) {
    std::string s = std::format("#{:02X}{:02X}{:02X}", byte_of(v.fields[0].value.f), byte_of(v.fields[1].value.f), byte_of(v.fields[2].value.f));
    if (byte_of(v.fields[3].value.f) != 255) s += std::format("{:02X}", byte_of(v.fields[3].value.f));
    return s;
}

bool parse_color_hex(std::string_view text, scene::Value& v) {
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    if ((text.size() != 6 && text.size() != 8) || !is_color(v)) return false;
    unsigned parts[4] = {0, 0, 0, 255};
    for (std::size_t i = 0; i < text.size() / 2; ++i) {
        const auto r = std::from_chars(text.data() + i * 2, text.data() + i * 2 + 2, parts[i], 16);
        if (r.ec != std::errc() || r.ptr != text.data() + i * 2 + 2) return false;
    }
    for (std::size_t i = 0; i < 4; ++i) v.fields[i].value.f = static_cast<double>(static_cast<float>(parts[i] / 255.0));
    return true;
}

std::string color_css(const scene::Value& v) {
    return std::format("rgba({}, {}, {}, {})", byte_of(v.fields[0].value.f), byte_of(v.fields[1].value.f), byte_of(v.fields[2].value.f), byte_of(v.fields[3].value.f));
}

void hsv_to_rgb(float h, float s, float v, float& r, float& g, float& b) {
    h = h - std::floor(h);
    const float x = h * 6.0f;
    const int i = static_cast<int>(x);
    const float f = x - static_cast<float>(i);
    const float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
}

Hsva hsva_of(const scene::Value& c) {
    const float r = static_cast<float>(c.fields[0].value.f), g = static_cast<float>(c.fields[1].value.f), b = static_cast<float>(c.fields[2].value.f);
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    Hsva o;
    o.v = mx;
    o.s = mx > 0 ? d / mx : 0.0f;
    o.a = static_cast<float>(c.fields[3].value.f);
    if (d > 0) {
        float h = mx == r ? (g - b) / d : mx == g ? 2 + (b - r) / d : 4 + (r - g) / d;
        h /= 6.0f;
        o.h = h < 0 ? h + 1 : h;
    }
    return o;
}

void set_hsva(scene::Value& c, const Hsva& in) {
    float r = 0, g = 0, b = 0;
    hsv_to_rgb(std::clamp(in.h, 0.0f, 1.0f), std::clamp(in.s, 0.0f, 1.0f), std::clamp(in.v, 0.0f, 1.0f), r, g, b);
    c.fields[0].value.f = static_cast<double>(r);
    c.fields[1].value.f = static_cast<double>(g);
    c.fields[2].value.f = static_cast<double>(b);
    c.fields[3].value.f = static_cast<double>(std::clamp(in.a, 0.0f, 1.0f));
}

std::string hue_css(float h) {
    float r = 0, g = 0, b = 0;
    hsv_to_rgb(h, 1, 1, r, g, b);
    return std::format("rgb({}, {}, {})", byte_of(static_cast<double>(r)), byte_of(static_cast<double>(g)), byte_of(static_cast<double>(b)));
}

std::string drawer_tab_for(std::string_view type, bool has_script) {
    const auto has = [&](std::string_view s) { return type.find(s) != std::string_view::npos; };
    if (has("Emitter") || has("Particles")) return "Particles";
    if (has("AnimatedSprite") || has("Animation")) return "Animation";
    if (has("Sprite")) return "Sprite";
    if (has("TileMap") || has("Tile")) return "Tiles";
    return has_script ? "Script" : std::string();
}

} // namespace cuervo::editor::ve
