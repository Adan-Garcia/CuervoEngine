#include "editor/rml/rml_rhi.hpp"

#include <algorithm>
#include <deque>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

#include "cuervo/assets/image.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/vector/flatten.hpp"
#include "editor/icons.hpp"
#include "editor/inspector_model.hpp"
#include "shaders/editor_ui_frag.hpp"
#include "shaders/editor_ui_vert.hpp"

namespace cuervo::editor {

namespace {

static_assert(sizeof(Rml::Vertex) == 20);
constexpr std::array<rhi::VertexAttribute, 3> vertex_attributes{{
    {0, rhi::VertexFormat::Float2, offsetof(Rml::Vertex, position)},
    {1, rhi::VertexFormat::Float2, offsetof(Rml::Vertex, tex_coord)},
    {2, rhi::VertexFormat::UByte4Norm, offsetof(Rml::Vertex, colour)}, // premultiplied RGBA bytes
}};

struct Globals {
    float scale_offset[4];
};

} // namespace

RmlRenderer::~RmlRenderer() { shutdown(); }

bool RmlRenderer::init(rhi::Device& device) {
    shutdown();
    device_ = &device;
    const std::array<rhi::VertexStream, 1> streams{{{sizeof(Rml::Vertex), rhi::VertexRate::PerVertex, vertex_attributes}}};
    rhi::PipelineDesc desc;
    desc.vertex = shaders::editor_ui_vert;
    desc.fragment = shaders::editor_ui_frag;
    desc.streams = streams;
    desc.blend = rhi::BlendMode::Premultiplied;
    desc.debug_name = "rml";
    pipeline_ = device.create_pipeline(desc);
    globals_ = device.create_buffer({rhi::BufferKind::Uniform, rhi::BufferUsage::Dynamic, sizeof(Globals), nullptr});
    const std::uint8_t px[4] = {255, 255, 255, 255};
    rhi::TextureDesc td;
    td.width = 1;
    td.height = 1;
    td.format = rhi::TextureFormat::RGBA8;
    td.filter = rhi::Filter::Linear;
    td.initial_data = px;
    td.debug_name = "rml white";
    white_ = device.create_texture(td);
    if (!pipeline_ || !globals_ || !white_) {
        LOG_ERROR("RmlUi renderer: failed to create GPU resources");
        shutdown();
        return false;
    }
    return true;
}

void RmlRenderer::shutdown() {
    if (device_ == nullptr) return;
    device_->destroy_pipeline(pipeline_);
    device_->destroy_buffer(vertices_);
    device_->destroy_buffer(indices_);
    device_->destroy_buffer(globals_);
    device_->destroy_texture(white_);
    pipeline_ = {};
    vertices_ = indices_ = globals_ = {};
    white_ = {};
    vertex_capacity_ = index_capacity_ = 0;
    device_ = nullptr;
}

Rml::CompiledGeometryHandle RmlRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
    Geometry g;
    g.vertices.assign(vertices.begin(), vertices.end());
    g.indices.assign(indices.begin(), indices.end());
    const Rml::CompiledGeometryHandle h = next_geometry_++;
    geometries_.emplace(h, std::move(g));
    return h;
}

void RmlRenderer::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) {
    const auto it = geometries_.find(geometry);
    if (it == geometries_.end()) return;
    const Geometry& g = it->second;
    const auto base = static_cast<std::uint32_t>(frame_vertices_.size());
    for (Rml::Vertex v : g.vertices) {
        v.position += translation;
        frame_vertices_.push_back(v);
    }
    const auto first = static_cast<std::uint32_t>(frame_indices_.size());
    for (const int i : g.indices) frame_indices_.push_back(base + static_cast<std::uint32_t>(i));
    // Consecutive geometry with the same texture and scissor merges into one draw.
    if (!draws_.empty()) {
        Draw& d = draws_.back();
        if (d.texture == static_cast<std::uint32_t>(texture) && d.scissor == scissor_on_ &&
            (!scissor_on_ || (d.rect.Left() == scissor_.Left() && d.rect.Top() == scissor_.Top() &&
                              d.rect.Right() == scissor_.Right() && d.rect.Bottom() == scissor_.Bottom())) &&
            d.first_index + d.count == first) {
            d.count += static_cast<std::uint32_t>(g.indices.size());
            return;
        }
    }
    draws_.push_back({static_cast<std::uint32_t>(texture), first, static_cast<std::uint32_t>(g.indices.size()), scissor_on_, scissor_});
}

void RmlRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) { geometries_.erase(geometry); }

namespace {
// Premultiplied RGBA8. h: white to clear (left to right); v: clear to black (top to bottom);
// hue: the rainbow; fade: dark gray to clear (the alpha strip sits on the color).
std::vector<std::uint8_t> rasterize_gradient(const std::string& name, Rml::Vector2i& dim) {
    const bool vertical = name == "v";
    dim = vertical ? Rml::Vector2i{1, 64} : Rml::Vector2i{name == "hue" ? 256 : 64, 1};
    const int n = vertical ? dim.y : dim.x;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(n) * 4);
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        float r = 0, g = 0, b = 0, a = 1;
        if (name == "h") { r = g = b = a = 1.0f - t; }
        else if (name == "v") { a = t; }
        else if (name == "fade") { a = 1.0f - t; r = g = b = 0.16f * a; }
        else if (name == "hue") { ve::hsv_to_rgb(std::min(t, 0.9999f), 1, 1, r, g, b); }
        else return {};
        const auto q = [](float x) { return static_cast<std::uint8_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f)); };
        out[static_cast<std::size_t>(i) * 4 + 0] = q(r);
        out[static_cast<std::size_t>(i) * 4 + 1] = q(g);
        out[static_cast<std::size_t>(i) * 4 + 2] = q(b);
        out[static_cast<std::size_t>(i) * 4 + 3] = q(a);
    }
    return out;
}
} // namespace

namespace {
std::deque<std::pair<std::string, assets::Image>>& memory_images() {
    static std::deque<std::pair<std::string, assets::Image>> images;
    return images;
}
} // namespace

void rml_set_memory_image(const std::string& key, const assets::Image& image) {
    auto& all = memory_images();
    for (const auto& e : all) if (e.first == key) return;
    all.emplace_back(key, image);
    if (all.size() > 6) all.pop_front();
}

Rml::TextureHandle RmlRenderer::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) {
    // "check:<n>": an n x n opaque checkerboard (one pixel per cell, nearest) behind transparent previews.
    if (source.starts_with("check:")) {
        const int n = std::clamp(std::atoi(source.c_str() + 6), 2, 64);
        std::vector<std::uint8_t> bits(static_cast<std::size_t>(n) * static_cast<std::size_t>(n) * 4);
        for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
            const std::uint8_t v = ((x + y) & 1) != 0 ? 0x5C : 0x4A;
            std::uint8_t* o = &bits[(static_cast<std::size_t>(y) * static_cast<std::size_t>(n) + static_cast<std::size_t>(x)) * 4];
            o[0] = o[1] = o[2] = v;
            o[3] = 255;
        }
        dimensions = {n, n};
        return make_texture({reinterpret_cast<const Rml::byte*>(bits.data()), bits.size()}, dimensions, rhi::Filter::Nearest);
    }
    // "mem:<key>": a registered in-memory image (rml_set_memory_image), premultiplied, nearest-neighbour.
    if (source.starts_with("mem:")) {
        for (const auto& [key, im] : memory_images()) {
            if (key != source.substr(4) || im.width <= 0 || im.height <= 0) continue;
            std::vector<std::uint8_t> out(im.rgba);
            for (std::size_t i = 0; i + 3 < out.size(); i += 4) for (int c = 0; c < 3; ++c) out[i + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(out[i + static_cast<std::size_t>(c)] * out[i + 3] / 255);
            dimensions = {im.width, im.height};
            return make_texture({reinterpret_cast<const Rml::byte*>(out.data()), out.size()}, dimensions, rhi::Filter::Nearest);
        }
        return 0;
    }
    // "grad:<name>": small static images the color picker stretches over its boxes.
    if (source.starts_with("grad:")) {
        const std::vector<std::uint8_t> bits = rasterize_gradient(source.substr(5), dimensions);
        if (bits.empty()) return 0;
        return GenerateTexture({reinterpret_cast<const Rml::byte*>(bits.data()), bits.size()}, dimensions);
    }
    // "file:<max px>:<absolute path>": an image file (PNG/JPEG/QOI) shrunk to fit `max px`, for asset
    // thumbnails and the sprite preview. The same device texture path as everything else, so it works
    // on both backends. Callers put the file's mtime into the path's query so edits reload.
    // "pixel:" is the same with nearest-neighbour sampling, for zoomed pixel-art previews (the filter is
    // per texture on both backends, so no RHI change is needed).
    const bool pixel = source.starts_with("pixel:");
    if (source.starts_with("file:") || pixel) {
        const std::size_t skip = pixel ? 6 : 5;
        const std::size_t colon = source.find(':', skip);
        if (colon == std::string::npos) return 0;
        const int max_px = std::clamp(std::atoi(source.c_str() + skip), 8, 4096);
        std::string path = source.substr(colon + 1);
        if (const std::size_t q = path.rfind('?'); q != std::string::npos) path.resize(q);
        std::ifstream in(platform::path_from_utf8(path), std::ios::binary);
        if (!in) return 0;
        const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const assets::DecodeResult dr = assets::decode_image({reinterpret_cast<const std::byte*>(raw.data()), raw.size()}, 8192);
        if (!dr.image || dr.image->width <= 0 || dr.image->height <= 0) return 0;
        const assets::Image& im = *dr.image;
        const float k = std::min(1.0f, static_cast<float>(max_px) / static_cast<float>(std::max(im.width, im.height)));
        const int w = std::max(1, static_cast<int>(std::lround(static_cast<float>(im.width) * k)));
        const int h = std::max(1, static_cast<int>(std::lround(static_cast<float>(im.height) * k)));
        std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
        for (int y = 0; y < h; ++y) {
            const int y0 = y * im.height / h, y1 = std::max(y0 + 1, (y + 1) * im.height / h);
            for (int x = 0; x < w; ++x) {
                const int x0 = x * im.width / w, x1 = std::max(x0 + 1, (x + 1) * im.width / w);
                std::uint32_t acc[4] = {0, 0, 0, 0}, n = 0;
                for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) {
                    const std::uint8_t* px = &im.rgba[(static_cast<std::size_t>(yy) * static_cast<std::size_t>(im.width) + static_cast<std::size_t>(xx)) * 4];
                    for (int c = 0; c < 3; ++c) acc[c] += static_cast<std::uint32_t>(px[c]) * px[3]; // premultiply before averaging
                    acc[3] += px[3];
                    ++n;
                }
                std::uint8_t* o = &out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4];
                for (int c = 0; c < 3; ++c) o[c] = static_cast<std::uint8_t>(acc[c] / (255u * n));
                o[3] = static_cast<std::uint8_t>(acc[3] / n);
            }
        }
        dimensions = {w, h};
        return make_texture({reinterpret_cast<const Rml::byte*>(out.data()), out.size()}, dimensions, pixel ? rhi::Filter::Nearest : rhi::Filter::Linear);
    }
    // "icon:<name>:<size>": the editor's vector icons at the current pixel ratio.
    if (!source.starts_with("icon:")) return 0;
    const std::size_t colon = source.find(':', 5);
    const std::string name = source.substr(5, colon == std::string::npos ? std::string::npos : colon - 5);
    const float size = colon == std::string::npos ? 16.0f : static_cast<float>(std::atof(source.c_str() + colon + 1));
    const int px = std::max(8, static_cast<int>(std::lround(size * ratio_)));
    const std::vector<std::uint8_t> bits = rasterize_icon(name, px);
    if (bits.empty()) return 0;
    dimensions = {px, px};
    return GenerateTexture({reinterpret_cast<const Rml::byte*>(bits.data()), bits.size()}, dimensions);
}

Rml::TextureHandle RmlRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) {
    return make_texture(source, dimensions, rhi::Filter::Linear);
}

Rml::TextureHandle RmlRenderer::make_texture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions, rhi::Filter filter) {
    rhi::TextureDesc td;
    td.width = dimensions.x;
    td.height = dimensions.y;
    td.format = rhi::TextureFormat::RGBA8;
    td.filter = filter;
    td.initial_data = source.data();
    td.debug_name = "rml";
    const rhi::TextureHandle h = device_->create_texture(td);
    return h ? static_cast<Rml::TextureHandle>(h.id) : 0;
}

void RmlRenderer::ReleaseTexture(Rml::TextureHandle texture) { device_->destroy_texture({static_cast<std::uint32_t>(texture)}); }

void RmlRenderer::EnableScissorRegion(bool enable) { scissor_on_ = enable; }
void RmlRenderer::SetScissorRegion(Rml::Rectanglei region) { scissor_ = region; }

bool RmlRenderer::reserve(rhi::BufferHandle& buffer, std::size_t& capacity, std::size_t bytes, rhi::BufferKind kind) {
    if (bytes <= capacity && buffer) return true;
    std::size_t grown = std::max<std::size_t>(capacity * 2, 64 * 1024);
    while (grown < bytes) grown *= 2;
    device_->destroy_buffer(buffer);
    buffer = device_->create_buffer({kind, rhi::BufferUsage::Stream, grown, nullptr});
    capacity = buffer ? grown : 0;
    return static_cast<bool>(buffer);
}

void RmlRenderer::begin_frame() {
    frame_vertices_.clear();
    frame_indices_.clear();
    draws_.clear();
    scissor_on_ = false;
}

void RmlRenderer::end_frame() {
    last_draws_ = draws_.size();
    if (device_ == nullptr || draws_.empty()) return;
    const ivec2 fb = device_->framebuffer_size();
    const std::size_t vb = frame_vertices_.size() * sizeof(Rml::Vertex);
    const std::size_t ib = frame_indices_.size() * sizeof(std::uint32_t);
    if (!reserve(vertices_, vertex_capacity_, vb, rhi::BufferKind::Vertex) ||
        !reserve(indices_, index_capacity_, ib, rhi::BufferKind::Index)) {
        return;
    }
    device_->update_buffer(vertices_, 0, std::as_bytes(std::span(frame_vertices_)));
    device_->update_buffer(indices_, 0, std::as_bytes(std::span(frame_indices_)));
    const Globals g{{2.0f / static_cast<float>(fb.x()), -2.0f / static_cast<float>(fb.y()), -1.0f, 1.0f}};
    device_->update_buffer(globals_, 0, std::as_bytes(std::span(&g, 1)));
    device_->set_viewport({0, 0, fb.x(), fb.y()});
    device_->bind_pipeline(pipeline_);
    device_->bind_uniform_buffer(0, globals_);
    device_->bind_vertex_buffer(0, vertices_, 0);
    device_->bind_index_buffer(indices_, rhi::IndexFormat::UInt32);
    for (const Draw& d : draws_) {
        if (d.scissor) {
            const int x0 = std::clamp(d.rect.Left(), 0, fb.x());
            const int y0 = std::clamp(d.rect.Top(), 0, fb.y());
            const int x1 = std::clamp(d.rect.Right(), 0, fb.x());
            const int y1 = std::clamp(d.rect.Bottom(), 0, fb.y());
            if (x1 <= x0 || y1 <= y0) continue;
            device_->set_scissor({x0, y0, x1 - x0, y1 - y0});
        } else {
            device_->set_scissor({0, 0, fb.x(), fb.y()});
        }
        device_->bind_texture(0, d.texture != 0 ? rhi::TextureHandle{d.texture} : white_);
        device_->draw_indexed(d.count, 1, d.first_index);
    }
    device_->set_scissor({0, 0, fb.x(), fb.y()});
}

std::vector<std::uint8_t> rasterize_icon(std::string_view name, int px) {
    const std::vector<IconShape>* shapes = icon_shapes(name);
    if (shapes == nullptr) return {};
    struct Seg { float ax, ay, bx, by; };
    std::vector<Seg> segs;
    std::vector<std::vector<vec2>> fills;
    const float k = static_cast<float>(px) / 24.0f;
    const float radius = std::max(1.3f, static_cast<float>(px) * 0.09f) * 0.5f;
    for (const IconShape& shape : *shapes) {
        std::vector<vector::Polyline> lines;
        vector::flatten(shape.path, 0.1f, lines);
        for (const vector::Polyline& pl : lines) {
            if (shape.fill) {
                fills.push_back(pl.points);
                continue;
            }
            for (std::size_t i = 0; i + 1 < pl.points.size(); ++i) {
                segs.push_back({pl.points[i].x() * k, pl.points[i].y() * k, pl.points[i + 1].x() * k, pl.points[i + 1].y() * k});
            }
            if (pl.closed && pl.points.size() > 2) {
                segs.push_back({pl.points.back().x() * k, pl.points.back().y() * k, pl.points[0].x() * k, pl.points[0].y() * k});
            }
            if (pl.points.size() == 1) segs.push_back({pl.points[0].x() * k, pl.points[0].y() * k, pl.points[0].x() * k, pl.points[0].y() * k});
        }
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(px) * px * 4, 0);
    for (int y = 0; y < px; ++y) {
        for (int x = 0; x < px; ++x) {
            const float cx = static_cast<float>(x) + 0.5f;
            const float cy = static_cast<float>(y) + 0.5f;
            float cover = 0.0f;
            for (const Seg& s : segs) {
                const float dx = s.bx - s.ax, dy = s.by - s.ay;
                const float len2 = dx * dx + dy * dy;
                const float t = len2 > 0.0f ? std::clamp(((cx - s.ax) * dx + (cy - s.ay) * dy) / len2, 0.0f, 1.0f) : 0.0f;
                const float ex = s.ax + t * dx - cx, ey = s.ay + t * dy - cy;
                cover = std::max(cover, std::clamp(radius + 0.5f - std::sqrt(ex * ex + ey * ey), 0.0f, 1.0f));
            }
            for (const auto& poly : fills) { // even-odd, 3x3 supersample
                int hit = 0;
                for (int sy = 0; sy < 3; ++sy) for (int sx = 0; sx < 3; ++sx) {
                    const float px_ = (static_cast<float>(x) + (sx + 0.5f) / 3.0f) / k, py_ = (static_cast<float>(y) + (sy + 0.5f) / 3.0f) / k;
                    bool in = false;
                    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
                        if (((poly[i].y() > py_) != (poly[j].y() > py_)) &&
                            (px_ < (poly[j].x() - poly[i].x()) * (py_ - poly[i].y()) / (poly[j].y() - poly[i].y()) + poly[i].x())) in = !in;
                    }
                    hit += in ? 1 : 0;
                }
                cover = std::max(cover, static_cast<float>(hit) / 9.0f);
            }
            const auto a = static_cast<std::uint8_t>(std::lround(std::clamp(cover, 0.0f, 1.0f) * 255.0f));
            std::uint8_t* p = &out[(static_cast<std::size_t>(y) * px + x) * 4];
            p[0] = p[1] = p[2] = p[3] = a; // premultiplied white
        }
    }
    return out;
}

} // namespace cuervo::editor
