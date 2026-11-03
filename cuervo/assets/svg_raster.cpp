#include "cuervo/assets/svg_raster.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "cuervo/vector/mesh.hpp"
#include "cuervo/vector/svg.hpp"
#include "cuervo/vector/vector_asset.hpp"

namespace cuervo::assets {

namespace {

constexpr int grid = 4; // samples per pixel side
constexpr int samples = grid * grid;
constexpr std::int32_t band_rows = 8; // pixel rows rasterized at once (bounds the sample buffer)

struct Rgba {
    float r = 0, g = 0, b = 0, a = 0; // premultiplied
};

Rgba mix(const Rgba& x, const Rgba& y, float t) {
    return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t};
}

// Port of gradient() in render/shaders/mesh.frag. (px, py) is in document
// space; `base` indexes the batch's paint window `w`.
Rgba gradient(const std::vector<std::array<float, 4>>& w, std::size_t base, float px, float py) {
    if (base + 4 > w.size()) return {};
    const auto& r0 = w[base];
    const auto& r1 = w[base + 1];
    const auto& g = w[base + 2];
    const auto& h = w[base + 3];
    const float qx = r0[0] * px + r0[1] * py + r0[2];
    const float qy = r1[0] * px + r1[1] * py + r1[2];
    float t;
    if (r0[3] < 0.5f) {
        const float dx = g[2] - g[0], dy = g[3] - g[1];
        t = ((qx - g[0]) * dx + (qy - g[1]) * dy) / std::max(dx * dx + dy * dy, 1e-20f);
    } else {
        const float cdx = g[0] - g[2], cdy = g[1] - g[3];
        const float qdx = qx - g[2], qdy = qy - g[3];
        const float r = h[0];
        const float a = cdx * cdx + cdy * cdy - r * r;
        const float b = qdx * cdx + qdy * cdy;
        const float c = qdx * qdx + qdy * qdy;
        t = (b - std::sqrt(std::max(b * b - a * c, 0.0f))) / std::min(a, -1e-20f);
    }
    const float spread = r1[3];
    if (spread < 0.5f) {
        t = std::clamp(t, 0.0f, 1.0f);
    } else if (spread < 1.5f) {
        const float m = t - 2.0f * std::floor(t * 0.5f); // GLSL mod
        t = 1.0f - std::fabs(m - 1.0f);
    } else {
        t = t - std::floor(t);
    }
    const int n = static_cast<int>(h[1] + 0.5f);
    const std::size_t colors = base + 4;
    const std::size_t offsets = colors + static_cast<std::size_t>(n);
    if (n <= 0 || offsets + static_cast<std::size_t>((n + 3) / 4) > w.size()) return {};
    auto color = [&](int i) {
        const auto& c = w[colors + static_cast<std::size_t>(i)];
        return Rgba{c[0], c[1], c[2], c[3]};
    };
    auto offset = [&](int i) { return w[offsets + static_cast<std::size_t>(i / 4)][static_cast<std::size_t>(i % 4)]; };
    if (t <= offset(0)) return color(0);
    for (int i = 1; i < n; ++i) {
        const float o1 = offset(i);
        if (t <= o1) {
            const float o0 = offset(i - 1);
            const float f = o1 > o0 ? (t - o0) / (o1 - o0) : 1.0f;
            return mix(color(i - 1), color(i), f);
        }
    }
    return color(n - 1);
}

struct Edge {
    float dx, dy, x, y;
    bool owns; // top or left edge: owns samples exactly on it
};

Edge make_edge(float x0, float y0, float x1, float y1) {
    Edge e{y0 - y1, x1 - x0, x0, y0, false};
    e.owns = (e.dx > 0.0f) || (e.dx == 0.0f && e.dy > 0.0f);
    return e;
}

std::uint8_t to_byte(float v) { return static_cast<std::uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f)); }

} // namespace

std::optional<RasterizedSvg> rasterize_vector(const vector::ImportedVector& vec, float pixels_per_unit) {
    vector::VectorAsset asset;
    asset.assign(vec);
    const vec2 size = asset.document().size;
    if (!(size.x() > 0.0f) || !(size.y() > 0.0f) || !(pixels_per_unit > 0.0f)) return std::nullopt;

    float scale = pixels_per_unit;
    const float longest = std::max(size.x(), size.y()) * scale;
    if (longest > static_cast<float>(svg_raster_max_side)) scale *= static_cast<float>(svg_raster_max_side) / longest;
    RasterizedSvg out;
    out.pixels_per_unit = scale;
    out.document_size = size;
    out.image.width = std::clamp(static_cast<std::int32_t>(std::ceil(size.x() * scale)), 1, svg_raster_max_side);
    out.image.height = std::clamp(static_cast<std::int32_t>(std::ceil(size.y() * scale)), 1, svg_raster_max_side);
    const std::int32_t W = out.image.width, H = out.image.height;
    out.image.rgba.assign(static_cast<std::size_t>(W) * static_cast<std::size_t>(H) * 4, 0);

    const vector::VectorAsset::Lod lod = asset.mesh_for_scale(scale);
    if (lod.mesh == nullptr) return out; // nothing drawable: a transparent image
    const vector::VectorMesh& mesh = *lod.mesh;

    std::vector<Rgba> buf(static_cast<std::size_t>(W) * band_rows * samples);
    const float inv = 1.0f / scale;
    for (std::int32_t y0 = 0; y0 < H; y0 += band_rows) {
        const std::int32_t rows = std::min(band_rows, H - y0);
        std::fill(buf.begin(), buf.end(), Rgba{});
        for (const vector::MeshBatch& batch : mesh.batches) {
            const std::uint32_t end = std::min<std::uint32_t>(batch.first_index + batch.index_count,
                                                              static_cast<std::uint32_t>(mesh.indices.size()));
            for (std::uint32_t ii = batch.first_index; ii + 2 < end; ii += 3) {
                const vector::MeshVertex& v0 = mesh.vertices[mesh.indices[ii]];
                const vector::MeshVertex& v1 = mesh.vertices[mesh.indices[ii + 1]];
                const vector::MeshVertex& v2 = mesh.vertices[mesh.indices[ii + 2]];
                float ax = v0.position.x() * scale, ay = v0.position.y() * scale;
                float bx = v1.position.x() * scale, by = v1.position.y() * scale;
                float cx = v2.position.x() * scale, cy = v2.position.y() * scale;
                const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
                if (area == 0.0f) continue;
                if (area < 0.0f) {
                    std::swap(bx, cx);
                    std::swap(by, cy);
                }
                const std::int32_t px0 = std::max(static_cast<std::int32_t>(std::floor(std::min({ax, bx, cx}))), 0);
                const std::int32_t px1 = std::min(static_cast<std::int32_t>(std::floor(std::max({ax, bx, cx}))), W - 1);
                const std::int32_t py0 = std::max(static_cast<std::int32_t>(std::floor(std::min({ay, by, cy}))), y0);
                const std::int32_t py1 =
                    std::min(static_cast<std::int32_t>(std::floor(std::max({ay, by, cy}))), y0 + rows - 1);
                if (px0 > px1 || py0 > py1) continue;
                const Edge edges[3] = {make_edge(ax, ay, bx, by), make_edge(bx, by, cx, cy), make_edge(cx, cy, ax, ay)};

                const std::uint32_t col = v0.color;
                const float ca = static_cast<float>((col >> 24) & 255u) / 255.0f;
                const Rgba tint{static_cast<float>(col & 255u) / 255.0f * ca,
                                static_cast<float>((col >> 8) & 255u) / 255.0f * ca,
                                static_cast<float>((col >> 16) & 255u) / 255.0f * ca, ca};
                const bool solid = v0.paint == vector::solid_paint;

                for (std::int32_t py = py0; py <= py1; ++py) {
                    for (std::int32_t px = px0; px <= px1; ++px) {
                        for (int s = 0; s < samples; ++s) {
                            const float sx = static_cast<float>(px) + (static_cast<float>(s % grid) + 0.5f) / grid;
                            const float sy = static_cast<float>(py) + (static_cast<float>(s / grid) + 0.5f) / grid;
                            bool in = true;
                            for (const Edge& e : edges) {
                                const float v = e.dx * (sx - e.x) + e.dy * (sy - e.y);
                                if (v < 0.0f || (v == 0.0f && !e.owns)) {
                                    in = false;
                                    break;
                                }
                            }
                            if (!in) continue;
                            Rgba src = tint;
                            if (!solid) {
                                const Rgba g = gradient(batch.paints, v0.paint, sx * inv, sy * inv);
                                src = {g.r * tint.r, g.g * tint.g, g.b * tint.b, g.a * tint.a};
                            }
                            const std::size_t at = (static_cast<std::size_t>(py - y0) * static_cast<std::size_t>(W) +
                                                    static_cast<std::size_t>(px)) *
                                                       samples +
                                                   static_cast<std::size_t>(s);
                            Rgba& d = buf[at];
                            const float k = 1.0f - src.a;
                            d = {src.r + d.r * k, src.g + d.g * k, src.b + d.b * k, src.a + d.a * k};
                        }
                    }
                }
            }
        }
        // Resolve: average the samples (premultiplied), then un-premultiply.
        for (std::int32_t y = 0; y < rows; ++y) {
            for (std::int32_t x = 0; x < W; ++x) {
                Rgba sum;
                const std::size_t base =
                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * samples;
                for (int s = 0; s < samples; ++s) {
                    const Rgba& d = buf[base + static_cast<std::size_t>(s)];
                    sum.r += d.r;
                    sum.g += d.g;
                    sum.b += d.b;
                    sum.a += d.a;
                }
                if (sum.a <= 0.0f) continue;
                std::uint8_t* o = &out.image.rgba[(static_cast<std::size_t>(y0 + y) * static_cast<std::size_t>(W) +
                                                   static_cast<std::size_t>(x)) *
                                                  4];
                const float k = 1.0f / sum.a;
                o[0] = to_byte(sum.r * k);
                o[1] = to_byte(sum.g * k);
                o[2] = to_byte(sum.b * k);
                o[3] = to_byte(sum.a / samples);
            }
        }
    }
    return out;
}

SvgRasterResult rasterize_svg(std::string_view svg_text, float pixels_per_unit, std::string_view name) {
    SvgRasterResult result;
    vector::SvgResult parsed = vector::parse_svg(svg_text, name);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }
    result.unsupported = std::move(parsed.unsupported);
    vector::ImportedVector vec;
    vec.document = std::move(parsed.document);
    result.raster = rasterize_vector(vec, pixels_per_unit);
    if (!result.raster) result.error = "document has no size";
    return result;
}

} // namespace cuervo::assets
