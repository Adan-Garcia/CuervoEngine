// rhi_parity: renders one scene per RHI feature through the frame-capture path,
// so the Vulkan and GL backends (and GL vs GLES 3.0) can be compared pixel by
// pixel (DESIGN.md 6, Phase 5 "Vulkan feature parity"; DESIGN.md 4.4 Clarified).
//
//   rhi_parity render --backend gl|vulkan|webgpu [--gl-es] [--tier compatible|full] --out <dir>
//   rhi_parity compare <a.ppm|dirA> <b.ppm|dirB> [--tolerance <0-255>] [--max-fraction <f>]
//   rhi_parity features --backend gl|vulkan|webgpu [--gl-es]
//
// `render` writes <dir>/<scene>.ppm for every scene below. With --tier full
// on a device that has base-vertex draws, it also checks in-process that
// draw_indexed_base_vertex matches the same mesh drawn with pre-offset
// indices. `compare` reports, per image, the largest channel difference and
// how many pixels differ by more than the tolerance, split into pixels on a
// color edge and interior pixels; it fails if more than `max-fraction`
// (default 0) of the pixels are interior differences. Edges may differ:
// Vulkan's y-flipped viewport mirrors the MSAA sample pattern and the
// rounding of exactly-half pixel/texel positions. `features` prints the
// device's capability report (rhi::DeviceInfo).
//
// GL runs headless with SDL_VIDEO_DRIVER=offscreen; Vulkan needs a display
// (README: NVIDIA lacks VK_EXT_headless_surface).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/core/log.hpp"
#include "cuervo/platform/window.hpp"
#include "cuervo/render/camera.hpp"
#include "cuervo/render/light_batcher.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/rhi/gl/gl_device.hpp"
#include "cuervo/rhi/rhi.hpp"
#include "cuervo/rhi/webgpu/webgpu_device.hpp"
#include "shaders/parity_color_frag.hpp"
#include "shaders/parity_color_vert.hpp"
#include "shaders/parity_instanced_vert.hpp"
#include "shaders/parity_textured_frag.hpp"
#include "shaders/parity_textured_vert.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

constexpr int target_size = 256;

std::string_view arg_value(int argc, char** argv, std::string_view flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == flag) return argv[i + 1];
    }
    return {};
}

bool has_flag(int argc, char** argv, std::string_view flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == flag) return true;
    }
    return false;
}

// --- images -----------------------------------------------------------------------

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
};

std::optional<Image> read_ppm(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    Image image;
    int max_value = 0;
    if (!(in >> magic >> image.width >> image.height >> max_value) || magic != "P6" || max_value != 255) {
        LOG_ERROR("{}: not a binary 8-bit PPM", path.string());
        return std::nullopt;
    }
    in.get(); // the single whitespace before the pixels
    image.rgb.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3);
    if (!in.read(reinterpret_cast<char*>(image.rgb.data()), static_cast<std::streamsize>(image.rgb.size()))) {
        LOG_ERROR("{}: truncated", path.string());
        return std::nullopt;
    }
    return image;
}

struct Diff {
    bool same_size = false;
    int max_channel = 0;
    std::size_t differing = 0; // pixels with a channel difference above the tolerance
    std::size_t interior = 0;  // ... of which not on an edge of either image
    std::size_t pixels = 0;
};

int pixel_distance(const Image& a, std::size_t p, const Image& b, std::size_t q) {
    int worst = 0;
    for (std::size_t c = 0; c < 3; ++c) worst = std::max(worst, std::abs(int(a.rgb[p * 3 + c]) - int(b.rgb[q * 3 + c])));
    return worst;
}

// Pixels within one pixel of a color edge. Edges are where the backends
// legitimately differ: Vulkan flips y with a negative viewport height, which
// mirrors the MSAA sample pattern and the rounding of positions that fall
// exactly between pixels or texels (vk/README.md). Shapes, colors, blending
// and every interior pixel must still match.
std::vector<bool> edge_mask(const Image& img) {
    const int w = img.width, h = img.height;
    std::vector<bool> edge(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), false);
    auto at = [w](int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x); };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            bool is_edge = false;
            for (int dy = -1; dy <= 1 && !is_edge; ++dy) {
                for (int dx = -1; dx <= 1 && !is_edge; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    is_edge = pixel_distance(img, at(x, y), img, at(nx, ny)) > 8;
                }
            }
            if (!is_edge) continue;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < w && ny < h) edge[at(nx, ny)] = true;
                }
            }
        }
    }
    return edge;
}

Diff diff_images(const Image& a, const Image& b, int tolerance) {
    Diff d;
    d.same_size = a.width == b.width && a.height == b.height;
    if (!d.same_size) return d;
    d.pixels = static_cast<std::size_t>(a.width) * static_cast<std::size_t>(a.height);
    const std::vector<bool> edge_a = edge_mask(a);
    const std::vector<bool> edge_b = edge_mask(b);
    for (std::size_t p = 0; p < d.pixels; ++p) {
        const int worst = pixel_distance(a, p, b, p);
        d.max_channel = std::max(d.max_channel, worst);
        if (worst > tolerance) {
            ++d.differing;
            if (!edge_a[p] && !edge_b[p]) ++d.interior;
        }
    }
    return d;
}

// --- scenes -------------------------------------------------------------------------

struct ColorVertex {
    float x, y;
    std::uint8_t r, g, b, a;
};

struct UvVertex {
    float x, y, u, v;
};

struct Instance {
    float x, y;
    std::uint8_t r, g, b, a;
    std::uint32_t palette;
};

struct Params {
    float offset_scale[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

ColorVertex cv(float x, float y, std::uint32_t rgba) {
    return {x, y, static_cast<std::uint8_t>(rgba >> 24), static_cast<std::uint8_t>(rgba >> 16),
            static_cast<std::uint8_t>(rgba >> 8), static_cast<std::uint8_t>(rgba)};
}

template<typename T>
std::span<const std::byte> bytes(const std::vector<T>& v) {
    return std::as_bytes(std::span<const T>(v));
}
template<typename T>
std::span<const std::byte> bytes(const T& v) {
    return std::as_bytes(std::span<const T>(&v, 1));
}

class Scenes {
public:
    explicit Scenes(rhi::Device& device) : d_(device) {}

    bool init() {
        static constexpr rhi::VertexAttribute color_attrs[] = {{0, rhi::VertexFormat::Float2, 0},
                                                               {1, rhi::VertexFormat::UByte4Norm, 8}};
        static constexpr rhi::VertexStream color_streams[] = {{sizeof(ColorVertex), rhi::VertexRate::PerVertex, color_attrs}};
        auto color_pipeline = [&](rhi::BlendMode blend, rhi::Topology topology, const char* name) {
            rhi::PipelineDesc desc;
            desc.vertex = shaders::parity_color_vert;
            desc.fragment = shaders::parity_color_frag;
            desc.streams = color_streams;
            desc.blend = blend;
            desc.topology = topology;
            desc.debug_name = name;
            return d_.create_pipeline(desc);
        };
        for (int b = 0; b < 4; ++b) {
            blend_[b] = color_pipeline(static_cast<rhi::BlendMode>(b), rhi::Topology::TriangleList, "parity_blend");
        }
        strip_ = color_pipeline(rhi::BlendMode::None, rhi::Topology::TriangleStrip, "parity_strip");
        points_ = color_pipeline(rhi::BlendMode::None, rhi::Topology::PointList, "parity_points");
        lines_ = color_pipeline(rhi::BlendMode::None, rhi::Topology::LineList, "parity_lines");
        line_strip_ = color_pipeline(rhi::BlendMode::None, rhi::Topology::LineStrip, "parity_line_strip");

        static constexpr rhi::VertexAttribute shape_attrs[] = {{0, rhi::VertexFormat::Float2, 0}};
        static constexpr rhi::VertexAttribute instance_attrs[] = {{2, rhi::VertexFormat::Float2, 0},
                                                                  {3, rhi::VertexFormat::UByte4Norm, 8},
                                                                  {4, rhi::VertexFormat::UInt1, 12}};
        static constexpr rhi::VertexStream instanced_streams[] = {
            {sizeof(ColorVertex), rhi::VertexRate::PerVertex, shape_attrs},
            {sizeof(Instance), rhi::VertexRate::PerInstance, instance_attrs}};
        rhi::PipelineDesc inst;
        inst.vertex = shaders::parity_instanced_vert;
        inst.fragment = shaders::parity_color_frag;
        inst.streams = instanced_streams;
        inst.blend = rhi::BlendMode::Alpha;
        inst.debug_name = "parity_instanced";
        instanced_ = d_.create_pipeline(inst);

        static constexpr rhi::VertexAttribute uv_attrs[] = {{0, rhi::VertexFormat::Float2, 0},
                                                            {1, rhi::VertexFormat::Float2, 8}};
        static constexpr rhi::VertexStream uv_streams[] = {{sizeof(UvVertex), rhi::VertexRate::PerVertex, uv_attrs}};
        rhi::PipelineDesc tex;
        tex.vertex = shaders::parity_textured_vert;
        tex.fragment = shaders::parity_textured_frag;
        tex.streams = uv_streams;
        tex.blend = rhi::BlendMode::Alpha;
        tex.debug_name = "parity_textured";
        textured_ = d_.create_pipeline(tex);

        // Uniforms: slot k of `params_` holds Params k, at the device's alignment.
        align_ = std::max<std::size_t>(d_.info().uniform_offset_alignment, sizeof(Params));
        params_ = d_.create_buffer({rhi::BufferKind::Uniform, rhi::BufferUsage::Dynamic, align_ * 16, nullptr});
        set_params(0, Params{});
        const float palette[16] = {1, 1, 1, 1, 1, 0.3f, 0.3f, 1, 0.3f, 1, 0.3f, 1, 0.3f, 0.5f, 1, 1};
        palette_ = d_.create_buffer({rhi::BufferKind::Uniform, rhi::BufferUsage::Static, sizeof(palette), palette});

        // A unit quad, as triangles and as a strip.
        const std::vector<ColorVertex> quad = {cv(0, 0, 0xffffffff), cv(1, 0, 0xffffffff), cv(1, 1, 0xffffffff),
                                               cv(0, 0, 0xffffffff), cv(1, 1, 0xffffffff), cv(0, 1, 0xffffffff)};
        quad_ = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static, bytes(quad));

        const std::vector<UvVertex> uv_quad = {{0, 0, 0, 0}, {1, 0, 1, 0}, {1, 1, 1, 1},
                                               {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 1, 0, 1}};
        uv_quad_ = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static, bytes(uv_quad));

        // Textures: a 4x4 RGBA8 checker (magnified: nearest vs linear) and an R8 ramp.
        std::vector<std::uint8_t> checker(4 * 4 * 4);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const bool on = (x + y) % 2 == 0;
                std::uint8_t* t = &checker[static_cast<std::size_t>((y * 4 + x) * 4)];
                t[0] = on ? 240 : 30;
                t[1] = static_cast<std::uint8_t>(x * 60);
                t[2] = on ? 40 : static_cast<std::uint8_t>(y * 70);
                t[3] = 255;
            }
        }
        for (int f = 0; f < 2; ++f) {
            checker_[f] = d_.create_texture({4, 4, rhi::TextureFormat::RGBA8,
                                             f == 0 ? rhi::Filter::Nearest : rhi::Filter::Linear, checker.data(),
                                             "parity_checker"});
        }
        std::vector<std::uint8_t> ramp(8 * 8);
        for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<std::uint8_t>(i * 4);
        mask_ = d_.create_texture({8, 8, rhi::TextureFormat::R8, rhi::Filter::Nearest, ramp.data(), "parity_mask"});
        blank_ = d_.create_texture({2, 2, rhi::TextureFormat::RGBA8, rhi::Filter::Nearest, nullptr, "parity_blank"});

        // 2D lighting (milestone 9.7): SDF shadows must match across backends.
        const bool lit = lights_.init(d_) && lit_shapes_.init(d_);

        return lit && blend_[0] && blend_[3] && strip_ && points_ && lines_ && line_strip_ && instanced_ && textured_ &&
               params_ && palette_ && quad_ && uv_quad_ && checker_[0] && checker_[1] && mask_ && blank_;
    }

    void shutdown() {
        lights_.shutdown();
        lit_shapes_.shutdown();
        for (rhi::PipelineHandle p : {blend_[0], blend_[1], blend_[2], blend_[3], strip_, points_, lines_, line_strip_,
                                      instanced_, textured_}) {
            d_.destroy_pipeline(p);
        }
        for (rhi::BufferHandle b : owned_buffers_) d_.destroy_buffer(b);
        d_.destroy_buffer(params_);
        d_.destroy_buffer(palette_);
        for (rhi::TextureHandle t : {checker_[0], checker_[1], mask_, blank_}) d_.destroy_texture(t);
    }

    // Every scene: name + draw function (state is reset by begin_frame).
    std::vector<std::pair<std::string, std::function<void()>>> list() {
        return {
            {"clear_scissor_viewport", [this] { clear_scissor_viewport(); }},
            {"triangles_and_strip", [this] { triangles_and_strip(); }},
            {"indexed_16_32", [this] { indexed(); }},
            {"instanced_first_instance", [this] { instanced(); }},
            {"blend_modes", [this] { blend_modes(); }},
            {"points_lines", [this] { points_lines(); }},
            {"uniform_offsets", [this] { uniform_offsets(); }},
            {"textures", [this] { textures(); }},
            {"buffer_update_order", [this] { buffer_updates(); }},
            {"stream_large", [this] { stream_large(); }},
            {"base_vertex", [this] { base_vertex(true); }},
            {"base_vertex_reference", [this] { base_vertex(false); }},
            {"lighting_shadows", [this] { lighting(false); }},
            {"lighting_ambient", [this] { lighting(true); }},
        };
    }

private:
    rhi::BufferHandle make_buffer(rhi::BufferKind kind, rhi::BufferUsage usage, std::span<const std::byte> data) {
        rhi::BufferHandle b = d_.create_buffer({kind, usage, data.size(), data.data()});
        owned_buffers_.push_back(b);
        return b;
    }

    void set_params(std::size_t slot, const Params& p) { d_.update_buffer(params_, slot * align_, bytes(p)); }

    // Draws with Params slot `slot` set to offset/scale/tint.
    void use_params(std::size_t slot, float x, float y, float sx, float sy, std::array<float, 4> tint = {1, 1, 1, 1}) {
        Params p;
        p.offset_scale[0] = x;
        p.offset_scale[1] = y;
        p.offset_scale[2] = sx;
        p.offset_scale[3] = sy;
        std::copy(tint.begin(), tint.end(), p.tint);
        set_params(slot, p);
        d_.bind_uniform_buffer(0, params_, slot * align_, sizeof(Params));
    }

    void colored_quad(std::size_t slot, rhi::PipelineHandle pipeline, float x, float y, float w, float h,
                      std::array<float, 4> color) {
        d_.bind_pipeline(pipeline);
        d_.bind_vertex_buffer(0, quad_);
        use_params(slot, x, y, w, h, color);
        d_.draw(6);
    }

    // A lit room (DESIGN.md 4.17, 9.7): a box, a circle and a rotated capsule
    // cast soft and hard shadows from a point light and a spot light. Additive
    // lights alone, or composited over an ambient darkness.
    void lighting(bool ambient) {
        const float s = static_cast<float>(target_size);
        const Affine2 to_ndc = render::virtual_to_ndc({s, s});
        lit_shapes_.clear();
        lit_shapes_.rounded_rect({s * 0.5f, s * 0.5f}, {s * 0.45f, s * 0.45f}, 12.0f, 0.0f, {0.45f, 0.42f, 0.38f, 1.0f});
        lit_shapes_.rounded_rect({96.0f, 120.0f}, {18.0f, 12.0f}, 3.0f, 0.0f, {0.7f, 0.2f, 0.2f, 1.0f});
        lit_shapes_.circle({170.0f, 90.0f}, 14.0f, {0.2f, 0.6f, 0.8f, 1.0f});
        lit_shapes_.flush(to_ndc, 1.0f);

        lights_.clear();
        lights_.add_occluder_box({96.0f, 120.0f}, {18.0f, 12.0f}, 0.0f, 3.0f);
        lights_.add_occluder_circle({170.0f, 90.0f}, 14.0f);
        lights_.add_occluder_capsule({150.0f, 180.0f}, 26.0f, 6.0f, 0.6f);
        render::LightDef point;
        point.position = {128.0f, 72.0f};
        point.radius = 170.0f;
        point.color = {1.0f, 0.85f, 0.6f, 1.0f};
        point.shadow_softness = 6.0f;
        lights_.add_light(point);
        render::LightDef spot;
        spot.position = {40.0f, 220.0f};
        spot.rotation = -0.7f;
        spot.radius = 200.0f;
        spot.spot_angle_rad = 1.1f;
        spot.color = {0.4f, 0.6f, 1.0f, 1.0f};
        spot.shadow_softness = 0.0f;
        lights_.add_light(spot);
        if (ambient) {
            render::AmbientLightDef amb;
            amb.enabled = true;
            amb.darkness = 0.85f;
            lights_.set_ambient(amb);
        }
        lights_.flush(to_ndc, {s * 0.5f, s * 0.5f}, {s * 0.5f, s * 0.5f});
    }

    void clear_scissor_viewport() {
        d_.set_scissor({24, 24, 100, 60});
        d_.clear({0.9f, 0.2f, 0.2f, 1.0f});
        d_.set_scissor({150, 110, 70, 120});
        d_.clear({0.2f, 0.8f, 0.3f, 1.0f});
        d_.set_scissor({0, 0, target_size, target_size});
        // A viewport in the bottom-right quarter: unit space maps into it,
        // so this full-target quad fills exactly that quarter (y-down on both).
        d_.set_viewport({128, 128, 128, 128});
        colored_quad(0, blend_[0], 0, 0, 128, 64, {0.3f, 0.4f, 1.0f, 1.0f});
        d_.set_viewport({0, 0, target_size, target_size});
        colored_quad(1, blend_[0], 10, 200, 40, 40, {1.0f, 1.0f, 0.2f, 1.0f});
    }

    void triangles_and_strip() {
        const std::vector<ColorVertex> tris = {
            cv(20, 20, 0xff0000ff), cv(120, 30, 0x00ff00ff), cv(40, 120, 0x0000ffff),
            cv(140, 20, 0xffff00ff), cv(236, 60, 0xff00ffff), cv(160, 110, 0x00ffffff),
        };
        const std::vector<ColorVertex> strip = {cv(20, 150, 0xffffffff), cv(20, 236, 0xff8000ff),
                                                cv(110, 140, 0x8000ffff), cv(130, 230, 0x00ff80ff),
                                                cv(236, 150, 0x404040ff), cv(220, 236, 0xffffffff)};
        rhi::BufferHandle tb = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static, bytes(tris));
        rhi::BufferHandle sb = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static, bytes(strip));
        d_.bind_pipeline(blend_[0]);
        use_params(0, 0, 0, 1, 1);
        d_.bind_vertex_buffer(0, tb);
        d_.draw(3);
        d_.draw(3, 1, 3); // first_vertex
        d_.bind_pipeline(strip_);
        d_.bind_vertex_buffer(0, sb);
        d_.draw(6);
    }

    void indexed() {
        std::vector<ColorVertex> grid;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                grid.push_back(cv(static_cast<float>(20 + x * 70), static_cast<float>(20 + y * 70),
                                  (static_cast<std::uint32_t>(x * 80) << 24) | (static_cast<std::uint32_t>(y * 80) << 16) |
                                      0x8000ffu));
            }
        }
        std::vector<std::uint16_t> i16;
        std::vector<std::uint32_t> i32;
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 3; ++x) {
                const auto v = static_cast<std::uint16_t>(y * 4 + x);
                const std::uint16_t cell[] = {v, static_cast<std::uint16_t>(v + 1), static_cast<std::uint16_t>(v + 5),
                                              v, static_cast<std::uint16_t>(v + 5), static_cast<std::uint16_t>(v + 4)};
                for (std::uint16_t i : cell) ((x + y) % 2 == 0 ? i16.push_back(i) : i32.push_back(i));
            }
        }
        rhi::BufferHandle vb = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static, bytes(grid));
        rhi::BufferHandle ib16 = make_buffer(rhi::BufferKind::Index, rhi::BufferUsage::Static, bytes(i16));
        rhi::BufferHandle ib32 = make_buffer(rhi::BufferKind::Index, rhi::BufferUsage::Static, bytes(i32));
        d_.bind_pipeline(blend_[0]);
        use_params(0, 0, 0, 1, 1);
        d_.bind_vertex_buffer(0, vb);
        d_.bind_index_buffer(ib16, rhi::IndexFormat::UInt16);
        d_.draw_indexed(static_cast<std::uint32_t>(i16.size()) - 6, 1, 6); // skips the first cell
        d_.bind_index_buffer(ib32, rhi::IndexFormat::UInt32);
        d_.draw_indexed(static_cast<std::uint32_t>(i32.size()));
    }

    void instanced() {
        std::vector<Instance> instances;
        for (std::uint32_t i = 0; i < 16; ++i) {
            instances.push_back({static_cast<float>(16 + (i % 4) * 60), static_cast<float>(16 + (i / 4) * 60),
                                 static_cast<std::uint8_t>(255 - i * 10), 255, static_cast<std::uint8_t>(i * 15), 230,
                                 i});
        }
        rhi::BufferHandle ib = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static, bytes(instances));
        const std::vector<std::uint16_t> indices = {0, 1, 2, 3, 4, 5};
        rhi::BufferHandle idx = make_buffer(rhi::BufferKind::Index, rhi::BufferUsage::Static, bytes(indices));
        d_.bind_pipeline(instanced_);
        use_params(0, 0, 0, 44, 44);
        d_.bind_uniform_buffer(1, palette_);
        d_.bind_vertex_buffer(0, quad_);
        d_.bind_vertex_buffer(1, ib);
        d_.draw(6, 5, 0, 2);               // instances 2..6
        d_.bind_index_buffer(idx, rhi::IndexFormat::UInt16);
        d_.draw_indexed(6, 4, 0, 9);       // instances 9..12
        d_.bind_vertex_buffer(1, ib, sizeof(Instance) * 15); // buffer offset + first_instance 0
        d_.draw(6, 1);
    }

    void blend_modes() {
        // Background: black/white halves, then each mode over both.
        colored_quad(0, blend_[0], 0, 0, 256, 128, {0.0f, 0.0f, 0.0f, 1.0f});
        colored_quad(1, blend_[0], 0, 128, 256, 128, {1.0f, 1.0f, 1.0f, 1.0f});
        const std::array<float, 4> color = {0.9f, 0.4f, 0.1f, 0.5f};
        for (std::size_t b = 0; b < 4; ++b) {
            colored_quad(2 + b, blend_[b], 12.0f + static_cast<float>(b) * 60.0f, 40, 48, 176, color);
        }
    }

    void points_lines() {
        std::vector<ColorVertex> pts;
        for (int i = 0; i < 10; ++i) pts.push_back(cv(20.0f + static_cast<float>(i) * 22.0f, 30, 0xffe040ff));
        std::vector<ColorVertex> lines = {cv(20.5f, 60.5f, 0x40ff40ff), cv(236.5f, 60.5f, 0x40ff40ff),
                                          cv(20.5f, 80.5f, 0xff4040ff), cv(20.5f, 230.5f, 0xff4040ff)};
        std::vector<ColorVertex> strip = {cv(60.5f, 100.5f, 0x80c0ffff), cv(200.5f, 100.5f, 0x80c0ffff),
                                          cv(200.5f, 220.5f, 0x80c0ffff), cv(60.5f, 220.5f, 0x80c0ffff),
                                          cv(60.5f, 100.5f, 0x80c0ffff)};
        rhi::BufferHandle pb = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static, bytes(pts));
        rhi::BufferHandle lb = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static, bytes(lines));
        rhi::BufferHandle sb = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static, bytes(strip));
        use_params(0, 0, 0, 1, 1);
        d_.bind_pipeline(points_);
        d_.bind_vertex_buffer(0, pb);
        d_.draw(10);
        d_.bind_pipeline(lines_);
        d_.bind_vertex_buffer(0, lb);
        d_.draw(4);
        d_.bind_pipeline(line_strip_);
        d_.bind_vertex_buffer(0, sb);
        d_.draw(5);
    }

    void uniform_offsets() {
        // Write all slots first, then draw from each: offsets must select them.
        for (std::size_t k = 0; k < 8; ++k) {
            Params p;
            p.offset_scale[0] = 10.0f + static_cast<float>(k % 4) * 60.0f;
            p.offset_scale[1] = 20.0f + static_cast<float>(k / 4) * 110.0f;
            p.offset_scale[2] = 50.0f;
            p.offset_scale[3] = 90.0f - static_cast<float>(k) * 5.0f;
            p.tint[0] = static_cast<float>(k) / 7.0f;
            p.tint[1] = 1.0f - static_cast<float>(k) / 7.0f;
            p.tint[2] = 0.5f;
            set_params(k, p);
        }
        d_.bind_pipeline(blend_[0]);
        d_.bind_vertex_buffer(0, quad_);
        for (std::size_t k = 0; k < 8; ++k) {
            d_.bind_uniform_buffer(0, params_, k * align_, sizeof(Params));
            d_.draw(6);
        }
    }

    void textures() {
        // Update a region before drawing (rhi.hpp: ordering is only defined that way).
        const std::uint8_t patch[] = {255, 255, 255, 255, 0, 0, 0, 255};
        d_.update_texture(checker_[0], {1, 1, 2, 1}, std::as_bytes(std::span(patch)));
        d_.update_texture(checker_[1], {1, 1, 2, 1}, std::as_bytes(std::span(patch)));
        d_.bind_pipeline(textured_);
        d_.bind_vertex_buffer(0, uv_quad_);
        d_.bind_texture(1, mask_);
        for (int f = 0; f < 2; ++f) {
            d_.bind_texture(0, checker_[f]);
            use_params(static_cast<std::size_t>(f), 8.0f + static_cast<float>(f) * 124.0f, 8, 116, 116, {1, 1, 1, 0});
            d_.draw(6);
        }
        d_.bind_texture(0, checker_[1]);
        // 8 texels over 112 px: texel boundaries on pixel edges, never on pixel
        // centers (where the y-flipped backends would round differently).
        use_params(2, 8, 132, 112, 112, {1.0f, 1.0f, 1.0f, 1.0f}); // the R8 mask: (r, 0, 0, 1)
        d_.draw(6);
        d_.bind_texture(0, blank_); // created without data: transparent black on both
        use_params(3, 132, 132, 116, 116, {1.0f, 1.0f, 1.0f, 0.5f});
        d_.draw(6);
    }

    void buffer_updates() {
        // GL semantics on both backends: a draw sees the buffer as of when it
        // was recorded, even when the buffer is updated later in the frame.
        if (!dynamic_) {
            dynamic_ = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Dynamic,
                                   bytes(std::vector<ColorVertex>(6, cv(0, 0, 0))));
            stream_ = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Stream,
                                  bytes(std::vector<ColorVertex>(12, cv(0, 0, 0))));
        }
        auto tri = [](float x, float y, std::uint32_t c) {
            return std::vector<ColorVertex>{cv(x, y, c), cv(x + 60, y, c), cv(x, y + 60, c)};
        };
        d_.bind_pipeline(blend_[0]);
        use_params(0, 0, 0, 1, 1);
        d_.update_buffer(dynamic_, 0, bytes(tri(20, 20, 0xff3030ff)));
        d_.update_buffer(dynamic_, sizeof(ColorVertex) * 3, bytes(tri(100, 20, 0x30ff30ff)));
        d_.bind_vertex_buffer(0, dynamic_);
        d_.draw(6);
        d_.update_buffer(dynamic_, 0, bytes(tri(20, 100, 0x3030ffff))); // second half keeps its data
        d_.draw(6);
        d_.update_buffer(stream_, 0, bytes(tri(180, 20, 0xffff30ff)));
        d_.bind_vertex_buffer(0, stream_);
        d_.draw(3);
        d_.update_buffer(stream_, sizeof(ColorVertex) * 3, bytes(tri(180, 100, 0xff30ffff)));
        d_.draw(3, 1, 3);
        d_.update_buffer(stream_, 0, bytes(tri(100, 180, 0x30ffffff)));
        d_.draw(3);
    }

    // A stream buffer past the GL backend's upload slice (1.2 MB in one
    // update) plus a second update beside it in the same frame: the batcher
    // pattern at a size that exercises sliced uploads and, on GL, the
    // rotating buffer versions (checked across frames by `frames`).
    void stream_large() {
        constexpr int cells = 128;
        constexpr std::size_t base_vertices = static_cast<std::size_t>(cells) * cells * 6;
        constexpr std::size_t overlay_vertices = static_cast<std::size_t>(cells) * cells / 4 * 3;
        if (!big_stream_) {
            big_stream_ = d_.create_buffer({rhi::BufferKind::Vertex, rhi::BufferUsage::Stream,
                                            (base_vertices + overlay_vertices) * sizeof(ColorVertex), nullptr});
            owned_buffers_.push_back(big_stream_);
        }
        std::vector<ColorVertex> base;
        std::vector<ColorVertex> overlay;
        base.reserve(base_vertices);
        overlay.reserve(overlay_vertices);
        for (int cy = 0; cy < cells; ++cy) {
            for (int cx = 0; cx < cells; ++cx) {
                const float x = static_cast<float>(cx * 2);
                const float y = static_cast<float>(cy * 2);
                const std::uint32_t h = static_cast<std::uint32_t>(cx * 73 + cy * 151 + (cx ^ cy) * 31);
                const std::uint32_t rgba = ((h * 2654435761u) & 0xffffff00u) | 0xffu;
                for (const ColorVertex& v : {cv(x, y, rgba), cv(x + 2, y, rgba), cv(x, y + 2, rgba), cv(x + 2, y, rgba),
                                             cv(x + 2, y + 2, rgba), cv(x, y + 2, rgba)}) {
                    base.push_back(v);
                }
                if (cx % 2 == 0 && cy % 2 == 0) {
                    for (const ColorVertex& v : {cv(x, y, 0xffffffff), cv(x + 1, y, 0xffffffff), cv(x, y + 1, 0xffffffff)}) {
                        overlay.push_back(v);
                    }
                }
            }
        }
        d_.bind_pipeline(blend_[0]);
        use_params(0, 0, 0, 1, 1);
        d_.update_buffer(big_stream_, 0, bytes(base));
        d_.bind_vertex_buffer(0, big_stream_);
        d_.draw(static_cast<std::uint32_t>(base_vertices));
        d_.update_buffer(big_stream_, base.size() * sizeof(ColorVertex), bytes(overlay));
        d_.draw(static_cast<std::uint32_t>(overlay.size()), 1, static_cast<std::uint32_t>(base_vertices));
    }

    // Two quads packed into one vertex buffer; the second is drawn with
    // 0-based indices plus base_vertex = 4 (or, for the reference, with
    // indices pre-offset by 4).
    void base_vertex(bool use_base_vertex) {
        if (!bv_vertices_) {
            const std::vector<ColorVertex> v = {cv(20, 20, 0xff8040ff),  cv(110, 20, 0xff8040ff),
                                                cv(110, 110, 0xff8040ff), cv(20, 110, 0xff8040ff),
                                                cv(130, 130, 0x40c0ffff), cv(236, 130, 0x40c0ffff),
                                                cv(236, 236, 0x40c0ffff), cv(130, 236, 0x40c0ffff)};
            const std::vector<std::uint16_t> idx = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
            bv_vertices_ = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static, bytes(v));
            bv_indices_ = make_buffer(rhi::BufferKind::Index, rhi::BufferUsage::Static, bytes(idx));
        }
        d_.bind_pipeline(blend_[0]);
        use_params(0, 0, 0, 1, 1);
        d_.bind_vertex_buffer(0, bv_vertices_);
        d_.bind_index_buffer(bv_indices_, rhi::IndexFormat::UInt16);
        d_.draw_indexed(6); // first quad: plain indexed draw
        if (use_base_vertex) {
            d_.draw_indexed_base_vertex(6, 1, 0, 4);
        } else if (d_.info().supports(rhi::Feature::BaseVertexDraws)) {
            d_.draw_indexed(6, 1, 6);
        }
    }

    rhi::Device& d_;
    render::LightBatcher lights_;
    render::SdfBatcher lit_shapes_;
    std::array<rhi::PipelineHandle, 4> blend_{};
    rhi::PipelineHandle strip_, points_, lines_, line_strip_, instanced_, textured_;
    rhi::BufferHandle params_, palette_, quad_, uv_quad_, dynamic_, stream_, big_stream_, bv_vertices_, bv_indices_;
    std::array<rhi::TextureHandle, 2> checker_{};
    rhi::TextureHandle mask_, blank_;
    std::vector<rhi::BufferHandle> owned_buffers_;
    std::size_t align_ = 256;
};

struct DeviceArgs {
    rhi::Backend backend = rhi::Backend::OpenGL;
    bool gl_es = false;
    rhi::Tier tier = rhi::Tier::Compatible;
};

bool parse_device_args(int argc, char** argv, DeviceArgs& out) {
    if (!rhi::parse_backend(arg_value(argc, argv, "--backend"), out.backend)) {
        LOG_ERROR("--backend gl|vulkan|webgpu is required");
        return false;
    }
    out.gl_es = has_flag(argc, argv, "--gl-es");
    if (const std::string_view tier = arg_value(argc, argv, "--tier"); !tier.empty() && !rhi::parse_tier(tier, out.tier)) {
        LOG_ERROR("--tier must be compatible or full");
        return false;
    }
    return true;
}

std::unique_ptr<rhi::Device> open(Window& window, const DeviceArgs& args) {
    if (!rhi::backend_compiled_in(args.backend)) {
        LOG_ERROR("{} is not compiled into this build", rhi::backend_name(args.backend));
        return nullptr;
    }
    if (!window.create({.title = "rhi_parity",
                        .width = target_size,
                        .height = target_size,
                        .graphics = rhi::graphics_api(args.backend),
                        .resizable = false,
                        .gl_es = args.gl_es})) {
        return nullptr;
    }
    rhi::DeviceOptions options;
    options.vsync = false;
    options.tier = args.tier;
    return rhi::create_device(args.backend, window, options);
}

int features(int argc, char** argv) {
    DeviceArgs args;
    args.tier = rhi::Tier::Full;
    if (!parse_device_args(argc, argv, args)) return 2;
    Window window;
    auto device = open(window, args);
    if (!device) return 1;
    const rhi::DeviceInfo& info = device->info();
    std::printf("backend   %s%s\napi       %s\nrenderer  %s\nmax tier  %s\n",
                std::string(rhi::backend_name(info.backend)).c_str(), info.gles ? " (GLES)" : "",
                std::string(info.api_version).c_str(), std::string(info.renderer).c_str(),
                std::string(rhi::tier_name(info.max_tier)).c_str());
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(rhi::Feature::Count); ++i) {
        const auto f = static_cast<rhi::Feature>(i);
        std::printf("  %-26s %-10s %s%s\n", std::string(rhi::feature_name(f)).c_str(),
                    std::string(rhi::tier_name(rhi::feature_tier(f))).c_str(),
                    info.hardware_features.has(f) ? "yes" : "no", rhi::feature_required(f) ? "" : " (optional)");
    }
    const rhi::Limits& l = info.limits;
    std::printf("limits    texture %u, msaa %u, uniform block %u, attributes %u, uniform alignment %u\n",
                l.max_texture_size, l.max_msaa_samples, l.max_uniform_block_size, l.max_vertex_attributes,
                l.uniform_offset_alignment);
    for (rhi::Tier tier : {rhi::Tier::Compatible, rhi::Tier::Full}) {
        const rhi::TierReport r = rhi::check_tier(info, tier);
        std::printf("tier %-10s %s\n", std::string(rhi::tier_name(tier)).c_str(), r.ok() ? "ok" : "NOT available");
    }
    device.reset();
    return 0;
}

int render_scenes(int argc, char** argv) {
    DeviceArgs args;
    if (!parse_device_args(argc, argv, args)) return 2;
    const fs::path out(arg_value(argc, argv, "--out"));
    if (out.empty()) {
        LOG_ERROR("--out <dir> is required");
        return 2;
    }
    std::error_code ec;
    fs::create_directories(out, ec);
    Window window;
    auto device = open(window, args);
    if (!device) return 1;
    if (window.pixel_size() != ivec2{target_size, target_size}) {
        LOG_WARN("Framebuffer is {}x{}, not {}x{} (display scaling?): images won't compare across backends",
                 window.pixel_size().x(), window.pixel_size().y(), target_size, target_size);
    }
    Scenes scenes(*device);
    if (!scenes.init()) {
        LOG_ERROR("Could not create the parity resources");
        return 1;
    }
    int failures = 0;
    for (auto& [name, draw] : scenes.list()) {
        const fs::path file = out / (name + ".ppm");
        fs::remove(file, ec);
        // A few attempts: a Vulkan swapchain may need a frame or two to settle.
        for (int attempt = 0; attempt < 4 && !fs::exists(file); ++attempt) {
            DeviceState devices;
            window.poll_events(devices);
            device->begin_frame(window.pixel_size());
            device->clear({0.08f, 0.09f, 0.12f, 1.0f});
            draw();
            device->capture_frame(file.string());
            device->end_frame();
        }
        if (!fs::exists(file)) {
            LOG_ERROR("Scene '{}' was not captured", name);
            ++failures;
        }
    }
    // Base vertex: on a device that may use it, it must equal the reference.
    if (device->info().supports(rhi::Feature::BaseVertexDraws)) {
        const auto a = read_ppm(out / "base_vertex.ppm");
        const auto b = read_ppm(out / "base_vertex_reference.ppm");
        const Diff d = a && b ? diff_images(*a, *b, 0) : Diff{};
        if (!d.same_size || d.differing != 0) {
            LOG_ERROR("draw_indexed_base_vertex differs from the reference ({} pixels)", d.differing);
            ++failures;
        } else {
            LOG_INFO("draw_indexed_base_vertex matches its reference");
        }
    }
    scenes.shutdown();
    device.reset();
    window.destroy();
    LOG_INFO("Rendered {} scenes to '{}' ({}{}, {} tier)", scenes.list().size(), out.string(),
             rhi::backend_name(args.backend), args.gl_es ? " ES" : "", rhi::tier_name(args.tier));
    return failures == 0 ? 0 : 1;
}

int compare(int argc, char** argv) {
    if (argc < 4) return 2;
    const fs::path a(argv[2]);
    const fs::path b(argv[3]);
    const std::string_view tol = arg_value(argc, argv, "--tolerance");
    const std::string_view frac = arg_value(argc, argv, "--max-fraction");
    const int tolerance = tol.empty() ? 2 : std::atoi(std::string(tol).c_str());
    const double max_fraction = frac.empty() ? 0.0 : std::atof(std::string(frac).c_str());
    std::vector<std::pair<fs::path, fs::path>> pairs;
    std::error_code ec;
    if (fs::is_directory(a, ec)) {
        std::vector<fs::path> names;
        for (const auto& e : fs::directory_iterator(a, ec)) {
            if (e.path().extension() == ".ppm") names.push_back(e.path().filename());
        }
        std::sort(names.begin(), names.end());
        for (const fs::path& n : names) pairs.emplace_back(a / n, b / n);
    } else {
        pairs.emplace_back(a, b);
    }
    if (pairs.empty()) {
        LOG_ERROR("No .ppm files in '{}'", a.string());
        return 1;
    }
    int failures = 0;
    for (const auto& [pa, pb] : pairs) {
        const auto ia = read_ppm(pa);
        const auto ib = read_ppm(pb);
        if (!ia || !ib) {
            ++failures;
            continue;
        }
        const Diff d = diff_images(*ia, *ib, tolerance);
        if (!d.same_size) {
            std::printf("FAIL %-28s size %dx%d vs %dx%d\n", pa.filename().string().c_str(), ia->width, ia->height,
                        ib->width, ib->height);
            ++failures;
            continue;
        }
        const double fraction = static_cast<double>(d.interior) / static_cast<double>(std::max<std::size_t>(d.pixels, 1));
        const bool ok = fraction <= max_fraction;
        std::printf("%s %-28s max diff %3d | %6zu px > %d: %6zu on edges, %6zu interior (%.4f%%)\n",
                    ok ? "ok  " : "FAIL", pa.filename().string().c_str(), d.max_channel, d.differing, tolerance,
                    d.differing - d.interior, d.interior, fraction * 100.0);
        if (!ok) ++failures;
    }
    return failures == 0 ? 0 : 1;
}

// Renders the large-stream scene for --count frames on one device, as a game
// does, and checks that the first and last frames are identical (the rotating
// stream versions must not leak stale data) and, on GL and WebGPU, that the
// device never let more than the bound queue up.
int frames(int argc, char** argv) {
    DeviceArgs args;
    if (!parse_device_args(argc, argv, args)) return 2;
    const fs::path out(arg_value(argc, argv, "--out"));
    if (out.empty()) {
        LOG_ERROR("--out <dir> is required");
        return 2;
    }
    int count = 24;
    if (const std::string_view value = arg_value(argc, argv, "--count"); !value.empty()) {
        count = std::max(2, std::atoi(std::string(value).c_str()));
    }
    std::error_code ec;
    fs::create_directories(out, ec);
    Window window;
    auto device = open(window, args);
    if (!device) return 1;
    Scenes scenes(*device);
    if (!scenes.init()) return 1;
    std::function<void()> draw;
    for (auto& [name, fn] : scenes.list()) {
        if (name == "stream_large") draw = fn;
    }
    const fs::path first = out / "frames_first.ppm";
    const fs::path last = out / "frames_last.ppm";
    fs::remove(first, ec);
    fs::remove(last, ec);
    for (int frame = 1; frame <= count + 6; ++frame) {
        DeviceState devices;
        window.poll_events(devices);
        device->begin_frame(window.pixel_size());
        device->clear({0.08f, 0.09f, 0.12f, 1.0f});
        draw();
        // A Vulkan swapchain may need a frame or two to settle: retry the captures.
        if (frame >= 2 && !fs::exists(first) && frame < count) device->capture_frame(first.string());
        if (frame >= count && !fs::exists(last)) device->capture_frame(last.string());
        device->end_frame();
    }
    int failures = 0;
    const auto a = read_ppm(first);
    const auto b = read_ppm(last);
    const Diff d = a && b ? diff_images(*a, *b, 0) : Diff{};
    if (!a || !b || !d.same_size || d.differing != 0) {
        LOG_ERROR("An early frame and frame {} differ ({} pixels): stream versions leaked data", count, d.differing);
        ++failures;
    }
    if (args.backend == rhi::Backend::OpenGL) {
        const std::size_t peak = gl::peak_frames_in_flight(*device);
        LOG_INFO("GL frames in flight: peak {} (bound {})", peak, gl::frame_limit());
        if (peak > gl::frame_limit()) {
            LOG_ERROR("The GL device let {} frames queue (bound {})", peak, gl::frame_limit());
            ++failures;
        }
    }
    if (args.backend == rhi::Backend::WebGPU) {
        const std::size_t peak = webgpu::peak_frames_in_flight(*device);
        LOG_INFO("WebGPU frames in flight: peak {} (bound {})", peak, webgpu::frame_limit());
        if (peak == 0 || peak > webgpu::frame_limit()) {
            LOG_ERROR("The WebGPU device let {} frames queue (bound {})", peak, webgpu::frame_limit());
            ++failures;
        }
    }
    scenes.shutdown();
    device.reset();
    window.destroy();
    return failures == 0 ? 0 : 1;
}

int usage() {
    std::fputs("usage:\n"
               "  rhi_parity render --backend gl|vulkan|webgpu [--gl-es] [--tier compatible|full] --out <dir>\n"
               "  rhi_parity frames --backend gl|vulkan|webgpu [--gl-es] [--count <n>] --out <dir>\n"
               "  rhi_parity compare <a.ppm|dirA> <b.ppm|dirB> [--tolerance <0-255>] [--max-fraction <f>]\n"
               "  rhi_parity features --backend gl|vulkan|webgpu [--gl-es]\n",
               stderr);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string_view cmd = argv[1];
    if (cmd == "render") return render_scenes(argc, argv);
    if (cmd == "frames") return frames(argc, argv);
    if (cmd == "compare") return argc >= 4 ? compare(argc, argv) : usage();
    if (cmd == "features") return features(argc, argv);
    return usage();
}
