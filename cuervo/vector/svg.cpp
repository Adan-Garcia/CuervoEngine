#include "cuervo/vector/svg.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

#include "cuervo/core/log.hpp"
#include "cuervo/vector/xml.hpp"

// Import-time code, not simulation: <cmath> is fine (DESIGN.md 4.1).

namespace cuervo::vector {

namespace {

using xml::Node;

constexpr double pi = 3.14159265358979323846;
constexpr int max_reference_depth = 16; // use -> use -> ... and gradient href chains

// --- small parsing helpers ------------------------------------------------------

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Parses a number at the start of `s` and advances past it (and a trailing
// separator). Follows the SVG number grammar, like the path parser.
bool take_number(std::string_view& s, double& out) {
    std::size_t i = 0;
    while (i < s.size() && (is_space(s[i]) || s[i] == ',')) ++i;
    const std::size_t start = i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    bool digits = false;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) { ++i; digits = true; }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) { ++i; digits = true; }
    }
    if (!digits) return false;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        std::size_t save = i++;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        if (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        } else {
            i = save;
        }
    }
    char buffer[64];
    const std::size_t n = std::min<std::size_t>(i - start, sizeof(buffer) - 1);
    std::memcpy(buffer, s.data() + start, n);
    buffer[n] = '\0';
    out = std::strtod(buffer, nullptr);
    s.remove_prefix(i);
    return true;
}

std::vector<double> number_list(std::string_view s) {
    std::vector<double> out;
    double v = 0.0;
    while (take_number(s, v)) out.push_back(v);
    return out;
}

std::optional<double> number(std::string_view s) {
    double v = 0.0;
    s = trim(s);
    if (!take_number(s, v) || !trim(s).empty()) return std::nullopt;
    return v;
}

// Lengths: percentages resolve against `percent_base`; absolute units at 96 dpi.
std::optional<double> length(std::string_view s, double percent_base) {
    s = trim(s);
    double v = 0.0;
    if (!take_number(s, v)) return std::nullopt;
    const std::string unit = lower(trim(s));
    if (unit.empty() || unit == "px") return v;
    if (unit == "%") return v * percent_base / 100.0;
    if (unit == "pt") return v * 96.0 / 72.0;
    if (unit == "pc") return v * 16.0;
    if (unit == "mm") return v * 96.0 / 25.4;
    if (unit == "cm") return v * 96.0 / 2.54;
    if (unit == "in") return v * 96.0;
    if (unit == "em") return v * 16.0; // no text support: assume the CSS default font size
    if (unit == "ex") return v * 8.0;
    return std::nullopt;
}

// --- colors ---------------------------------------------------------------------

struct NamedColor {
    const char* name;
    std::uint32_t rgb;
};

// CSS Color Module Level 3 / SVG 1.1 named colors.
constexpr NamedColor named_colors[] = {
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF}, {"aquamarine", 0x7FFFD4},
    {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4}, {"black", 0x000000},
    {"blanchedalmond", 0xFFEBCD}, {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E},
    {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C},
    {"cyan", 0x00FFFF}, {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9}, {"darkkhaki", 0xBDB76B},
    {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC},
    {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1}, {"darkviolet", 0x9400D3},
    {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22},
    {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF}, {"gold", 0xFFD700},
    {"goldenrod", 0xDAA520}, {"gray", 0x808080}, {"grey", 0x808080}, {"green", 0x008000},
    {"greenyellow", 0xADFF2F}, {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C},
    {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA},
    {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6},
    {"lightcoral", 0xF08080}, {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3},
    {"lightgreen", 0x90EE90}, {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A},
    {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00}, {"limegreen", 0x32CD32},
    {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66CDAA},
    {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371},
    {"mediumslateblue", 0x7B68EE}, {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC},
    {"mediumvioletred", 0xC71585}, {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1},
    {"moccasin", 0xFFE4B5}, {"navajowhite", 0xFFDEAD}, {"navy", 0x000080}, {"oldlace", 0xFDF5E6},
    {"olive", 0x808000}, {"olivedrab", 0x6B8E23}, {"orange", 0xFFA500}, {"orangered", 0xFF4500},
    {"orchid", 0xDA70D6}, {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE},
    {"palevioletred", 0xDB7093}, {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F},
    {"pink", 0xFFC0CB}, {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6}, {"purple", 0x800080},
    {"red", 0xFF0000}, {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1}, {"saddlebrown", 0x8B4513},
    {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57}, {"seashell", 0xFFF5EE},
    {"sienna", 0xA0522D}, {"silver", 0xC0C0C0}, {"skyblue", 0x87CEEB}, {"slateblue", 0x6A5ACD},
    {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xFFFAFA}, {"springgreen", 0x00FF7F},
    {"steelblue", 0x4682B4}, {"tan", 0xD2B48C}, {"teal", 0x008080}, {"thistle", 0xD8BFD8},
    {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE}, {"wheat", 0xF5DEB3},
    {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00}, {"yellowgreen", 0x9ACD32},
    {"rebeccapurple", 0x663399},
};

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Color from_rgb(std::uint32_t rgb) {
    return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f};
}

} // namespace

bool parse_color(std::string_view text, Color& out) {
    std::string s = lower(trim(text));
    if (s.empty()) return false;
    if (s[0] == '#') {
        std::array<int, 8> d{};
        const std::size_t n = s.size() - 1;
        if (n != 3 && n != 4 && n != 6 && n != 8) return false;
        for (std::size_t i = 0; i < n; ++i) {
            d[i] = hex_digit(s[i + 1]);
            if (d[i] < 0) return false;
        }
        if (n == 3 || n == 4) {
            out = {static_cast<float>(d[0] * 17) / 255.0f, static_cast<float>(d[1] * 17) / 255.0f,
                   static_cast<float>(d[2] * 17) / 255.0f, n == 4 ? static_cast<float>(d[3] * 17) / 255.0f : 1.0f};
        } else {
            out = {static_cast<float>(d[0] * 16 + d[1]) / 255.0f, static_cast<float>(d[2] * 16 + d[3]) / 255.0f,
                   static_cast<float>(d[4] * 16 + d[5]) / 255.0f,
                   n == 8 ? static_cast<float>(d[6] * 16 + d[7]) / 255.0f : 1.0f};
        }
        return true;
    }
    if (s.starts_with("rgb(") || s.starts_with("rgba(")) {
        const std::size_t open = s.find('(');
        const std::size_t close = s.find(')');
        if (close == std::string::npos) return false;
        std::string_view args(s.data() + open + 1, close - open - 1);
        float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (int i = 0; i < 4; ++i) {
            while (!args.empty() && (is_space(args.front()) || args.front() == ',' || args.front() == '/')) {
                args.remove_prefix(1);
            }
            if (args.empty()) {
                if (i < 3) return false;
                break;
            }
            double v = 0.0;
            if (!take_number(args, v)) return false;
            const bool percent = !args.empty() && args.front() == '%';
            if (percent) args.remove_prefix(1);
            if (i < 3) {
                c[i] = static_cast<float>(std::clamp(percent ? v / 100.0 : v / 255.0, 0.0, 1.0));
            } else {
                c[i] = static_cast<float>(std::clamp(percent ? v / 100.0 : v, 0.0, 1.0));
            }
        }
        out = {c[0], c[1], c[2], c[3]};
        return true;
    }
    if (s == "transparent") {
        out = {0.0f, 0.0f, 0.0f, 0.0f};
        return true;
    }
    for (const NamedColor& named : named_colors) {
        if (s == named.name) {
            out = from_rgb(named.rgb);
            return true;
        }
    }
    return false;
}

bool parse_transform(std::string_view text, Affine2& out) {
    out = {};
    std::string_view s = text;
    Affine2 result;
    while (true) {
        while (!s.empty() && (is_space(s.front()) || s.front() == ',')) s.remove_prefix(1);
        if (s.empty()) break;
        std::size_t name_end = 0;
        while (name_end < s.size() && std::isalpha(static_cast<unsigned char>(s[name_end]))) ++name_end;
        const std::string_view name = s.substr(0, name_end);
        s.remove_prefix(name_end);
        while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
        if (s.empty() || s.front() != '(') return false;
        const std::size_t close = s.find(')');
        if (close == std::string_view::npos) return false;
        const std::vector<double> a = number_list(s.substr(1, close - 1));
        s.remove_prefix(close + 1);

        Affine2 t;
        const std::size_t n = a.size();
        if (name == "matrix") {
            if (n != 6) return false;
            t = {static_cast<float>(a[0]), static_cast<float>(a[1]), static_cast<float>(a[2]),
                 static_cast<float>(a[3]), static_cast<float>(a[4]), static_cast<float>(a[5])};
        } else if (name == "translate") {
            if (n != 1 && n != 2) return false;
            t = Affine2::translate({static_cast<float>(a[0]), n == 2 ? static_cast<float>(a[1]) : 0.0f});
        } else if (name == "scale") {
            if (n != 1 && n != 2) return false;
            t = Affine2::scale({static_cast<float>(a[0]), static_cast<float>(n == 2 ? a[1] : a[0])});
        } else if (name == "rotate") {
            if (n != 1 && n != 3) return false;
            // Degrees, positive = clockwise on a y-down screen (same as Affine2::rotate).
            const double r = a[0] * pi / 180.0;
            const auto co = static_cast<float>(std::cos(r));
            const auto si = static_cast<float>(std::sin(r));
            t = {co, si, -si, co, 0.0f, 0.0f};
            if (n == 3) {
                const vec2 c{static_cast<float>(a[1]), static_cast<float>(a[2])};
                t = Affine2::translate(c) * t * Affine2::translate(-c);
            }
        } else if (name == "skewX") {
            if (n != 1) return false;
            t = {1.0f, 0.0f, static_cast<float>(std::tan(a[0] * pi / 180.0)), 1.0f, 0.0f, 0.0f};
        } else if (name == "skewY") {
            if (n != 1) return false;
            t = {1.0f, static_cast<float>(std::tan(a[0] * pi / 180.0)), 0.0f, 1.0f, 0.0f, 0.0f};
        } else {
            return false;
        }
        result = result * t; // "A B" applies B first, then A
    }
    out = result;
    return true;
}

namespace {

// --- styles ---------------------------------------------------------------------

struct PaintSpec {
    enum class Kind { None, Color, CurrentColor, Url };
    Kind kind = Kind::None;
    Color color;
    std::string url; // id without '#'
    // Fallback after url(): none / a color / currentColor; kind None if absent.
    Kind fallback = Kind::None;
    Color fallback_color;
};

// Inherited properties.
struct Style {
    PaintSpec fill{PaintSpec::Kind::Color, {0.0f, 0.0f, 0.0f, 1.0f}, {}, PaintSpec::Kind::None, {}};
    float fill_opacity = 1.0f;
    FillRule fill_rule = FillRule::NonZero;
    PaintSpec stroke;
    float stroke_width = 1.0f;
    float stroke_opacity = 1.0f;
    LineJoin join = LineJoin::Miter;
    LineCap cap = LineCap::Butt;
    float miter_limit = 4.0f;
    std::vector<float> dashes;
    float dash_offset = 0.0f;
    Color color{0.0f, 0.0f, 0.0f, 1.0f};
    FillRule clip_rule = FillRule::NonZero;
    bool visible = true;
    bool stroke_first = false; // paint-order
};

// Non-inherited properties of one element.
struct Local {
    float opacity = 1.0f;
    bool display_none = false;
    std::string clip_path; // id
    Color stop_color{0.0f, 0.0f, 0.0f, 1.0f};
    float stop_opacity = 1.0f;
};

std::optional<std::string> url_id(std::string_view value) {
    value = trim(value);
    if (!value.starts_with("url(")) return std::nullopt;
    const std::size_t close = value.find(')');
    if (close == std::string_view::npos) return std::nullopt;
    std::string_view inner = trim(value.substr(4, close - 4));
    if (!inner.empty() && (inner.front() == '"' || inner.front() == '\'')) {
        inner.remove_prefix(1);
        if (!inner.empty()) inner.remove_suffix(1);
    }
    if (!inner.starts_with("#")) return std::nullopt; // external references aren't supported
    return std::string(inner.substr(1));
}

std::optional<float> opacity_value(std::string_view v) {
    v = trim(v);
    double x = 0.0;
    if (!take_number(v, x)) return std::nullopt;
    if (trim(v) == "%") x /= 100.0;
    return static_cast<float>(std::clamp(x, 0.0, 1.0));
}

class SvgImporter {
public:
    SvgResult run(std::string_view text, std::string_view name) {
        SvgResult result;
        xml::Document doc;
        if (!xml::parse(text, doc, &result.error)) {
            return result;
        }
        if (doc.root->name != "svg") {
            result.error = std::format("root element is <{}>, not <svg>", doc.root->name);
            return result;
        }
        index_ids(*doc.root);

        const Node& root = *doc.root;
        const std::vector<double> view_box = viewbox(root);
        // Document size: width/height when absolute, else the viewBox size, else
        // the CSS default replaced-element size.
        double width = 0.0, height = 0.0;
        const std::string* w_attr = root.attribute("width");
        const std::string* h_attr = root.attribute("height");
        const bool w_percent = w_attr && w_attr->find('%') != std::string::npos;
        const bool h_percent = h_attr && h_attr->find('%') != std::string::npos;
        if (w_attr && !w_percent) width = length(*w_attr, 0.0).value_or(0.0);
        if (h_attr && !h_percent) height = length(*h_attr, 0.0).value_or(0.0);
        if (view_box.size() == 4) {
            if (width <= 0.0 && height <= 0.0) {
                width = view_box[2];
                height = view_box[3];
            } else if (width <= 0.0) {
                width = view_box[3] > 0.0 ? height * view_box[2] / view_box[3] : height;
            } else if (height <= 0.0) {
                height = view_box[2] > 0.0 ? width * view_box[3] / view_box[2] : width;
            }
        }
        if (width <= 0.0) width = 300.0;
        if (height <= 0.0) height = 150.0;
        document_.size = {static_cast<float>(width), static_cast<float>(height)};

        Context ctx;
        ctx.viewport = {width, height};
        if (view_box.size() == 4 && view_box[2] > 0.0 && view_box[3] > 0.0) {
            ctx.ctm = viewbox_transform(root, view_box, 0.0, 0.0, width, height);
            ctx.viewport = {view_box[2], view_box[3]};
        }
        walk_children_with_own_style(root, ctx);

        result.ok = true;
        result.document = std::move(document_);
        result.unsupported.assign(unsupported_.begin(), unsupported_.end());
        if (!result.unsupported.empty()) {
            std::string list;
            for (const std::string& u : result.unsupported) {
                list += list.empty() ? u : ", " + u;
            }
            LOG_WARN("SVG '{}': skipped unsupported features: {}", name, list);
        }
        return result;
    }

private:
    struct Viewport {
        double w = 0.0, h = 0.0;
        double diagonal() const { return std::sqrt((w * w + h * h) / 2.0); }
    };

    struct Context {
        Affine2 ctm;
        Style style;
        float opacity = 1.0f;
        std::vector<std::uint32_t> clips;
        Viewport viewport;
        int reference_depth = 0;
    };

    void index_ids(const Node& node) {
        if (const std::string* id = node.attribute("id")) {
            ids_.emplace(*id, &node); // first definition wins, like browsers
        }
        for (const auto& child : node.children) {
            index_ids(*child);
        }
    }

    const Node* find(const std::string& id) const {
        auto it = ids_.find(id);
        return it == ids_.end() ? nullptr : it->second;
    }

    void unsupported(std::string what) { unsupported_.insert(std::move(what)); }

    static const std::string* href(const Node& node) {
        if (const std::string* h = node.attribute("href")) return h;
        return node.attribute("xlink:href");
    }

    static std::vector<double> viewbox(const Node& node) {
        const std::string* vb = node.attribute("viewBox");
        return vb ? number_list(*vb) : std::vector<double>{};
    }

    // viewBox -> viewport mapping with preserveAspectRatio (SVG 1.1 7.8).
    static Affine2 viewbox_transform(const Node& node, const std::vector<double>& vb, double x, double y, double w,
                                     double h) {
        double sx = w / vb[2];
        double sy = h / vb[3];
        double align_x = 0.5, align_y = 0.5;
        bool none = false;
        if (const std::string* par = node.attribute("preserveAspectRatio")) {
            std::string_view v = trim(*par);
            if (v.starts_with("defer")) v = trim(v.substr(5));
            const bool slice = v.find("slice") != std::string_view::npos;
            if (v.starts_with("none")) {
                none = true;
            } else if (v.size() >= 8) {
                const std::string_view ax = v.substr(1, 3);
                const std::string_view ay = v.substr(5, 3);
                align_x = ax == "Min" ? 0.0 : ax == "Max" ? 1.0 : 0.5;
                align_y = ay == "Min" ? 0.0 : ay == "Max" ? 1.0 : 0.5;
            }
            if (!none) {
                const double s = slice ? std::max(sx, sy) : std::min(sx, sy);
                sx = sy = s;
            }
        } else {
            sx = sy = std::min(sx, sy); // xMidYMid meet
        }
        const double tx = x - vb[0] * sx + (w - vb[2] * sx) * align_x;
        const double ty = y - vb[1] * sy + (h - vb[3] * sy) * align_y;
        return {static_cast<float>(sx), 0.0f, 0.0f, static_cast<float>(sy), static_cast<float>(tx),
                static_cast<float>(ty)};
    }

    // Presentation attributes, then the `style` attribute (which wins).
    static std::vector<std::pair<std::string, std::string>> declarations(const Node& node) {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& [k, v] : node.attributes) {
            if (k != "style") out.emplace_back(k, v);
        }
        if (const std::string* style = node.attribute("style")) {
            std::string_view s = *style;
            while (!s.empty()) {
                const std::size_t semi = std::min(s.find(';'), s.size());
                const std::string_view decl = s.substr(0, semi);
                s.remove_prefix(std::min(semi + 1, s.size()));
                const std::size_t colon = decl.find(':');
                if (colon == std::string_view::npos) continue;
                std::string key = lower(trim(decl.substr(0, colon)));
                std::string_view value = trim(decl.substr(colon + 1));
                if (const std::size_t bang = value.find("!important"); bang != std::string_view::npos) {
                    value = trim(value.substr(0, bang));
                }
                out.emplace_back(std::move(key), std::string(value));
            }
        }
        return out;
    }

    bool parse_paint(std::string_view value, const Style& parent, PaintSpec& out) {
        value = trim(value);
        if (value == "none") {
            out = {};
            return true;
        }
        if (value == "currentColor") {
            out = {PaintSpec::Kind::CurrentColor, {}, {}, PaintSpec::Kind::None, {}};
            return true;
        }
        if (value.starts_with("url(")) {
            const auto id = url_id(value);
            if (!id) {
                unsupported("external or malformed paint url()");
                return false;
            }
            PaintSpec p{PaintSpec::Kind::Url, {}, *id, PaintSpec::Kind::None, {}};
            const std::string_view rest = trim(value.substr(value.find(')') + 1));
            if (rest == "currentColor") {
                p.fallback = PaintSpec::Kind::CurrentColor;
            } else if (!rest.empty() && rest != "none" && parse_color(rest, p.fallback_color)) {
                p.fallback = PaintSpec::Kind::Color;
            }
            out = p;
            return true;
        }
        Color c;
        if (parse_color(value, c)) {
            out = {PaintSpec::Kind::Color, c, {}, PaintSpec::Kind::None, {}};
            return true;
        }
        (void)parent;
        return false; // invalid: the property is ignored (stays inherited)
    }

    // Applies one declaration. Unknown names are ignored silently (they are
    // mostly editor metadata such as inkscape:label or sodipodi:nodetypes).
    void apply(const std::string& key, const std::string& raw, const Style& parent, const Viewport& vp, Style& style,
               Local& local) {
        const std::string_view value = trim(raw);
        if (value == "inherit") {
            // Inherited properties already hold the parent's value.
            if (key == "opacity") local.opacity = 1.0f;
            return;
        }
        if (key == "fill") {
            parse_paint(value, parent, style.fill);
        } else if (key == "stroke") {
            parse_paint(value, parent, style.stroke);
        } else if (key == "fill-opacity") {
            if (auto o = opacity_value(value)) style.fill_opacity = *o;
        } else if (key == "stroke-opacity") {
            if (auto o = opacity_value(value)) style.stroke_opacity = *o;
        } else if (key == "opacity") {
            if (auto o = opacity_value(value)) local.opacity = *o;
        } else if (key == "fill-rule") {
            style.fill_rule = value == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero;
        } else if (key == "clip-rule") {
            style.clip_rule = value == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero;
        } else if (key == "stroke-width") {
            if (auto w = length(value, vp.diagonal()); w && *w >= 0.0) style.stroke_width = static_cast<float>(*w);
        } else if (key == "stroke-linejoin") {
            if (value == "round") style.join = LineJoin::Round;
            else if (value == "bevel") style.join = LineJoin::Bevel;
            else if (value == "miter" || value == "miter-clip") style.join = LineJoin::Miter;
            else if (value == "arcs") { style.join = LineJoin::Miter; unsupported("stroke-linejoin arcs"); }
        } else if (key == "stroke-linecap") {
            if (value == "round") style.cap = LineCap::Round;
            else if (value == "square") style.cap = LineCap::Square;
            else if (value == "butt") style.cap = LineCap::Butt;
        } else if (key == "stroke-miterlimit") {
            if (auto m = number(value); m && *m >= 1.0) style.miter_limit = static_cast<float>(*m);
        } else if (key == "stroke-dasharray") {
            style.dashes.clear();
            if (value != "none") {
                std::string_view rest = value;
                while (true) {
                    while (!rest.empty() && (is_space(rest.front()) || rest.front() == ',')) rest.remove_prefix(1);
                    if (rest.empty()) break;
                    std::size_t end = 0;
                    while (end < rest.size() && !is_space(rest[end]) && rest[end] != ',') ++end;
                    auto d = length(rest.substr(0, end), vp.diagonal());
                    if (!d || *d < 0.0) {
                        style.dashes.clear(); // invalid list: solid (SVG 1.1 11.4)
                        break;
                    }
                    style.dashes.push_back(static_cast<float>(*d));
                    rest.remove_prefix(end);
                }
            }
        } else if (key == "stroke-dashoffset") {
            if (auto o = length(value, vp.diagonal())) style.dash_offset = static_cast<float>(*o);
        } else if (key == "color") {
            Color c;
            if (parse_color(value, c)) style.color = c;
        } else if (key == "display") {
            local.display_none = value == "none";
        } else if (key == "visibility") {
            style.visible = !(value == "hidden" || value == "collapse");
        } else if (key == "clip-path") {
            if (value != "none") {
                if (auto id = url_id(value)) local.clip_path = *id;
                else unsupported("CSS clip-path shapes");
            }
        } else if (key == "stop-color") {
            Color c;
            if (value == "currentColor") local.stop_color = style.color;
            else if (parse_color(value, c)) local.stop_color = c;
        } else if (key == "stop-opacity") {
            if (auto o = opacity_value(value)) local.stop_opacity = *o;
        } else if (key == "filter") {
            if (value != "none") unsupported("filters");
        } else if (key == "mask") {
            if (value != "none") unsupported("masks");
        } else if (key == "marker" || key == "marker-start" || key == "marker-mid" || key == "marker-end") {
            if (value != "none") unsupported("markers");
        } else if (key == "vector-effect") {
            if (value != "none") unsupported("vector-effect");
        } else if (key == "paint-order") {
            // Listed keywords first, the rest in the default order (fill,
            // stroke, markers). Markers aren't drawn, so only fill vs stroke matters.
            std::vector<std::string> order;
            std::string_view rest = value;
            while (!rest.empty()) {
                while (!rest.empty() && is_space(rest.front())) rest.remove_prefix(1);
                std::size_t end = 0;
                while (end < rest.size() && !is_space(rest[end])) ++end;
                if (end > 0) order.emplace_back(rest.substr(0, end));
                rest.remove_prefix(end);
            }
            if (order.empty() || order[0] == "normal") {
                style.stroke_first = false;
            } else {
                for (const char* k : {"fill", "stroke", "markers"}) {
                    if (std::find(order.begin(), order.end(), k) == order.end()) order.emplace_back(k);
                }
                style.stroke_first = std::find(order.begin(), order.end(), "stroke") <
                                     std::find(order.begin(), order.end(), "fill");
            }
        } else if (key == "mix-blend-mode") {
            if (value != "normal") unsupported("mix-blend-mode");
        }
    }

    void compute_style(const Node& node, const Context& parent, Style& style, Local& local) {
        style = parent.style;
        for (const auto& [k, v] : declarations(node)) {
            apply(k, v, parent.style, parent.viewport, style, local);
        }
    }

    // Children of the root <svg> with its own presentation attributes applied.
    void walk_children_with_own_style(const Node& svg, Context ctx) {
        Local local;
        Style style;
        compute_style(svg, ctx, style, local);
        if (local.display_none) return;
        ctx.style = style;
        ctx.opacity *= local.opacity;
        if (!local.clip_path.empty()) push_clip(svg, local.clip_path, ctx);
        for (const auto& child : svg.children) {
            walk(*child, ctx);
        }
    }

    void walk(const Node& node, const Context& parent) {
        const std::string& name = node.name;
        // Elements that never render directly.
        if (name == "defs" || name == "clipPath" || name == "linearGradient" || name == "radialGradient" ||
            name == "symbol" || name == "title" || name == "desc" || name == "metadata" || name == "stop" ||
            name == "namedview") {
            return;
        }
        if (name == "style") {
            unsupported("CSS stylesheets (<style>)");
            return;
        }
        if (name == "text" || name == "tspan" || name == "textPath") {
            unsupported("<text>");
            return;
        }
        if (name == "image") { unsupported("<image>"); return; }
        if (name == "foreignObject") { unsupported("<foreignObject>"); return; }
        if (name == "filter") { return; } // reported where referenced
        if (name == "mask") { return; }
        if (name == "pattern") { return; }
        if (name == "marker") { return; }
        if (name == "script") { unsupported("<script>"); return; }
        if (name.starts_with("animate") || name == "set") { unsupported("SMIL animation"); return; }
        if (name.find(':') != std::string::npos) return; // editor namespaces (sodipodi:namedview, ...)

        Context ctx = parent;
        Local local;
        compute_style(node, parent, ctx.style, local);
        if (local.display_none) return;
        ctx.opacity *= local.opacity;
        if (const std::string* t = node.attribute("transform")) {
            Affine2 m;
            if (parse_transform(*t, m)) ctx.ctm = ctx.ctm * m;
        }

        if (name == "g" || name == "a" || name == "switch") {
            if (!local.clip_path.empty()) push_clip(node, local.clip_path, ctx);
            for (const auto& child : node.children) {
                walk(*child, ctx);
                if (name == "switch" && !child->name.empty()) break; // render the first child only
            }
            return;
        }
        if (name == "svg") {
            nested_svg(node, ctx, local);
            return;
        }
        if (name == "use") {
            use(node, ctx, local);
            return;
        }

        Path path;
        if (!shape_path(node, ctx.viewport, path)) {
            if (name != "path" && name != "rect" && name != "circle" && name != "ellipse" && name != "line" &&
                name != "polyline" && name != "polygon") {
                unsupported(std::format("<{}>", name));
            }
            return;
        }
        if (!local.clip_path.empty()) push_clip(node, local.clip_path, ctx);
        add_shape(node, std::move(path), ctx);
    }

    void nested_svg(const Node& node, Context ctx, const Local& local) {
        const double x = length_attr(node, "x", ctx.viewport.w, 0.0);
        const double y = length_attr(node, "y", ctx.viewport.h, 0.0);
        const double w = length_attr(node, "width", ctx.viewport.w, ctx.viewport.w);
        const double h = length_attr(node, "height", ctx.viewport.h, ctx.viewport.h);
        if (w <= 0.0 || h <= 0.0) return;
        const std::vector<double> vb = viewbox(node);
        if (vb.size() == 4 && vb[2] > 0.0 && vb[3] > 0.0) {
            ctx.ctm = ctx.ctm * viewbox_transform(node, vb, x, y, w, h);
            ctx.viewport = {vb[2], vb[3]};
        } else {
            ctx.ctm = ctx.ctm * Affine2::translate({static_cast<float>(x), static_cast<float>(y)});
            ctx.viewport = {w, h};
        }
        if (!local.clip_path.empty()) push_clip(node, local.clip_path, ctx);
        for (const auto& child : node.children) {
            walk(*child, ctx);
        }
    }

    void use(const Node& node, Context ctx, const Local& local) {
        const std::string* ref = href(node);
        if (!ref || !ref->starts_with("#")) return;
        const Node* target = find(ref->substr(1));
        if (!target || ctx.reference_depth >= max_reference_depth) {
            if (target) unsupported("<use> nested too deeply (or recursive)");
            return;
        }
        // Reference cycle: target is an ancestor of this <use>.
        for (const Node* p = &node; p; p = p->parent) {
            if (p == target) {
                unsupported("recursive <use>");
                return;
            }
        }
        const double x = length_attr(node, "x", ctx.viewport.w, 0.0);
        const double y = length_attr(node, "y", ctx.viewport.h, 0.0);
        ctx.ctm = ctx.ctm * Affine2::translate({static_cast<float>(x), static_cast<float>(y)});
        ++ctx.reference_depth;
        if (!local.clip_path.empty()) push_clip(node, local.clip_path, ctx);
        if (target->name == "symbol") {
            Local symbol_local;
            Style symbol_style;
            compute_style(*target, ctx, symbol_style, symbol_local);
            ctx.style = symbol_style;
            ctx.opacity *= symbol_local.opacity;
            const std::vector<double> vb = viewbox(*target);
            const double w = length_attr(node, "width", ctx.viewport.w, ctx.viewport.w);
            const double h = length_attr(node, "height", ctx.viewport.h, ctx.viewport.h);
            if (vb.size() == 4 && vb[2] > 0.0 && vb[3] > 0.0 && w > 0.0 && h > 0.0) {
                ctx.ctm = ctx.ctm * viewbox_transform(*target, vb, 0.0, 0.0, w, h);
                ctx.viewport = {vb[2], vb[3]};
            }
            for (const auto& child : target->children) {
                walk(*child, ctx);
            }
        } else {
            walk(*target, ctx);
        }
    }

    double length_attr(const Node& node, const char* key, double percent_base, double fallback) const {
        const std::string* v = node.attribute(key);
        if (!v) {
            // Geometry properties may also come from `style` (SVG 2); rare, but cheap.
            return fallback;
        }
        return length(*v, percent_base).value_or(fallback);
    }

    // Builds the geometry of a basic shape or path. False if `node` isn't one
    // (or has no geometry).
    bool shape_path(const Node& node, const Viewport& vp, Path& out) {
        const std::string& name = node.name;
        if (name == "path") {
            const std::string* d = node.attribute("d");
            if (!d) return false;
            std::string error;
            if (!parse_path_data(*d, out, &error)) {
                LOG_WARN("SVG: {}{}: {} (rendering the part before the error)", "<path>",
                         node.attribute("id") ? " #" + *node.attribute("id") : std::string(), error);
            }
            return !out.empty();
        }
        if (name == "rect") {
            const double x = length_attr(node, "x", vp.w, 0.0), y = length_attr(node, "y", vp.h, 0.0);
            const double w = length_attr(node, "width", vp.w, 0.0), h = length_attr(node, "height", vp.h, 0.0);
            if (w <= 0.0 || h <= 0.0) return false;
            double rx = length_attr(node, "rx", vp.w, -1.0), ry = length_attr(node, "ry", vp.h, -1.0);
            if (rx < 0.0 && ry < 0.0) rx = ry = 0.0;
            else if (rx < 0.0) rx = ry;
            else if (ry < 0.0) ry = rx;
            out.add_rect({static_cast<float>(x), static_cast<float>(y)}, {static_cast<float>(w), static_cast<float>(h)},
                         {static_cast<float>(rx), static_cast<float>(ry)});
            return true;
        }
        if (name == "circle") {
            const double r = length_attr(node, "r", vp.diagonal(), 0.0);
            if (r <= 0.0) return false;
            out.add_ellipse({static_cast<float>(length_attr(node, "cx", vp.w, 0.0)),
                             static_cast<float>(length_attr(node, "cy", vp.h, 0.0))},
                            vec2(static_cast<float>(r)));
            return true;
        }
        if (name == "ellipse") {
            double rx = length_attr(node, "rx", vp.w, -1.0), ry = length_attr(node, "ry", vp.h, -1.0);
            if (rx < 0.0) rx = ry; // SVG 2 "auto"
            if (ry < 0.0) ry = rx;
            if (rx <= 0.0 || ry <= 0.0) return false;
            out.add_ellipse({static_cast<float>(length_attr(node, "cx", vp.w, 0.0)),
                             static_cast<float>(length_attr(node, "cy", vp.h, 0.0))},
                            {static_cast<float>(rx), static_cast<float>(ry)});
            return true;
        }
        if (name == "line") {
            out.move_to({static_cast<float>(length_attr(node, "x1", vp.w, 0.0)),
                         static_cast<float>(length_attr(node, "y1", vp.h, 0.0))});
            out.line_to({static_cast<float>(length_attr(node, "x2", vp.w, 0.0)),
                         static_cast<float>(length_attr(node, "y2", vp.h, 0.0))});
            return true;
        }
        if (name == "polyline" || name == "polygon") {
            const std::string* points = node.attribute("points");
            if (!points) return false;
            const std::vector<double> v = number_list(*points);
            if (v.size() < 2) return false;
            out.move_to({static_cast<float>(v[0]), static_cast<float>(v[1])});
            for (std::size_t i = 2; i + 1 < v.size(); i += 2) {
                out.line_to({static_cast<float>(v[i]), static_cast<float>(v[i + 1])});
            }
            if (name == "polygon") out.close();
            return true;
        }
        return false;
    }

    // Bounding box of an element's geometry in its own user space (for
    // objectBoundingBox units on clip paths).
    Bounds element_bounds(const Node& node, const Viewport& vp, int depth = 0) {
        Bounds b;
        Path path;
        if (shape_path(node, vp, path)) {
            return path.bounds();
        }
        if (depth > max_reference_depth) return b;
        for (const auto& child : node.children) {
            Bounds cb = element_bounds(*child, vp, depth + 1);
            if (cb.empty) continue;
            Affine2 m;
            if (const std::string* t = child->attribute("transform")) parse_transform(*t, m);
            for (vec2 corner : {cb.min, cb.max, vec2(cb.min.x(), cb.max.y()), vec2(cb.max.x(), cb.min.y())}) {
                b.add(m.apply(corner));
            }
        }
        return b;
    }

    // Resolves clip-path="url(#id)" on `node` (whose CTM is ctx.ctm) into a
    // new ClipPath entry and adds it to ctx.clips.
    void push_clip(const Node& node, const std::string& id, Context& ctx) {
        const Node* clip = find(id);
        if (!clip || clip->name != "clipPath") {
            // Per spec a reference to a missing element is an error; browsers
            // ignore it, so do we.
            if (clip && clip->name == "mask") unsupported("masks");
            return;
        }
        Affine2 base = ctx.ctm;
        if (const std::string* units = clip->attribute("clipPathUnits"); units && trim(*units) == "objectBoundingBox") {
            const Bounds bb = element_bounds(node, ctx.viewport);
            if (bb.empty) {
                ctx.clips.push_back(add_empty_clip());
                return;
            }
            base = base * Affine2::translate(bb.min) * Affine2::scale(bb.size());
        }
        if (const std::string* t = clip->attribute("transform")) {
            Affine2 m;
            if (parse_transform(*t, m)) base = base * m;
        }
        if (clip->attribute("clip-path")) unsupported("clip-path on <clipPath>");

        Context clip_ctx;
        clip_ctx.viewport = ctx.viewport;
        Local clip_local;
        compute_style(*clip, clip_ctx, clip_ctx.style, clip_local);

        ClipPath region;
        for (const auto& child_ptr : clip->children) {
            const Node* child = child_ptr.get();
            Affine2 child_transform = base;
            if (child->name == "use") {
                if (const std::string* t = child->attribute("transform")) {
                    Affine2 m;
                    if (parse_transform(*t, m)) child_transform = child_transform * m;
                }
                const double x = length_attr(*child, "x", ctx.viewport.w, 0.0);
                const double y = length_attr(*child, "y", ctx.viewport.h, 0.0);
                child_transform = child_transform * Affine2::translate({static_cast<float>(x), static_cast<float>(y)});
                const std::string* ref = href(*child);
                child = ref && ref->starts_with("#") ? find(ref->substr(1)) : nullptr;
                if (!child) continue;
            }
            if (child->name == "text") {
                unsupported("<text> in <clipPath>");
                continue;
            }
            Style style;
            Local local;
            compute_style(*child, clip_ctx, style, local);
            if (local.display_none || !style.visible) continue;
            if (!local.clip_path.empty()) unsupported("clip-path on <clipPath> children");
            if (const std::string* t = child->attribute("transform")) {
                Affine2 m;
                if (parse_transform(*t, m)) child_transform = child_transform * m;
            }
            ClipShape shape;
            if (!shape_path(*child, ctx.viewport, shape.path)) continue;
            shape.transform = child_transform;
            shape.rule = style.clip_rule;
            region.shapes.push_back(std::move(shape));
        }
        ctx.clips.push_back(static_cast<std::uint32_t>(document_.clip_paths.size()));
        document_.clip_paths.push_back(std::move(region));
    }

    std::uint32_t add_empty_clip() {
        document_.clip_paths.emplace_back();
        return static_cast<std::uint32_t>(document_.clip_paths.size() - 1);
    }

    // --- gradients ------------------------------------------------------------------

    // Looks up an attribute along the href chain (SVG 1.1 13.2.2 inheritance).
    const std::string* gradient_attr(const Node& node, const char* key) const {
        const Node* n = &node;
        for (int depth = 0; n && depth < max_reference_depth; ++depth) {
            if (const std::string* v = n->attribute(key)) return v;
            const std::string* ref = href(*n);
            if (!ref || !ref->starts_with("#")) break;
            n = find(ref->substr(1));
            if (n && n->name != "linearGradient" && n->name != "radialGradient") break;
        }
        return nullptr;
    }

    const Node* gradient_stops_owner(const Node& node) const {
        const Node* n = &node;
        for (int depth = 0; n && depth < max_reference_depth; ++depth) {
            for (const auto& child : n->children) {
                if (child->name == "stop") return n;
            }
            const std::string* ref = href(*n);
            if (!ref || !ref->starts_with("#")) break;
            n = find(ref->substr(1));
            if (n && n->name != "linearGradient" && n->name != "radialGradient") break;
        }
        return nullptr;
    }

    // Gradient coordinate: a fraction in bbox units, a length in user units.
    double gradient_coord(const Node& node, const char* key, bool bbox, double percent_base, double fallback) const {
        const std::string* v = gradient_attr(node, key);
        if (!v) return fallback;
        if (bbox) {
            std::string_view s = trim(*v);
            double x = 0.0;
            if (!take_number(s, x)) return fallback;
            return trim(s) == "%" ? x / 100.0 : x;
        }
        return length(*v, percent_base).value_or(fallback);
    }

    // Resolves a gradient element into the document; returns its index, or
    // nothing when it has no stops (paint = none). Single-stop gradients are
    // reported through `solid`.
    std::optional<std::uint32_t> gradient(const Node& node, const Viewport& vp, std::optional<Color>& solid) {
        solid.reset();
        if (auto it = gradient_cache_.find(&node); it != gradient_cache_.end()) {
            solid = it->second.solid;
            return it->second.index;
        }
        Gradient g;
        g.kind = node.name == "radialGradient" ? GradientKind::Radial : GradientKind::Linear;
        if (const std::string* units = gradient_attr(node, "gradientUnits")) {
            g.bbox_units = trim(*units) != "userSpaceOnUse";
        }
        if (const std::string* t = gradient_attr(node, "gradientTransform")) {
            parse_transform(*t, g.transform);
        }
        if (const std::string* s = gradient_attr(node, "spreadMethod")) {
            const std::string_view v = trim(*s);
            g.spread = v == "reflect" ? Spread::Reflect : v == "repeat" ? Spread::Repeat : Spread::Pad;
        }
        const bool bb = g.bbox_units;
        if (g.kind == GradientKind::Linear) {
            g.p0 = {static_cast<float>(gradient_coord(node, "x1", bb, vp.w, 0.0)),
                    static_cast<float>(gradient_coord(node, "y1", bb, vp.h, 0.0))};
            g.p1 = {static_cast<float>(gradient_coord(node, "x2", bb, vp.w, bb ? 1.0 : vp.w)),
                    static_cast<float>(gradient_coord(node, "y2", bb, vp.h, 0.0))};
        } else {
            const double cx = gradient_coord(node, "cx", bb, vp.w, bb ? 0.5 : vp.w * 0.5);
            const double cy = gradient_coord(node, "cy", bb, vp.h, bb ? 0.5 : vp.h * 0.5);
            g.p0 = {static_cast<float>(cx), static_cast<float>(cy)};
            g.p1 = {static_cast<float>(gradient_coord(node, "fx", bb, vp.w, cx)),
                    static_cast<float>(gradient_coord(node, "fy", bb, vp.h, cy))};
            g.radius = static_cast<float>(gradient_coord(node, "r", bb, vp.diagonal(), bb ? 0.5 : vp.diagonal() * 0.5));
            if (gradient_attr(node, "fr")) unsupported("radialGradient fr");
        }

        if (const Node* owner = gradient_stops_owner(node)) {
            float previous = 0.0f;
            for (const auto& child : owner->children) {
                if (child->name != "stop") continue;
                Local local;
                Style style;
                Context stop_ctx;
                for (const auto& [k, v] : declarations(*child)) {
                    apply(k, v, stop_ctx.style, stop_ctx.viewport, style, local);
                }
                float offset = 0.0f;
                if (const std::string* o = child->attribute("offset")) {
                    if (auto value = opacity_value(*o)) offset = *value; // number or %, clamped to [0, 1]
                }
                offset = std::max(offset, previous); // offsets never decrease (13.2.4)
                previous = offset;
                Color c = local.stop_color;
                c.a *= local.stop_opacity;
                g.stops.push_back({offset, c});
            }
        }

        CachedGradient cached;
        if (g.stops.empty()) {
            cached.index = std::nullopt; // paint = none (13.2.4)
        } else if (g.stops.size() == 1) {
            cached.solid = g.stops[0].color;
        } else {
            cached.index = static_cast<std::uint32_t>(document_.gradients.size());
            document_.gradients.push_back(std::move(g));
        }
        gradient_cache_.emplace(&node, cached);
        solid = cached.solid;
        return cached.index;
    }

    Paint resolve_paint(const PaintSpec& spec, const Style& style, const Viewport& vp) {
        Paint p;
        switch (spec.kind) {
            case PaintSpec::Kind::None:
                return p;
            case PaintSpec::Kind::Color:
                p.kind = PaintKind::Solid;
                p.color = spec.color;
                return p;
            case PaintSpec::Kind::CurrentColor:
                p.kind = PaintKind::Solid;
                p.color = style.color;
                return p;
            case PaintSpec::Kind::Url: {
                const Node* target = find(spec.url);
                if (target && (target->name == "linearGradient" || target->name == "radialGradient")) {
                    std::optional<Color> solid;
                    if (auto index = gradient(*target, vp, solid)) {
                        p.kind = PaintKind::Gradient;
                        p.gradient = *index;
                    } else if (solid) {
                        p.kind = PaintKind::Solid;
                        p.color = *solid;
                    }
                    return p;
                }
                if (target && target->name == "pattern") unsupported("pattern paint");
                // Missing or unsupported paint server: use the fallback.
                if (spec.fallback == PaintSpec::Kind::Color) {
                    p.kind = PaintKind::Solid;
                    p.color = spec.fallback_color;
                } else if (spec.fallback == PaintSpec::Kind::CurrentColor) {
                    p.kind = PaintKind::Solid;
                    p.color = style.color;
                }
                return p;
            }
        }
        return p;
    }

    void add_shape(const Node& node, Path path, const Context& ctx) {
        if (!ctx.style.visible) return;
        Shape shape;
        if (const std::string* id = node.attribute("id")) shape.id = *id;
        shape.path = std::move(path);
        shape.transform = ctx.ctm;
        shape.fill = resolve_paint(ctx.style.fill, ctx.style, ctx.viewport);
        shape.fill_rule = ctx.style.fill_rule;
        shape.fill_opacity = ctx.style.fill_opacity;
        shape.stroke = resolve_paint(ctx.style.stroke, ctx.style, ctx.viewport);
        shape.stroke_opacity = ctx.style.stroke_opacity;
        shape.stroke_first = ctx.style.stroke_first;
        shape.stroke_style.width = ctx.style.stroke_width;
        shape.stroke_style.join = ctx.style.join;
        shape.stroke_style.cap = ctx.style.cap;
        shape.stroke_style.miter_limit = ctx.style.miter_limit;
        shape.stroke_style.dashes = ctx.style.dashes;
        shape.stroke_style.dash_offset = ctx.style.dash_offset;
        shape.opacity = ctx.opacity;
        shape.clips = ctx.clips;
        if (shape.stroke_style.width <= 0.0f) shape.stroke = {};
        if (shape.fill.kind == PaintKind::None && shape.stroke.kind == PaintKind::None) return;
        if (shape.opacity <= 0.0f) return;
        document_.shapes.push_back(std::move(shape));
    }

    struct CachedGradient {
        std::optional<std::uint32_t> index;
        std::optional<Color> solid;
    };

    VectorDocument document_;
    std::unordered_map<std::string, const Node*> ids_;
    std::map<const Node*, CachedGradient> gradient_cache_;
    std::set<std::string> unsupported_;
};

} // namespace

SvgResult parse_svg(std::string_view text, std::string_view name) {
    SvgImporter importer;
    SvgResult result = importer.run(text, name);
    if (!result.ok) {
        LOG_ERROR("SVG '{}': {}", name, result.error);
    }
    return result;
}

} // namespace cuervo::vector
