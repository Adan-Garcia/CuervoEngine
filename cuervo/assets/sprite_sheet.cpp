#include "cuervo/assets/sprite_sheet.hpp"

#include <cstdlib>

namespace cuervo::assets {

namespace {

// A tiny JSON reader, enough for sheet metadata: objects keep file order,
// numbers are doubles, nesting is capped (untrusted input).
struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    double number = 0;
    std::string text;
    std::vector<Json> items;                       // array elements / object values
    std::vector<std::string> keys;                 // object keys, parallel to items
    const Json* get(std::string_view key) const {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) {
                return &items[i];
            }
        }
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    bool parse(Json& out) {
        if (!value(out, 0)) {
            return false;
        }
        ws();
        return pos_ == s_.size();
    }

private:
    void ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\t' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }
    bool lit(std::string_view w) {
        if (s_.substr(pos_, w.size()) == w) {
            pos_ += w.size();
            return true;
        }
        return false;
    }
    bool str(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') {
            return false;
        }
        ++pos_;
        while (pos_ < s_.size() && s_[pos_] != '"') {
            char c = s_[pos_++];
            if (c == '\\') {
                if (pos_ >= s_.size()) {
                    return false;
                }
                c = s_[pos_++];
                switch (c) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '"': case '\\': case '/': break;
                default: return false; // \u escapes are not needed for frame names
                }
            }
            out.push_back(c);
        }
        if (pos_ >= s_.size()) {
            return false;
        }
        ++pos_;
        return true;
    }
    bool value(Json& out, int depth) {
        if (depth > 16) {
            return false;
        }
        ws();
        if (pos_ >= s_.size()) {
            return false;
        }
        const char c = s_[pos_];
        if (c == '{' || c == '[') {
            const bool obj = c == '{';
            out.kind = obj ? Json::Kind::Object : Json::Kind::Array;
            ++pos_;
            ws();
            if (pos_ < s_.size() && s_[pos_] == (obj ? '}' : ']')) {
                ++pos_;
                return true;
            }
            for (;;) {
                ws();
                if (obj) {
                    std::string key;
                    if (!str(key)) {
                        return false;
                    }
                    ws();
                    if (pos_ >= s_.size() || s_[pos_++] != ':') {
                        return false;
                    }
                    out.keys.push_back(std::move(key));
                }
                out.items.emplace_back();
                if (!value(out.items.back(), depth + 1)) {
                    return false;
                }
                ws();
                if (pos_ >= s_.size()) {
                    return false;
                }
                const char d = s_[pos_++];
                if (d == (obj ? '}' : ']')) {
                    return true;
                }
                if (d != ',') {
                    return false;
                }
            }
        }
        if (c == '"') {
            out.kind = Json::Kind::String;
            return str(out.text);
        }
        if (lit("true")) {
            out.kind = Json::Kind::Bool;
            out.number = 1;
            return true;
        }
        if (lit("false")) {
            out.kind = Json::Kind::Bool;
            return true;
        }
        if (lit("null")) {
            return true;
        }
        const std::string rest(s_.substr(pos_, 32));
        char* end = nullptr;
        out.number = std::strtod(rest.c_str(), &end);
        if (end == rest.c_str()) {
            return false;
        }
        out.kind = Json::Kind::Number;
        pos_ += static_cast<std::size_t>(end - rest.c_str());
        return true;
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

bool int_field(const Json& obj, std::string_view key, std::int32_t& out) {
    const Json* v = obj.get(key);
    if (v == nullptr || v->kind != Json::Kind::Number || v->number < 0 || v->number > 1e6) {
        return false;
    }
    out = static_cast<std::int32_t>(v->number);
    return static_cast<double>(out) == v->number;
}

bool read_rect(const Json& obj, SheetFrame& f) {
    const Json* r = obj.get("frame");
    const Json& src = (r != nullptr && r->kind == Json::Kind::Object) ? *r : obj;
    return src.kind == Json::Kind::Object && int_field(src, "x", f.x) && int_field(src, "y", f.y) &&
           int_field(src, "w", f.w) && int_field(src, "h", f.h);
}

bool check_frame(const SpriteSheet& sheet, const SheetFrame& f, std::string& error) {
    if (f.w <= 0 || f.h <= 0 || std::int64_t(f.x) + f.w > sheet.width || std::int64_t(f.y) + f.h > sheet.height) {
        error = "frame '" + f.name + "' lies outside the " + std::to_string(sheet.width) + "x" +
                std::to_string(sheet.height) + " sheet";
        return false;
    }
    for (const SheetFrame& g : sheet.frames) {
        if (g.name == f.name) {
            error = "duplicate frame name '" + f.name + "'";
            return false;
        }
    }
    return true;
}

} // namespace

const SheetFrame* SpriteSheet::find(std::string_view name) const {
    for (const SheetFrame& f : frames) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

SheetResult slice_grid(std::int32_t sheet_w, std::int32_t sheet_h, const GridOptions& o) {
    if (sheet_w <= 0 || sheet_h <= 0 || o.frame_w <= 0 || o.frame_h <= 0 || o.margin < 0 || o.spacing < 0) {
        return {std::nullopt, "invalid grid parameters"};
    }
    const std::int32_t fit_cols = (sheet_w - 2 * o.margin + o.spacing) / (o.frame_w + o.spacing);
    const std::int32_t fit_rows = (sheet_h - 2 * o.margin + o.spacing) / (o.frame_h + o.spacing);
    const std::int32_t cols = o.columns > 0 ? o.columns : fit_cols;
    const std::int32_t rows = o.rows > 0 ? o.rows : fit_rows;
    if (cols <= 0 || rows <= 0 || cols > fit_cols || rows > fit_rows) {
        return {std::nullopt, "the grid does not fit the sheet"};
    }
    const std::int32_t count = cols * rows;
    if (!o.names.empty() && static_cast<std::int32_t>(o.names.size()) != count) {
        return {std::nullopt, "grid has " + std::to_string(count) + " cells but " + std::to_string(o.names.size()) +
                                  " names"};
    }
    SpriteSheet sheet;
    sheet.width = sheet_w;
    sheet.height = sheet_h;
    for (std::int32_t i = 0; i < count; ++i) {
        SheetFrame f;
        f.index = i;
        f.name = o.names.empty() ? o.prefix + "_" + std::to_string(i) : o.names[static_cast<std::size_t>(i)];
        f.x = o.margin + (i % cols) * (o.frame_w + o.spacing);
        f.y = o.margin + (i / cols) * (o.frame_h + o.spacing);
        f.w = o.frame_w;
        f.h = o.frame_h;
        std::string error;
        if (!check_frame(sheet, f, error)) {
            return {std::nullopt, error};
        }
        sheet.frames.push_back(std::move(f));
    }
    return {std::move(sheet), {}};
}

SheetResult slice_json(std::string_view json, std::int32_t sheet_w, std::int32_t sheet_h) {
    Json root;
    if (!Parser(json).parse(root) || root.kind != Json::Kind::Object) {
        return {std::nullopt, "sheet metadata is not valid JSON"};
    }
    const Json* frames = root.get("frames");
    if (frames == nullptr || (frames->kind != Json::Kind::Array && frames->kind != Json::Kind::Object)) {
        return {std::nullopt, "sheet metadata has no \"frames\" array or object"};
    }
    SpriteSheet sheet;
    sheet.width = sheet_w;
    sheet.height = sheet_h;
    const bool keyed = frames->kind == Json::Kind::Object;
    for (std::size_t i = 0; i < frames->items.size(); ++i) {
        const Json& item = frames->items[i];
        SheetFrame f;
        f.index = static_cast<std::int32_t>(i);
        if (keyed) {
            f.name = frames->keys[i];
        } else {
            const Json* n = item.get("name");
            if (n == nullptr || n->kind != Json::Kind::String || n->text.empty()) {
                return {std::nullopt, "frame " + std::to_string(i) + " has no name"};
            }
            f.name = n->text;
        }
        if (!read_rect(item, f)) {
            return {std::nullopt, "frame '" + f.name + "' needs integer x, y, w, h"};
        }
        std::string error;
        if (!check_frame(sheet, f, error)) {
            return {std::nullopt, error};
        }
        sheet.frames.push_back(std::move(f));
    }
    return {std::move(sheet), {}};
}

AtlasSpecResult parse_atlas_spec(std::string_view json) {
    Json root;
    if (!Parser(json).parse(root) || root.kind != Json::Kind::Object) {
        return {std::nullopt, "atlas spec is not valid JSON"};
    }
    AtlasSpec spec;
    auto num = [&](const Json& obj, std::string_view key, std::int32_t& out) {
        if (obj.get(key) != nullptr && !int_field(obj, key, out)) return false;
        return true;
    };
    auto flt = [&](const Json& obj, std::string_view key, float& out) {
        const Json* v = obj.get(key);
        if (v == nullptr) return true;
        if (v->kind != Json::Kind::Number || !(v->number > 0.0) || v->number > 64.0) return false;
        out = static_cast<float>(v->number);
        return true;
    };
    if (!num(root, "max_size", spec.max_size) || !num(root, "padding", spec.padding)) {
        return {std::nullopt, "atlas spec: max_size and padding must be non-negative integers"};
    }
    if (const Json* e = root.get("extrude"); e != nullptr && e->kind == Json::Kind::Bool) spec.extrude = e->number != 0;
    if (const Json* e = root.get("power_of_two"); e != nullptr && e->kind == Json::Kind::Bool) {
        spec.power_of_two = e->number != 0;
    }
    if (const Json* sprites = root.get("sprites")) {
        if (sprites->kind != Json::Kind::Array) return {std::nullopt, "atlas spec: \"sprites\" must be an array"};
        for (const Json& item : sprites->items) {
            AtlasSpec::Sprite s;
            if (item.kind == Json::Kind::String) {
                s.path = item.text;
            } else if (item.kind == Json::Kind::Object) {
                const Json* p = item.get("path");
                if (p == nullptr || p->kind != Json::Kind::String) {
                    return {std::nullopt, "atlas spec: a sprite object needs a \"path\""};
                }
                s.path = p->text;
                if (const Json* n = item.get("name"); n != nullptr && n->kind == Json::Kind::String) s.name = n->text;
                if (!flt(item, "scale", s.scale)) return {std::nullopt, "atlas spec: \"scale\" must be in (0, 64]"};
            } else {
                return {std::nullopt, "atlas spec: sprites are paths or objects"};
            }
            if (s.path.empty()) return {std::nullopt, "atlas spec: empty sprite path"};
            spec.sprites.push_back(std::move(s));
        }
    }
    if (const Json* sheets = root.get("sheets")) {
        if (sheets->kind != Json::Kind::Array) return {std::nullopt, "atlas spec: \"sheets\" must be an array"};
        for (const Json& item : sheets->items) {
            AtlasSpec::Sheet s;
            const Json* img = item.kind == Json::Kind::Object ? item.get("image") : nullptr;
            if (img == nullptr || img->kind != Json::Kind::String) {
                return {std::nullopt, "atlas spec: a sheet needs an \"image\""};
            }
            s.image = img->text;
            if (const Json* m = item.get("metadata"); m != nullptr && m->kind == Json::Kind::String) {
                s.metadata = m->text;
            }
            if (!num(item, "frame_w", s.grid.frame_w) || !num(item, "frame_h", s.grid.frame_h) ||
                !num(item, "margin", s.grid.margin) || !num(item, "spacing", s.grid.spacing) ||
                !num(item, "columns", s.grid.columns) || !num(item, "rows", s.grid.rows)) {
                return {std::nullopt, "atlas spec: sheet numbers must be non-negative integers"};
            }
            if (const Json* p = item.get("prefix"); p != nullptr && p->kind == Json::Kind::String) {
                s.grid.prefix = p->text;
            } else {
                const std::size_t slash = s.image.rfind('/');
                std::string stem = slash == std::string::npos ? s.image : s.image.substr(slash + 1);
                if (const std::size_t dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
                s.grid.prefix = stem;
            }
            if (s.metadata.empty() && (s.grid.frame_w <= 0 || s.grid.frame_h <= 0)) {
                return {std::nullopt, "atlas spec: a sheet needs frame_w and frame_h (or \"metadata\")"};
            }
            spec.sheets.push_back(std::move(s));
        }
    }
    if (spec.sprites.empty() && spec.sheets.empty()) return {std::nullopt, "atlas spec lists no sprites or sheets"};
    return {std::move(spec), {}};
}

} // namespace cuervo::assets
