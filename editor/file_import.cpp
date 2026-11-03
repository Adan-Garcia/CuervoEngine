#include "editor/file_import.hpp"

#include <format>
#include <fstream>
#include <system_error>

#include "editor/sprite_tools.hpp"

namespace cuervo::editor {

namespace fs = std::filesystem;

std::string_view import_folder(AssetKind kind) {
    switch (kind) {
        case AssetKind::Image:
        case AssetKind::Vector: return "art";
        case AssetKind::Sound: return "sounds";
        case AssetKind::Font: return "fonts";
        default: return {};
    }
}

namespace {

bool has_svg_element(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string head(65536, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    return head.find("<svg") != std::string::npos;
}

bool safe_folder(std::string_view f) {
    return f.empty() || (f.front() != '/' && f.find("..") == std::string_view::npos && f.find('\\') == std::string_view::npos);
}

ImportResult import_one(const fs::path& project_dir, const fs::path& source, std::string_view folder) {
    ImportResult r;
    r.source = source.string();
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) {
        r.error = "not a file";
        return r;
    }
    r.kind = asset_kind_of(source);
    const std::string_view default_folder = import_folder(r.kind);
    if (default_folder.empty() || r.kind == AssetKind::Atlas) {
        r.error = std::format("'{}' is not an image, SVG, sound or font", source.filename().string());
        return r;
    }
    if (fs::file_size(source, ec) > max_import_bytes) {
        r.error = "file is too large to import";
        return r;
    }
    const std::string target_folder = folder.empty() ? std::string(default_folder) : std::string(folder);
    const fs::path dir = project_dir / target_folder;
    fs::create_directories(dir, ec);
    // Never overwrite: name, name_2, name_3...
    const std::string stem = source.stem().string();
    const std::string ext = source.extension().string();
    fs::path dest = dir / (stem + ext);
    for (int n = 2; fs::exists(dest); ++n) dest = dir / std::format("{}_{}{}", stem, n, ext);
    fs::copy_file(source, dest, ec);
    if (ec) {
        r.error = std::format("cannot copy: {}", ec.message());
        return r;
    }
    const std::string rel = target_folder + "/" + dest.filename().string();
    bool valid = true;
    if (r.kind == AssetKind::Image) {
        const ImageInfo info = image_info(project_dir, rel);
        valid = info.ok;
        if (!valid) r.error = info.error;
    } else if (r.kind == AssetKind::Vector && dest.extension() == ".svg") {
        valid = has_svg_element(dest);
        if (!valid) r.error = "not an SVG document";
    }
    if (!valid) {
        fs::remove(dest, ec);
        return r;
    }
    r.ok = true;
    r.dest = rel;
    return r;
}

} // namespace

std::vector<ImportResult> import_files(const fs::path& project_dir, const std::vector<fs::path>& sources,
                                       std::string_view folder) {
    std::vector<ImportResult> out;
    for (const fs::path& s : sources) {
        if (!safe_folder(folder)) {
            ImportResult r;
            r.source = s.string();
            r.error = "the folder must be inside the project";
            out.push_back(std::move(r));
            continue;
        }
        out.push_back(import_one(project_dir, s, folder));
    }
    return out;
}

} // namespace cuervo::editor
