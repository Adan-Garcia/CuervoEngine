#include "editor/sprite_tools.hpp"

#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

#include "cuervo/assets/atlas_file.hpp"
#include "cuervo/assets/image.hpp"

namespace cuervo::editor {

namespace fs = std::filesystem;

namespace {

bool read_file(const fs::path& file, std::vector<std::byte>& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out.resize(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) out[i] = static_cast<std::byte>(raw[i]);
    return true;
}

bool safe_relative(std::string_view p) {
    return !(p.empty() || p.front() == '/' || p.find("..") != std::string_view::npos ||
             p.find('\\') != std::string_view::npos);
}

std::string stem_of(std::string_view path) {
    std::string s(path);
    if (const std::size_t dot = s.rfind('.'); dot != std::string::npos && (s.rfind('/') == std::string::npos || dot > s.rfind('/'))) {
        s.resize(dot);
    }
    return s;
}

// JSON string with the escapes a path or frame name can need.
void json_string(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    out += '"';
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

ImageInfo image_info(const fs::path& project_dir, std::string_view path) {
    ImageInfo info;
    if (!safe_relative(path)) {
        info.error = "not a project path";
        return info;
    }
    std::vector<std::byte> bytes;
    if (!read_file(project_dir / std::string(path), bytes)) {
        info.error = std::format("cannot read '{}'", path);
        return info;
    }
    assets::DecodeResult r = assets::decode_image(bytes);
    if (!r.image) {
        info.error = r.error;
        return info;
    }
    info.ok = true;
    info.width = r.image->width;
    info.height = r.image->height;
    return info;
}

AtlasEditor AtlasEditor::create(const fs::path& project_dir, std::string path) {
    AtlasEditor a;
    a.dir_ = project_dir;
    a.path_ = std::move(path);
    a.dirty_ = true;
    return a;
}

std::optional<AtlasEditor> AtlasEditor::open(const fs::path& project_dir, std::string path, std::string& error) {
    if (!safe_relative(path) || !fs::exists(project_dir / path)) {
        error = std::format("cannot read '{}'", path);
        return std::nullopt;
    }
    assets::AtlasSpecResult r = assets::parse_atlas_spec(read_text(project_dir / path));
    if (!r.spec) {
        error = r.error;
        return std::nullopt;
    }
    AtlasEditor a;
    a.dir_ = project_dir;
    a.path_ = std::move(path);
    a.spec_ = std::move(*r.spec);
    return a;
}

bool AtlasEditor::add_sprite(std::string_view image, std::string_view name, float scale, std::string& error) {
    if (!safe_relative(image) || !fs::exists(dir_ / std::string(image))) {
        error = std::format("'{}' is not a file of the project", image);
        return false;
    }
    if (!(scale > 0.0f && scale <= 64.0f)) {
        error = "scale must be in (0, 64]";
        return false;
    }
    assets::AtlasSpec::Sprite s;
    s.path = std::string(image);
    s.name = std::string(name);
    s.scale = scale;
    spec_.sprites.push_back(std::move(s));
    dirty_ = true;
    return true;
}

bool AtlasEditor::add_sheet(std::string_view image, const assets::GridOptions& grid, std::string& error) {
    if (grid.frame_w <= 0 || grid.frame_h <= 0) {
        error = "frame size must be positive";
        return false;
    }
    const ImageInfo info = image_info(dir_, image);
    if (!info.ok) {
        error = info.error;
        return false;
    }
    assets::GridOptions g = grid;
    if (g.prefix.empty() || g.prefix == "frame") {
        const std::string stem = stem_of(image);
        const std::size_t slash = stem.rfind('/');
        if (g.prefix.empty()) g.prefix = slash == std::string::npos ? stem : stem.substr(slash + 1);
    }
    assets::SheetResult sliced = assets::slice_grid(info.width, info.height, g);
    if (!sliced.sheet) {
        error = sliced.error;
        return false;
    }
    assets::AtlasSpec::Sheet s;
    s.image = std::string(image);
    s.grid = std::move(g);
    spec_.sheets.push_back(std::move(s));
    dirty_ = true;
    return true;
}

bool AtlasEditor::remove_sprite(std::size_t index) {
    if (index >= spec_.sprites.size()) return false;
    spec_.sprites.erase(spec_.sprites.begin() + static_cast<std::ptrdiff_t>(index));
    dirty_ = true;
    return true;
}

bool AtlasEditor::remove_sheet(std::size_t index) {
    if (index >= spec_.sheets.size()) return false;
    spec_.sheets.erase(spec_.sheets.begin() + static_cast<std::ptrdiff_t>(index));
    dirty_ = true;
    return true;
}

void AtlasEditor::set_packing(std::int32_t max_size, std::int32_t padding, bool extrude, bool power_of_two) {
    spec_.max_size = max_size;
    spec_.padding = padding;
    spec_.extrude = extrude;
    spec_.power_of_two = power_of_two;
    dirty_ = true;
}

std::optional<assets::SpriteSheet> AtlasEditor::sheet_frames(std::size_t index, std::string& error) const {
    if (index >= spec_.sheets.size()) {
        error = "no such sheet";
        return std::nullopt;
    }
    const assets::AtlasSpec::Sheet& sheet = spec_.sheets[index];
    const ImageInfo info = image_info(dir_, sheet.image);
    if (!info.ok) {
        error = info.error;
        return std::nullopt;
    }
    assets::SheetResult r = sheet.metadata.empty()
                                ? assets::slice_grid(info.width, info.height, sheet.grid)
                                : assets::slice_json(read_text(dir_ / sheet.metadata), info.width, info.height);
    if (!r.sheet) {
        error = r.error;
        return std::nullopt;
    }
    return std::move(r.sheet);
}

std::vector<std::string> AtlasEditor::frame_names(std::string* error) const {
    std::vector<std::string> names;
    for (const assets::AtlasSpec::Sprite& s : spec_.sprites) names.push_back(s.name.empty() ? stem_of(s.path) : s.name);
    for (std::size_t i = 0; i < spec_.sheets.size(); ++i) {
        std::string err;
        std::optional<assets::SpriteSheet> sheet = sheet_frames(i, err);
        if (!sheet) {
            if (error != nullptr && error->empty()) *error = err;
            continue;
        }
        for (const assets::SheetFrame& f : sheet->frames) names.push_back(f.name);
    }
    return names;
}

std::optional<AtlasEditor::FrameSource> AtlasEditor::frame_source(std::string_view name) const {
    for (const assets::AtlasSpec::Sprite& s : spec_.sprites) {
        if ((s.name.empty() ? stem_of(s.path) : s.name) == name) return FrameSource{s.path, 0, 0, 0, 0};
    }
    for (std::size_t i = 0; i < spec_.sheets.size(); ++i) {
        std::string err;
        std::optional<assets::SpriteSheet> sheet = sheet_frames(i, err);
        if (!sheet) continue;
        for (const assets::SheetFrame& f : sheet->frames) {
            if (f.name == name) return FrameSource{spec_.sheets[i].image, f.x, f.y, f.w, f.h};
        }
    }
    return std::nullopt;
}

std::string AtlasEditor::to_json() const {
    std::string o = std::format("{{\n  \"max_size\": {},\n  \"padding\": {},\n  \"extrude\": {},\n  \"power_of_two\": {},\n",
                                spec_.max_size, spec_.padding, spec_.extrude ? "true" : "false",
                                spec_.power_of_two ? "true" : "false");
    o += "  \"sprites\": [";
    for (std::size_t i = 0; i < spec_.sprites.size(); ++i) {
        const assets::AtlasSpec::Sprite& s = spec_.sprites[i];
        o += i == 0 ? "\n    " : ",\n    ";
        if (s.name.empty() && s.scale == 1.0f) {
            json_string(o, s.path);
        } else {
            o += "{\"path\": ";
            json_string(o, s.path);
            if (!s.name.empty()) {
                o += ", \"name\": ";
                json_string(o, s.name);
            }
            if (s.scale != 1.0f) o += std::format(", \"scale\": {}", s.scale);
            o += "}";
        }
    }
    o += spec_.sprites.empty() ? "],\n" : "\n  ],\n";
    o += "  \"sheets\": [";
    for (std::size_t i = 0; i < spec_.sheets.size(); ++i) {
        const assets::AtlasSpec::Sheet& s = spec_.sheets[i];
        o += i == 0 ? "\n    {\"image\": " : ",\n    {\"image\": ";
        json_string(o, s.image);
        if (!s.metadata.empty()) {
            o += ", \"metadata\": ";
            json_string(o, s.metadata);
        } else {
            const assets::GridOptions& g = s.grid;
            o += std::format(", \"frame_w\": {}, \"frame_h\": {}", g.frame_w, g.frame_h);
            if (g.margin != 0) o += std::format(", \"margin\": {}", g.margin);
            if (g.spacing != 0) o += std::format(", \"spacing\": {}", g.spacing);
            if (g.columns != 0) o += std::format(", \"columns\": {}", g.columns);
            if (g.rows != 0) o += std::format(", \"rows\": {}", g.rows);
        }
        o += ", \"prefix\": ";
        json_string(o, s.grid.prefix);
        o += "}";
    }
    o += spec_.sheets.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return o;
}

bool AtlasEditor::save(std::string& error) {
    const fs::path file = dir_ / path_;
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = std::format("cannot write '{}'", path_);
        return false;
    }
    out << to_json();
    out.close();
    if (!out) {
        error = std::format("cannot write '{}'", path_);
        return false;
    }
    dirty_ = false;
    return true;
}

std::vector<std::string> atlas_frame_names(const fs::path& project_dir, std::string_view path, std::string& error) {
    std::vector<std::string> names;
    if (path.ends_with(".catlas")) {
        std::vector<std::byte> bytes;
        if (!safe_relative(path) || !read_file(project_dir / std::string(path), bytes)) {
            error = std::format("cannot read '{}'", path);
            return names;
        }
        auto atlas = assets::decode_catlas(bytes);
        if (!atlas) {
            error = "not a valid .catlas";
            return names;
        }
        for (const assets::AtlasFrame& f : atlas->frames) names.push_back(f.name);
        return names;
    }
    auto editor = AtlasEditor::open(project_dir, std::string(path), error);
    if (!editor) return names;
    return editor->frame_names(&error);
}

} // namespace cuervo::editor
