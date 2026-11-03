#include "cuervo/assets/atlas_file.hpp"

#include <algorithm>
#include <cstring>

#include "cuervo/assets/svg_raster.hpp"
#include "cuervo/vector/imported_vector.hpp"

namespace cuervo::assets {

namespace {

constexpr std::uint32_t kCatlVersion = 1;

void put32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xffu));
}

std::uint32_t get32(const std::byte* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
    return v;
}

std::string lower_ext(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::size_t dot = path.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    std::string e(path.substr(dot));
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string strip_ext(std::string_view path) {
    const std::string e = lower_ext(path);
    return std::string(path.substr(0, path.size() - e.size()));
}

// One source file as an image; SVG and .evec are rasterized at `scale`.
bool load_image(const FileReader& read, const std::string& path, float scale, Image& out,
                std::vector<std::string>& warnings, std::string& error) {
    std::vector<std::byte> bytes;
    if (!read(path, bytes, error)) return false;
    const std::string ext = lower_ext(path);
    if (ext == ".svg") {
        SvgRasterResult r = rasterize_svg(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                                          scale, path);
        if (!r.raster) {
            error = "'" + path + "': " + (r.error.empty() ? "SVG raster failed" : r.error);
            return false;
        }
        for (const std::string& u : r.unsupported) warnings.push_back(path + ": unsupported SVG feature '" + u + "'");
        out = std::move(r.raster->image);
        return true;
    }
    if (vector::is_imported_vector(bytes)) {
        vector::ImportedVector vec;
        std::string e;
        if (!vector::deserialize_imported_vector(bytes, vec, &e)) {
            error = "'" + path + "': " + e;
            return false;
        }
        auto r = rasterize_vector(vec, scale);
        if (!r) {
            error = "'" + path + "': vector image has no size";
            return false;
        }
        out = std::move(r->image);
        return true;
    }
    DecodeResult d = decode_image(bytes);
    if (!d.image) {
        error = "'" + path + "': " + d.error;
        return false;
    }
    out = std::move(*d.image);
    return true;
}

Image crop(const Image& src, const SheetFrame& f) {
    Image out;
    out.width = f.w;
    out.height = f.h;
    out.rgba.resize(static_cast<std::size_t>(f.w) * static_cast<std::size_t>(f.h) * 4);
    for (std::int32_t y = 0; y < f.h; ++y) {
        std::memcpy(&out.rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(f.w) * 4],
                    &src.rgba[(static_cast<std::size_t>(f.y + y) * static_cast<std::size_t>(src.width) +
                               static_cast<std::size_t>(f.x)) *
                              4],
                    static_cast<std::size_t>(f.w) * 4);
    }
    return out;
}

} // namespace

const AtlasFrame* PackedAtlas::find(std::string_view name) const {
    auto it = std::lower_bound(frames.begin(), frames.end(), name,
                               [](const AtlasFrame& f, std::string_view n) { return f.name < n; });
    return it != frames.end() && it->name == name ? &*it : nullptr;
}

AtlasBuildResult build_atlas(const AtlasSpec& spec, const FileReader& read) {
    AtlasBuildResult result;
    std::vector<AtlasInput> inputs;
    for (const AtlasSpec::Sprite& s : spec.sprites) {
        AtlasInput in;
        in.name = s.name.empty() ? strip_ext(s.path) : s.name;
        if (!load_image(read, s.path, s.scale, in.image, result.warnings, result.error)) return result;
        inputs.push_back(std::move(in));
    }
    for (const AtlasSpec::Sheet& sh : spec.sheets) {
        Image img;
        if (!load_image(read, sh.image, 1.0f, img, result.warnings, result.error)) return result;
        SheetResult sliced;
        if (!sh.metadata.empty()) {
            std::vector<std::byte> meta;
            if (!read(sh.metadata, meta, result.error)) return result;
            sliced = slice_json(std::string_view(reinterpret_cast<const char*>(meta.data()), meta.size()), img.width,
                                img.height);
        } else {
            sliced = slice_grid(img.width, img.height, sh.grid);
        }
        if (!sliced.sheet) {
            result.error = "'" + sh.image + "': " + sliced.error;
            return result;
        }
        for (const SheetFrame& f : sliced.sheet->frames) {
            inputs.push_back({sh.metadata.empty() ? sh.grid.prefix + "_" + std::to_string(f.index) : f.name,
                              crop(img, f)});
        }
    }
    AtlasOptions options;
    options.max_size = spec.max_size;
    options.padding = spec.padding;
    options.extrude = spec.extrude;
    options.power_of_two = spec.power_of_two;
    AtlasResult packed = pack_atlas(inputs, options);
    if (!packed.atlas) {
        result.error = packed.error;
        return result;
    }
    PackedAtlas out;
    out.frames = std::move(packed.atlas->frames);
    for (const Image& page : packed.atlas->pages) out.pages.push_back(build_texture(page, true));
    result.atlas = std::move(out);
    return result;
}

AtlasBuildResult build_atlas_from_json(std::string_view spec_json, const FileReader& read) {
    AtlasSpecResult spec = parse_atlas_spec(spec_json);
    if (!spec.spec) {
        AtlasBuildResult r;
        r.error = spec.error;
        return r;
    }
    return build_atlas(*spec.spec, read);
}

std::vector<std::byte> encode_catlas(const PackedAtlas& atlas) {
    std::vector<std::byte> out;
    for (char c : {'C', 'A', 'T', 'L'}) out.push_back(static_cast<std::byte>(c));
    put32(out, kCatlVersion);
    put32(out, static_cast<std::uint32_t>(atlas.pages.size()));
    put32(out, static_cast<std::uint32_t>(atlas.frames.size()));
    for (const TextureData& page : atlas.pages) {
        const std::vector<std::byte> ctex = encode_ctex(page);
        put32(out, static_cast<std::uint32_t>(ctex.size()));
        out.insert(out.end(), ctex.begin(), ctex.end());
    }
    for (const AtlasFrame& f : atlas.frames) {
        put32(out, static_cast<std::uint32_t>(f.name.size()));
        for (char c : f.name) out.push_back(static_cast<std::byte>(c));
        for (std::int32_t v : {f.page, f.x, f.y, f.w, f.h}) put32(out, static_cast<std::uint32_t>(v));
    }
    return out;
}

std::optional<PackedAtlas> decode_catlas(std::span<const std::byte> bytes) {
    if (bytes.size() < 16 || std::memcmp(bytes.data(), "CATL", 4) != 0 || get32(bytes.data() + 4) != kCatlVersion) {
        return std::nullopt;
    }
    const std::uint32_t pages = get32(bytes.data() + 8);
    const std::uint32_t frames = get32(bytes.data() + 12);
    if (pages > 256 || frames > (1u << 20)) return std::nullopt;
    PackedAtlas out;
    std::size_t at = 16;
    for (std::uint32_t i = 0; i < pages; ++i) {
        if (at + 4 > bytes.size()) return std::nullopt;
        const std::size_t n = get32(bytes.data() + at);
        at += 4;
        if (n > bytes.size() - at) return std::nullopt;
        auto page = decode_ctex(bytes.subspan(at, n));
        if (!page) return std::nullopt;
        out.pages.push_back(std::move(*page));
        at += n;
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
        if (at + 4 > bytes.size()) return std::nullopt;
        const std::size_t n = get32(bytes.data() + at);
        at += 4;
        if (n > bytes.size() - at || bytes.size() - at - n < 20) return std::nullopt;
        AtlasFrame f;
        f.name.resize(n);
        std::memcpy(f.name.data(), bytes.data() + at, n);
        at += n;
        f.page = static_cast<std::int32_t>(get32(bytes.data() + at));
        f.x = static_cast<std::int32_t>(get32(bytes.data() + at + 4));
        f.y = static_cast<std::int32_t>(get32(bytes.data() + at + 8));
        f.w = static_cast<std::int32_t>(get32(bytes.data() + at + 12));
        f.h = static_cast<std::int32_t>(get32(bytes.data() + at + 16));
        at += 20;
        if (f.page < 0 || static_cast<std::size_t>(f.page) >= out.pages.size() || f.w < 0 || f.h < 0) {
            return std::nullopt;
        }
        out.frames.push_back(std::move(f));
    }
    if (!std::is_sorted(out.frames.begin(), out.frames.end(),
                        [](const AtlasFrame& a, const AtlasFrame& b) { return a.name < b.name; })) {
        return std::nullopt;
    }
    return out;
}

} // namespace cuervo::assets
