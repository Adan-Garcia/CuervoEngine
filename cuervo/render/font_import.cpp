#include "cuervo/render/font_import.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <thread>

#include "cuervo/core/log.hpp"
#include "cuervo/vector/imported_vector.hpp" // read_file

#ifdef CUERVO_HAS_FONT_IMPORT
#include <msdfgen-ext.h>
#include <msdfgen.h>

#include <msdf-atlas-gen/BitmapAtlasStorage.h>
#include <msdf-atlas-gen/Charset.h>
#include <msdf-atlas-gen/FontGeometry.h>
#include <msdf-atlas-gen/GlyphGeometry.h>
#include <msdf-atlas-gen/ImmediateAtlasGenerator.h>
#include <msdf-atlas-gen/TightAtlasPacker.h>
#include <msdf-atlas-gen/glyph-generators.h>
#endif

namespace cuervo::render {

namespace {

#ifdef CUERVO_HAS_FONT_IMPORT

// RAII for msdfgen's FreeType handles.
struct FreetypeLibrary {
    msdfgen::FreetypeHandle* handle = msdfgen::initializeFreetype();
    ~FreetypeLibrary() {
        if (handle) {
            msdfgen::deinitializeFreetype(handle);
        }
    }
};

struct FontFace {
    msdfgen::FontHandle* handle = nullptr;
    ~FontFace() {
        if (handle) {
            msdfgen::destroyFont(handle);
        }
    }
};

// Corner angle (radians) above which an edge-color change is forced; 3.0 is
// msdf-atlas-gen's default.
constexpr double max_corner_angle = 3.0;
// Fixed seed: the same font always produces the same atlas bytes.
constexpr unsigned long long coloring_seed = 0;

std::optional<FontAtlas> import_with_msdf(std::span<const std::byte> data, std::string_view name,
                                          const FontImportOptions& options) {
    if (data.empty() || data.size() > static_cast<std::size_t>(INT32_MAX)) {
        LOG_ERROR("Font '{}': empty or oversized font data", name);
        return std::nullopt;
    }
    if (!(options.em_size > 0.0f) || !(options.distance_range > 0.0f)) {
        LOG_ERROR("Font '{}': em_size and distance_range must be positive", name);
        return std::nullopt;
    }
    FreetypeLibrary library;
    if (!library.handle) {
        LOG_ERROR("Font '{}': FreeType initialization failed", name);
        return std::nullopt;
    }
    FontFace face;
    face.handle = msdfgen::loadFontData(library.handle, reinterpret_cast<const msdfgen::byte*>(data.data()),
                                        static_cast<int>(data.size()));
    if (!face.handle) {
        LOG_ERROR("Font '{}': FreeType could not parse the font", name);
        return std::nullopt;
    }

    msdf_atlas::Charset charset;
    for (const auto& [first, last] : options.ranges) {
        for (std::uint32_t cp = first; cp <= last && cp <= 0x10FFFF; ++cp) {
            charset.add(cp);
        }
    }

    // Font scale 1: geometry, advances, kerning and metrics in ems.
    std::vector<msdf_atlas::GlyphGeometry> glyphs;
    msdf_atlas::FontGeometry geometry(&glyphs);
    const int loaded = geometry.loadCharset(face.handle, 1.0, charset);
    if (loaded <= 0) {
        LOG_ERROR("Font '{}': none of the requested {} codepoints could be loaded", name, charset.size());
        return std::nullopt;
    }
    if (static_cast<std::size_t>(loaded) < charset.size()) {
        LOG_DEBUG("Font '{}': {} of {} requested codepoints present", name, loaded, charset.size());
    }
    for (msdf_atlas::GlyphGeometry& glyph : glyphs) {
        glyph.edgeColoring(&msdfgen::edgeColoringInkTrap, max_corner_angle, coloring_seed);
    }

    msdf_atlas::TightAtlasPacker packer;
    packer.setDimensionsConstraint(msdf_atlas::DimensionsConstraint::MULTIPLE_OF_FOUR_SQUARE);
    packer.setScale(options.em_size);
    packer.setPixelRange(msdfgen::Range(options.distance_range));
    packer.setMiterLimit(1.0);
    packer.setSpacing(std::max(options.spacing, 0));
    if (const int failed = packer.pack(glyphs.data(), static_cast<int>(glyphs.size())); failed != 0) {
        LOG_ERROR("Font '{}': atlas packing failed ({})", name, failed);
        return std::nullopt;
    }
    int width = 0;
    int height = 0;
    packer.getDimensions(width, height);

    using Generator = msdf_atlas::ImmediateAtlasGenerator<float, 4, msdf_atlas::mtsdfGenerator,
                                                          msdf_atlas::BitmapAtlasStorage<msdf_atlas::byte, 4>>;
    Generator generator(width, height);
    msdf_atlas::GeneratorAttributes attributes;
    attributes.config.overlapSupport = true; // overlapping contours (common in variable/merged fonts)
    attributes.scanlinePass = true;          // fixes the sign of fields of self-intersecting outlines
    generator.setAttributes(attributes);
    const int threads = options.threads > 0 ? options.threads
                                            : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    generator.setThreadCount(threads);
    generator.generate(glyphs.data(), static_cast<int>(glyphs.size()));
    const msdfgen::BitmapConstRef<msdf_atlas::byte, 4> bitmap = generator.atlasStorage();

    FontAtlas font;
    font.name = std::string(name);
    font.em_size = options.em_size;
    font.distance_range = options.distance_range;
    font.width = width;
    font.height = height;
    const msdfgen::FontMetrics& metrics = geometry.getMetrics();
    font.metrics.ascender = static_cast<float>(metrics.ascenderY);
    font.metrics.descender = static_cast<float>(metrics.descenderY);
    font.metrics.line_height = static_cast<float>(metrics.lineHeight);
    font.metrics.underline_y = static_cast<float>(metrics.underlineY);
    font.metrics.underline_thickness = static_cast<float>(metrics.underlineThickness);

    // msdfgen bitmaps are bottom-up (row 0 = y 0 at the bottom); the engine's
    // textures are top row first.
    font.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        std::memcpy(font.pixels.data() + static_cast<std::size_t>(height - 1 - y) * static_cast<std::size_t>(width) * 4,
                    bitmap(0, y), static_cast<std::size_t>(width) * 4);
    }

    std::multimap<int, std::uint32_t> codepoints_of_index; // several codepoints may share a glyph
    for (const msdf_atlas::GlyphGeometry& glyph : glyphs) {
        FontGlyph out;
        out.codepoint = glyph.getCodepoint();
        out.advance = static_cast<float>(glyph.getAdvance());
        if (!glyph.isWhitespace()) {
            double l = 0, b = 0, r = 0, t = 0;
            glyph.getQuadPlaneBounds(l, b, r, t); // ems, y up
            out.left = static_cast<float>(l);
            out.right = static_cast<float>(r);
            out.top = static_cast<float>(-t);
            out.bottom = static_cast<float>(-b);
            glyph.getQuadAtlasBounds(l, b, r, t); // texels, y up from the bottom row
            out.atlas_left = static_cast<float>(l);
            out.atlas_right = static_cast<float>(r);
            out.atlas_top = static_cast<float>(height - t);
            out.atlas_bottom = static_cast<float>(height - b);
        }
        font.glyphs.push_back(out);
        codepoints_of_index.emplace(glyph.getIndex(), out.codepoint);
    }
    std::sort(font.glyphs.begin(), font.glyphs.end(),
              [](const FontGlyph& a, const FontGlyph& b) { return a.codepoint < b.codepoint; });

    for (const auto& [pair, advance] : geometry.getKerning()) {
        const auto firsts = codepoints_of_index.equal_range(pair.first);
        const auto seconds = codepoints_of_index.equal_range(pair.second);
        for (auto a = firsts.first; a != firsts.second; ++a) {
            for (auto b = seconds.first; b != seconds.second; ++b) {
                font.kerning.push_back({a->second, b->second, static_cast<float>(advance)});
            }
        }
    }
    std::sort(font.kerning.begin(), font.kerning.end(), [](const KerningPair& a, const KerningPair& b) {
        return a.first != b.first ? a.first < b.first : a.second < b.second;
    });

    if (std::string invalid = validate_font_atlas(font); !invalid.empty()) {
        LOG_ERROR("Font '{}': imported atlas is invalid: {}", name, invalid);
        return std::nullopt;
    }
    return font;
}

#endif // CUERVO_HAS_FONT_IMPORT

} // namespace

bool font_import_available() {
#ifdef CUERVO_HAS_FONT_IMPORT
    return true;
#else
    return false;
#endif
}

std::optional<FontAtlas> import_font(std::span<const std::byte> font_data, std::string_view name,
                                     const FontImportOptions& options) {
#ifdef CUERVO_HAS_FONT_IMPORT
    return import_with_msdf(font_data, name, options);
#else
    (void)font_data;
    (void)options;
    LOG_ERROR("Font '{}': font import is not compiled in (CUERVO_FONT_IMPORT=OFF); load an .efont atlas", name);
    return std::nullopt;
#endif
}

bool is_sfnt_font(std::span<const std::byte> data) {
    if (data.size() < 4) {
        return false;
    }
    const auto tag = [&](const char (&t)[5]) { return std::memcmp(data.data(), t, 4) == 0; };
    static constexpr char truetype[5] = {'\x00', '\x01', '\x00', '\x00', '\0'};
    return tag(truetype) || tag("OTTO") || tag("true") || tag("ttcf");
}

std::optional<FontAtlas> load_font_file(const std::string& path, const FontImportOptions& options) {
    std::vector<std::byte> data;
    if (!vector::read_file(path, data)) {
        return std::nullopt;
    }
    return load_font_data(data, path, options);
}

std::optional<FontAtlas> load_font_data(std::span<const std::byte> data, std::string_view name,
                                        const FontImportOptions& options) {
    if (is_font_atlas(data)) {
        FontAtlas font;
        std::string error;
        if (!deserialize_font_atlas(data, font, &error)) {
            LOG_ERROR("Font '{}': {}", name, error);
            return std::nullopt;
        }
        return font;
    }
    if (is_sfnt_font(data)) {
        return import_font(data, name, options);
    }
    LOG_ERROR("Font '{}': neither an .efont atlas nor a TrueType/OpenType font", name);
    return std::nullopt;
}

} // namespace cuervo::render
