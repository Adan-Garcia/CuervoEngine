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

#include <SDL3/SDL.h>

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
#include "cuervo/assets/atlas.hpp"
#include "cuervo/assets/image.hpp"
#include "cuervo/assets/svg_raster.hpp"
#include "cuervo/platform/window.hpp"
#include "cuervo/render/camera.hpp"
#include "cuervo/render/light_batcher.hpp"
#include "cuervo/render/mesh_batcher.hpp"
#include "cuervo/vector/mesh.hpp"
#include "cuervo/vector/svg.hpp"
#include "cuervo/render/font_import.hpp"
#include "cuervo/render/post_fx.hpp"
#include "cuervo/vector/imported_vector.hpp"
#include "cuervo/render/sdf_batcher.hpp"
#include "cuervo/render/sprite_batcher.hpp"
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
        for (int b = 0; b < 5; ++b) {
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
        tex.blend = rhi::BlendMode::Additive;
        tex.debug_name = "parity_textured_additive";
        textured_additive_ = d_.create_pipeline(tex);

        // Uniforms: slot k of `params_` holds Params k, at the device's alignment.
        align_ = std::max<std::size_t>(d_.info().uniform_offset_alignment, sizeof(Params));
        params_ = d_.create_buffer({rhi::BufferKind::Uniform, rhi::BufferUsage::Dynamic, align_ * 16, nullptr});
        set_params(0, Params{});
        const float palette[16] = {1, 1, 1, 1, 1, 0.3f, 0.3f, 1, 0.3f, 1, 0.3f, 1, 0.3f, 0.5f, 1, 1};
        palette_ = d_.create_buffer({rhi::BufferKind::Uniform, rhi::BufferUsage::Static, sizeof(palette), palette});

        // A warm grading cube for the post scenes: blue pulled down, red lifted
        // (created here: a texture destroyed in the frame it was created is never uploaded).
        std::vector<std::uint8_t> lut_bytes = render::make_identity_lut(16);
        for (std::size_t i = 0; i < lut_bytes.size(); i += 4) {
            lut_bytes[i] = static_cast<std::uint8_t>(std::min(255, lut_bytes[i] + 24));
            lut_bytes[i + 2] = static_cast<std::uint8_t>(lut_bytes[i + 2] * 3 / 5);
        }
        lut_ = d_.create_texture({.width = 256, .height = 16, .format = rhi::TextureFormat::RGBA8,
                                  .filter = rhi::Filter::Linear, .initial_data = lut_bytes.data(), .debug_name = "parity_lut"});

        // A unit quad, as triangles and as a strip.
        const std::vector<ColorVertex> quad = {cv(0, 0, 0xffffffff), cv(1, 0, 0xffffffff), cv(1, 1, 0xffffffff),
                                               cv(0, 0, 0xffffffff), cv(1, 1, 0xffffffff), cv(0, 1, 0xffffffff)};
        quad_ = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static, bytes(quad));

        const std::vector<UvVertex> uv_quad = {{0, 0, 0, 0}, {1, 0, 1, 0}, {1, 1, 1, 1},
                                               {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 1, 0, 1}};
        uv_quad_ = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static, bytes(uv_quad));
        // The same quad for sampling a render target: GL stores target rows bottom-up.
        const std::vector<UvVertex> target_quad = {{0, 0, 0, 1}, {1, 0, 1, 1}, {1, 1, 1, 0},
                                                   {0, 0, 0, 1}, {1, 1, 1, 0}, {0, 1, 0, 0}};
        const std::vector<UvVertex>& sampled = d_.info().render_target_flip_v ? target_quad : uv_quad;
        target_quad_ = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static, bytes(sampled));

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

        // Mip chains (11.2a): 64x64, one flat color per level so that a minified draw
        // shows which level the sampler picked, on every backend.
        {
            constexpr std::uint8_t level_colors[7][3] = {{230, 40, 40},  {40, 230, 40},  {40, 40, 230}, {230, 230, 40},
                                                         {40, 230, 230}, {230, 40, 230}, {230, 230, 230}};
            std::vector<std::vector<std::byte>> levels(7);
            std::vector<std::span<const std::byte>> spans;
            for (std::uint32_t l = 0; l < 7; ++l) {
                const std::size_t n = std::size_t{64u >> l} * std::size_t{64u >> l};
                levels[l].resize(n * 4);
                for (std::size_t i = 0; i < n; ++i) {
                    for (std::size_t c = 0; c < 3; ++c) levels[l][i * 4 + c] = std::byte{level_colors[l][c]};
                    levels[l][i * 4 + 3] = std::byte{255};
                }
                spans.emplace_back(levels[l]);
            }
            for (int f = 0; f < 2; ++f) {
                rhi::TextureDesc td{64, 64, rhi::TextureFormat::RGBA8,
                                    f == 0 ? rhi::Filter::Nearest : rhi::Filter::Linear, nullptr, "parity_mips"};
                td.mip_levels = 0; // the full chain: 7 levels
                td.initial_levels = spans;
                mips_[f] = d_.create_texture(td);
            }
            // sRGB: four gray texels decoded to linear before the (nearest) fetch.
            const std::uint8_t grays[4 * 4] = {32, 32, 32, 255, 96, 96, 96, 255, 160, 160, 160, 255, 224, 224, 224, 255};
            rhi::TextureDesc sd{4, 1, rhi::TextureFormat::RGBA8, rhi::Filter::Nearest, grays, "parity_srgb"};
            sd.srgb = true;
            srgb_ = d_.create_texture(sd);
        }

        // Sprites (11.2a): procedural sprites packed by the slice 1 atlas packer, uploaded
        // with their mip chain, drawn by the sprite batcher.
        const bool sprites_ok = init_sprites();

        // 2D lighting (milestone 9.7): SDF shadows must match across backends.
        const bool lit = lights_.init(d_) && lit_shapes_.init(d_) && post_.init(d_) && init_mesh_gradient();

        return lit && sprites_ok && blend_[0] && blend_[4] && textured_additive_ && target_quad_ && strip_ && points_ && lines_ && line_strip_ && instanced_ && textured_ &&
               params_ && palette_ && quad_ && uv_quad_ && checker_[0] && checker_[1] && mask_ && blank_ && mips_[0] && mips_[1] && srgb_ && lut_;
    }

    void shutdown() {
        meshes_.shutdown();
        post_.shutdown();
        sprite_batcher_.shutdown();
        for (rhi::TextureHandle t : sprite_atlas_) d_.destroy_texture(t);
        lights_.shutdown();
        lit_shapes_.shutdown();
        for (rhi::PipelineHandle p : {blend_[0], blend_[1], blend_[2], blend_[3], blend_[4], textured_additive_, strip_, points_, lines_, line_strip_,
                                      instanced_, textured_}) {
            d_.destroy_pipeline(p);
        }
        for (rhi::BufferHandle b : owned_buffers_) d_.destroy_buffer(b);
        d_.destroy_buffer(params_);
        d_.destroy_buffer(palette_);
        for (rhi::TextureHandle t : {checker_[0], checker_[1], mask_, blank_, mips_[0], mips_[1], srgb_, lut_}) d_.destroy_texture(t);
    }

    // Every scene: name + draw function (state is reset by begin_frame).
    std::vector<std::pair<std::string, std::function<void()>>> list() {
        return {
            {"clear_scissor_viewport", [this] { clear_scissor_viewport(); }},
            {"triangles_and_strip", [this] { triangles_and_strip(); }},
            {"indexed_16_32", [this] { indexed(); }},
            {"instanced_first_instance", [this] { instanced(); }},
            {"blend_modes", [this] { blend_modes(); }},
            {"render_targets", [this] { render_targets(); }},
            {"additive_glow", [this] { additive_glow(); }},
            {"post_bloom_vignette", [this] { post_scene(false); }},
            {"post_lut_crt", [this] { post_scene(true); }},
            {"points_lines", [this] { points_lines(); }},
            {"uniform_offsets", [this] { uniform_offsets(); }},
            {"textures", [this] { textures(); }},
            {"mips", [this] { mips(); }},
            {"sprites", [this] { sprites(); }},
            {"mesh_gradient", [this] { mesh_gradient(); }},
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
    // Mesh gradient paints (11.2b): stops interpolate in linear space under --linear.
    bool init_mesh_gradient() {
        const vector::SvgResult svg = vector::parse_svg(
            R"x(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><defs>
<linearGradient id="a" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#ff2020"/><stop offset="0.5" stop-color="#20ff20"/><stop offset="1" stop-color="#2020ff"/></linearGradient>
<radialGradient id="b"><stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#802000" stop-opacity="0.6"/></radialGradient></defs>
<rect x="0" y="0" width="100" height="40" fill="url(#a)"/><circle cx="50" cy="72" r="26" fill="url(#b)"/><rect x="2" y="42" width="20" height="10" fill="#808080"/></svg>)x",
            "parity_gradient");
        if (!svg.ok || !meshes_.init(d_)) return false;
        vector::VectorMesh mesh;
        vector::DocumentTessellator().build(svg.document, 0, mesh);
        gradient_mesh_ = meshes_.create_mesh(mesh);
        return static_cast<bool>(gradient_mesh_);
    }

    void mesh_gradient() {
        const float s = static_cast<float>(target_size);
        meshes_.clear();
        meshes_.draw(gradient_mesh_, Affine2::translate({8.0f, 8.0f}) * Affine2::scale({2.2f, 2.2f}));
        meshes_.draw(gradient_mesh_, Affine2::translate({140.0f, 140.0f}) * Affine2::scale({1.0f, 1.0f}),
                     {1.0f, 1.0f, 1.0f, 0.7f});
        meshes_.flush(render::virtual_to_ndc({s, s}));
    }

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
        for (std::size_t b = 0; b < 5; ++b) { // None, Alpha, Premultiplied, Additive, Screen
            colored_quad(2 + b, blend_[b], 8.0f + static_cast<float>(b) * 50.0f, 40, 42, 176, color);
        }
    }

    // 11.2b: a pass into an offscreen target (MSAA edges, a transparent clear),
    // a second pass that downsamples it into a smaller target, and the window
    // drawn before and after the detour (its contents must survive it). The
    // result is composited with alpha blending and additively.
    void render_targets() {
        d_.clear({0.1f, 0.1f, 0.14f, 1.0f});
        colored_quad(0, blend_[0], 0, 0, 256, 20, {0.9f, 0.8f, 0.2f, 1.0f});
        const rhi::RenderTargetHandle a = d_.create_render_target({128, 128, rhi::Filter::Linear, "parity_a"});
        const rhi::RenderTargetHandle b = d_.create_render_target({64, 64, rhi::Filter::Linear, "parity_b"});
        if (!a || !b) return;
        d_.set_render_target(a);
        d_.clear({0.0f, 0.0f, 0.0f, 0.0f});
        colored_quad(1, blend_[1], 16, 16, 112, 112, {0.9f, 0.2f, 0.2f, 1.0f});
        colored_quad(2, blend_[1], 112, 112, 128, 128, {0.2f, 0.8f, 0.3f, 0.6f});
        {
            const std::vector<ColorVertex> tri = {cv(128, 20, 0x4060ffff), cv(236, 110, 0x4060ffff), cv(60, 90, 0xffe040ff)};
            const rhi::BufferHandle tb = make_buffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static, bytes(tri));
            d_.bind_pipeline(blend_[1]);
            d_.bind_vertex_buffer(0, tb);
            use_params(3, 0, 0, 1, 1);
            d_.draw(3);
        }
        d_.set_render_target(b);
        d_.bind_pipeline(textured_);
        d_.bind_vertex_buffer(0, target_quad_);
        d_.bind_texture(0, d_.render_target_texture(a));
        d_.bind_texture(1, mask_);
        use_params(4, 0, 0, 256, 256, {1, 1, 1, 0}); // half-size copy: 2x2 box average
        d_.draw(6);
        d_.set_render_target({}); // back to the window, which still holds the strip
        colored_quad(5, blend_[1], 0, 10, 256, 10, {0.2f, 0.7f, 0.9f, 0.5f});
        d_.bind_pipeline(textured_);
        d_.bind_vertex_buffer(0, target_quad_);
        d_.bind_texture(0, d_.render_target_texture(a));
        d_.bind_texture(1, mask_);
        use_params(6, 0, 32, 128, 128, {1, 1, 1, 0});
        d_.draw(6);
        d_.bind_texture(0, d_.render_target_texture(b));
        use_params(7, 140, 32, 112, 112, {1, 1, 1, 0}); // magnified, linear
        d_.draw(6);
        d_.bind_pipeline(textured_additive_);
        use_params(8, 8, 176, 64, 64, {1, 1, 1, 0});
        d_.draw(6);
        d_.bind_texture(0, d_.render_target_texture(a));
        use_params(9, 80, 176, 64, 64, {1, 1, 1, 0});
        d_.draw(6);
        d_.destroy_render_target(a);
        d_.destroy_render_target(b);
    }

    // 11.2b: instanced SDF shapes composited normally, additively (sums, can
    // exceed white) and with Screen (never exceeds white), in one flush.
    void additive_glow() {
        d_.clear({0.05f, 0.05f, 0.1f, 1.0f});
        const float s = static_cast<float>(target_size);
        const Affine2 to_ndc = render::virtual_to_ndc({s, s});
        lit_shapes_.clear();
        lit_shapes_.circle({70.0f, 60.0f}, 40.0f, {0.9f, 0.3f, 0.1f, 0.8f});
        lit_shapes_.circle({100.0f, 60.0f}, 40.0f, {0.1f, 0.4f, 0.9f, 0.8f});
        lit_shapes_.set_blend(render::SdfBlend::Additive);
        lit_shapes_.circle({70.0f, 140.0f}, 40.0f, {0.9f, 0.3f, 0.1f, 0.8f});
        lit_shapes_.circle({100.0f, 140.0f}, 40.0f, {0.1f, 0.4f, 0.9f, 0.8f});
        lit_shapes_.circle({85.0f, 160.0f}, 40.0f, {0.2f, 0.9f, 0.2f, 0.8f});
        lit_shapes_.set_blend(render::SdfBlend::Screen);
        lit_shapes_.circle({190.0f, 140.0f}, 40.0f, {0.9f, 0.3f, 0.1f, 0.8f});
        lit_shapes_.circle({220.0f, 140.0f}, 40.0f, {0.1f, 0.4f, 0.9f, 0.8f});
        lit_shapes_.circle({205.0f, 160.0f}, 40.0f, {0.2f, 0.9f, 0.2f, 0.8f});
        lit_shapes_.set_blend(render::SdfBlend::Normal);
        lit_shapes_.circle({190.0f, 60.0f}, 30.0f, {0.9f, 0.9f, 0.2f, 0.5f});
        lit_shapes_.flush(to_ndc, 1.0f);
    }

    // 11.2b post-processing: the frame is drawn into a render target and run
    // through PostFx into the window. `grade`: LUT + CRT; otherwise bloom + vignette.
    void post_scene(bool grade) {
        const rhi::RenderTargetHandle scene = d_.create_render_target({target_size, target_size, rhi::Filter::Linear, "parity_scene"});
        if (!scene) return;
        d_.set_render_target(scene);
        d_.clear({0.04f, 0.05f, 0.12f, 1.0f});
        const float s = static_cast<float>(target_size);
        lit_shapes_.clear();
        lit_shapes_.circle({80.0f, 90.0f}, 34.0f, {1.0f, 0.95f, 0.7f, 1.0f});
        lit_shapes_.ring({180.0f, 80.0f}, 28.0f, 8.0f, {0.4f, 0.9f, 1.0f, 1.0f});
        lit_shapes_.rounded_rect({70.0f, 190.0f}, {40.0f, 20.0f}, 6.0f, 0.3f, {1.0f, 0.4f, 0.3f, 1.0f});
        lit_shapes_.circle({190.0f, 190.0f}, 22.0f, {0.3f, 0.3f, 0.35f, 1.0f});
        lit_shapes_.flush(render::virtual_to_ndc({s, s}), 1.0f);

        render::PostSettings settings;
        if (grade) {
            settings.lut = lut_;
            settings.lut_strength = 0.9f;
            settings.crt = true;
        } else {
            settings.bloom = true;
            settings.bloom_threshold = 0.55f;
            settings.bloom_intensity = 1.0f;
            settings.vignette = 0.6f;
        }
        post_.apply(scene, {target_size, target_size}, {}, settings);
        d_.destroy_render_target(scene);
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

    // 11.2a: a 64x64 chain minified by powers of two lands exactly on one level
    // (sizes on pixel edges), 24 px blends two levels (trilinear / nearest level),
    // and an sRGB texture is decoded before sampling.
    void mips() {
        d_.bind_pipeline(textured_);
        d_.bind_vertex_buffer(0, uv_quad_);
        d_.bind_texture(1, mask_);
        const float sizes[] = {64, 32, 16, 8, 4, 24, 12};
        std::size_t slot = 0;
        for (int f = 1; f >= 0; --f) { // linear row first
            d_.bind_texture(0, mips_[f]);
            float x = 8;
            const float y = f == 1 ? 8.0f : 84.0f;
            for (float size : sizes) {
                use_params(slot++, x, y, size, size, {1, 1, 1, 0});
                d_.draw(6);
                x += size + 8;
            }
        }
        d_.bind_texture(0, srgb_);
        use_params(slot, 8, 160, 128, 32, {1, 1, 1, 0});
        d_.draw(6);
    }

    // Deterministic 16x16 sprites (asymmetric, so flips show) plus a 48x48 one and
    // a soft round one with partial alpha.
    static assets::Image make_sprite_image(int k, int size) {
        assets::Image img;
        img.width = img.height = size;
        img.rgba.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                std::uint8_t* t = &img.rgba[static_cast<std::size_t>((y * size + x) * 4)];
                const int u = x * 255 / (size - 1), v = y * 255 / (size - 1);
                t[0] = static_cast<std::uint8_t>((u + k * 60) & 255);
                t[1] = static_cast<std::uint8_t>(v);
                t[2] = ((x / 2 + y / 2) & 1) ? 200 : 40;
                t[3] = 255;
                if (x < size / 4 && y < size / 4) t[0] = t[1] = t[2] = 255; // top-left mark
                if (k == 3) { // round, soft edge
                    const float dx = static_cast<float>(x) - static_cast<float>(size - 1) * 0.5f;
                    const float dy = static_cast<float>(y) - static_cast<float>(size - 1) * 0.5f;
                    const float r = std::sqrt(dx * dx + dy * dy) / (static_cast<float>(size) * 0.5f);
                    t[3] = static_cast<std::uint8_t>(std::clamp(255.0f * (1.4f - 1.4f * r), 0.0f, 255.0f));
                }
            }
        }
        return img;
    }

    // A 9-slice source: a 6 texel border (darker, with a stripe) around a light middle.
    static assets::Image make_panel_image(int size) {
        assets::Image img;
        img.width = img.height = size;
        img.rgba.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                std::uint8_t* t = &img.rgba[static_cast<std::size_t>((y * size + x) * 4)];
                const bool border = x < 6 || y < 6 || x >= size - 6 || y >= size - 6;
                const bool corner = (x < 6 || x >= size - 6) && (y < 6 || y >= size - 6);
                t[0] = border ? (corner ? 230 : 40) : 190;
                t[1] = border ? (corner ? 120 : 60) : 215;
                t[2] = border ? (corner ? 40 : 150) : 235;
                t[3] = 255;
                if (border && !corner && ((x + y) & 3) == 0) t[0] = t[1] = t[2] = 120; // stretch shows
            }
        }
        return img;
    }

    bool init_sprites() {
        std::vector<assets::AtlasInput> inputs;
        inputs.push_back({"a", make_sprite_image(0, 16)});
        inputs.push_back({"b", make_sprite_image(1, 16)});
        inputs.push_back({"c", make_sprite_image(2, 16)});
        inputs.push_back({"round", make_sprite_image(3, 16)});
        inputs.push_back({"big", make_sprite_image(4, 48)});
        inputs.push_back({"panel", make_panel_image(24)});
        // SVG raster fallback (11.2a): rasterized by the engine's own rasterizer at 2 px per unit.
        const assets::SvgRasterResult svg = assets::rasterize_svg(
            R"x(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#ffd23f"/><stop offset="1" stop-color="#ee4266"/></linearGradient></defs><circle cx="8" cy="8" r="6.5" fill="url(#g)" stroke="#1b1b3a" stroke-width="1.5"/><rect x="5" y="7" width="6" height="2" fill="#ffffff"/></svg>)x",
            2.0f, "parity");
        if (!svg.raster) return false;
        inputs.push_back({"svg", svg.raster->image});
        assets::AtlasResult packed = assets::pack_atlas(inputs);
        if (!packed.atlas || packed.atlas->pages.size() != 1) {
            LOG_ERROR("rhi_parity: sprite atlas failed: {}", packed.error);
            return false;
        }
        atlas_ = std::move(*packed.atlas);
        // build_texture(srgb = true) averages mips in linear light; the GPU still samples the
        // stored bytes as display-referred (create_sprite_texture).
        const assets::TextureData tex = assets::build_texture(atlas_.pages[0], true);
        atlas_w_ = tex.width;
        atlas_h_ = tex.height;
        sprite_atlas_[0] = render::create_sprite_texture(d_, tex, rhi::Filter::Nearest, "parity_atlas_nearest");
        sprite_atlas_[1] = render::create_sprite_texture(d_, tex, rhi::Filter::Linear, "parity_atlas_linear");
        return sprite_atlas_[0] && sprite_atlas_[1] && sprite_batcher_.init(d_, 64);
    }

    render::SpriteUv frame_uv(const char* name, bool fx = false, bool fy = false) const {
        const assets::AtlasFrame* f = atlas_.find(name);
        return f ? render::sprite_uv(*f, atlas_w_, atlas_h_, fx, fy) : render::SpriteUv{};
    }

    void sprites() {
        const Affine2 to_ndc = render::virtual_to_ndc({256.0f, 256.0f});
        lit_shapes_.clear();
        lit_shapes_.rounded_rect({128.0f, 128.0f}, {124.0f, 124.0f}, 6.0f, 0.0f, {0.18f, 0.2f, 0.28f, 1.0f});
        lit_shapes_.flush(to_ndc, 1.0f);

        sprite_batcher_.clear();
        const rhi::Color white{1, 1, 1, 1};
        // Rows 1 and 2: the same frames with the nearest and the linear texture, 16 -> 32 px.
        const char* names[] = {"a", "b", "c", "a"};
        for (int row = 0; row < 2; ++row) {
            for (int i = 0; i < 4; ++i) {
                const bool fx = i == 2, fy = i == 3;
                rhi::Color tint = white;
                if (row == 1 && i == 1) tint = {1.0f, 0.4f, 0.4f, 1.0f};
                if (row == 1 && i == 2) tint = {0.4f, 1.0f, 0.6f, 0.6f};
                sprite_batcher_.add(sprite_atlas_[row], {24.0f + 56.0f * static_cast<float>(i), 24.0f + 40.0f * static_cast<float>(row)},
                                    {32, 32}, 0.0f, frame_uv(names[i], fx, fy), tint);
            }
        }
        // Row 3: the 48 px sprite minified 4:1 (mip level 2), a source sub-rect magnified,
        // a soft-edged round sprite over the backdrop, and a rotated one.
        if (const assets::AtlasFrame* big = atlas_.find("big")) {
            sprite_batcher_.add(sprite_atlas_[1], {24, 104}, {12, 12}, 0.0f, frame_uv("big"));
            sprite_batcher_.add(sprite_atlas_[1], {64, 104}, {24, 24}, 0.0f, frame_uv("big"));
            sprite_batcher_.add(sprite_atlas_[0], {120, 104}, {32, 32}, 0.0f,
                                render::sprite_uv(atlas_w_, atlas_h_, big->x + 8, big->y + 8, 8, 8));
        }
        sprite_batcher_.add(sprite_atlas_[1], {176, 104}, {40, 40}, 0.0f, frame_uv("round"));
        sprite_batcher_.add(sprite_atlas_[1], {224, 104}, {32, 32}, 0.0f, frame_uv("round"), {1.0f, 0.8f, 0.2f, 1.0f});
        // Row 4: many instances of one texture are one draw; rotated sprites.
        for (int i = 0; i < 5; ++i) {
            sprite_batcher_.add(sprite_atlas_[0], {24.0f + 28.0f * static_cast<float>(i), 160.0f}, {16, 16}, 0.0f,
                                frame_uv(names[i % 3], (i & 1) != 0, false));
        }
        // 9-slice (nearest and linear, one rotated) of the 24 px panel: 6 texel borders kept, the
        // middle stretched; and the SVG raster fallback sprite.
        if (const assets::AtlasFrame* panel = atlas_.find("panel")) {
            const render::SpriteUv puv = render::sprite_uv(*panel, atlas_w_, atlas_h_);
            const render::NinePatchMargins margins{6, 6, 6, 6};
            render::add_nine_patch(sprite_batcher_, sprite_atlas_[0], puv, 24, 24, margins, {188, 152}, {88, 32}, 0.0f);
            render::add_nine_patch(sprite_batcher_, sprite_atlas_[1], puv, 24, 24, margins, {232, 190}, {40, 44}, 0.0f,
                                   {1, 1, 1, 1}, false);
            render::add_nine_patch(sprite_batcher_, sprite_atlas_[1], puv, 24, 24, margins, {228, 232}, {40, 20}, 0.3f);
        }
        sprite_batcher_.add(sprite_atlas_[1], {112, 190}, {32, 32}, 0.0f, frame_uv("svg"));
        sprite_batcher_.add(sprite_atlas_[1], {64, 216}, {48, 48}, 0.5f, frame_uv("b"));
        sprite_batcher_.add(sprite_atlas_[1], {160, 216}, {48, 24}, -0.3f, frame_uv("a"), {1, 1, 1, 0.8f});
        sprite_batcher_.flush(to_ndc);
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
    render::MeshBatcher meshes_;
    render::MeshHandle gradient_mesh_;
    render::PostFx post_;
    render::SdfBatcher lit_shapes_;
    std::array<rhi::PipelineHandle, 5> blend_{};
    rhi::PipelineHandle textured_additive_;
    rhi::BufferHandle target_quad_;
    rhi::PipelineHandle strip_, points_, lines_, line_strip_, instanced_, textured_;
    rhi::BufferHandle params_, palette_, quad_, uv_quad_, dynamic_, stream_, big_stream_, bv_vertices_, bv_indices_;
    std::array<rhi::TextureHandle, 2> checker_{};
    rhi::TextureHandle mask_, blank_, srgb_, lut_;
    rhi::TextureHandle sprite_atlas_[2]; // nearest, linear
    assets::Atlas atlas_;
    std::int32_t atlas_w_ = 0, atlas_h_ = 0;
    render::SpriteBatcher sprite_batcher_;
    rhi::TextureHandle mips_[2]; // nearest, linear
    std::vector<rhi::BufferHandle> owned_buffers_;
    std::size_t align_ = 256;
};

struct DeviceArgs {
    rhi::Backend backend = rhi::Backend::OpenGL;
    bool gl_es = false;
    rhi::Tier tier = rhi::Tier::Compatible;
    bool linear = false; // --linear: DeviceOptions::linear_color (4.19)
};

bool parse_device_args(int argc, char** argv, DeviceArgs& out) {
    if (!rhi::parse_backend(arg_value(argc, argv, "--backend"), out.backend)) {
        LOG_ERROR("--backend gl|vulkan|webgpu is required");
        return false;
    }
    out.gl_es = has_flag(argc, argv, "--gl-es");
    out.linear = has_flag(argc, argv, "--linear");
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
    options.linear_color = args.linear;
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
    // --linear renders like a linear-color game (game_runner): the scene goes to a
    // render target, PostFx's composite pass encodes it for the window. Only scenes
    // that draw straight to the window take part (the render-target scenes manage
    // their own targets).
    const bool linear = device->info().linear_color;
    render::PostFx composite;
    rhi::RenderTargetHandle scene_target;
    if (linear) {
        if (!composite.init(*device)) {
            LOG_ERROR("--linear: the composite pass is unavailable");
            return 1;
        }
        scene_target = device->create_render_target({target_size, target_size, rhi::Filter::Linear, "parity_scene"});
    }
    static constexpr std::string_view linear_scenes[] = {"mesh_gradient", "lighting_shadows", "lighting_ambient",
                                                         "sprites", "blend_modes", "textures"};
    for (auto& [name, draw] : scenes.list()) {
        if (linear && std::find(std::begin(linear_scenes), std::end(linear_scenes), name) == std::end(linear_scenes)) {
            continue;
        }
        const fs::path file = out / (name + ".ppm");
        fs::remove(file, ec);
        // A few attempts: a Vulkan swapchain may need a frame or two to settle.
        for (int attempt = 0; attempt < 4 && !fs::exists(file); ++attempt) {
            DeviceState devices;
            window.poll_events(devices);
            device->begin_frame(window.pixel_size());
            const rhi::Color clear{0.08f, 0.09f, 0.12f, 1.0f};
            if (linear) {
                device->set_render_target(scene_target);
                device->clear(render::srgb_to_linear(clear));
            } else {
                device->clear(clear);
            }
            draw();
            if (linear) composite.apply(scene_target, {target_size, target_size}, {}, {});
            device->capture_frame(file.string());
            device->end_frame();
        }
        if (!fs::exists(file)) {
            LOG_ERROR("Scene '{}' was not captured", name);
            ++failures;
        }
    }
    // Base vertex: on a device that may use it, it must equal the reference.
    if (!linear && device->info().supports(rhi::Feature::BaseVertexDraws)) {
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
    composite.shutdown();
    if (scene_target) device->destroy_render_target(scene_target);
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
    const bool expect_different = has_flag(argc, argv, "--expect-different");
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
        const bool ok = expect_different ? d.differing > 0 : fraction <= max_fraction;
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

// Renders scenes to the main window and to a second window's surface
// (Feature::MultiSurface) on one device, interleaved, and requires the two
// captures to be identical: shared resources, per-surface swapchain / context.
int surfaces(int argc, char** argv) {
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
    if (!device->info().supports(rhi::Feature::MultiSurface)) {
        LOG_INFO("{} has no multi-surface support: skipped", rhi::backend_name(args.backend));
        device.reset();
        return 0;
    }
    SDL_Window* second = rhi::create_surface_window(args.backend, "rhi_parity second", target_size, target_size, false);
    if (!second) return 1;
    const rhi::SurfaceHandle surface = device->create_window_surface(second);
    if (!surface) return 1;
    Scenes scenes(*device);
    if (!scenes.init()) return 1;
    int failures = 0;
    constexpr int warmup = 4; // a Vulkan swapchain may need a frame or two to settle
    for (auto& [name, draw] : scenes.list()) {
        if (name != "triangles_and_strip" && name != "textures" && name != "blend_modes" && name != "sprites") continue;
        const fs::path main_file = out / (name + "_main.ppm");
        const fs::path second_file = out / (name + "_second.ppm");
        fs::remove(main_file, ec);
        fs::remove(second_file, ec);
        for (int frame = 0; frame < warmup + 2; ++frame) {
            const bool last = frame >= warmup;
            device->begin_frame(window.pixel_size());
            device->clear({0.08f, 0.09f, 0.12f, 1.0f});
            draw();
            if (last) device->capture_frame(main_file.string());
            device->end_frame();
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(second, &w, &h);
            device->begin_frame_surface(surface, {w, h});
            device->clear({0.08f, 0.09f, 0.12f, 1.0f});
            draw();
            if (last) device->capture_frame(second_file.string());
            device->end_frame_surface();
        }
        const auto a = read_ppm(main_file);
        const auto b = read_ppm(second_file);
        const Diff d = a && b ? diff_images(*a, *b, 0) : Diff{};
        if (!a || !b || !d.same_size || d.differing != 0) {
            LOG_ERROR("{}: the second surface differs from the main window ({} pixels)", name, d.differing);
            ++failures;
        } else {
            LOG_INFO("{}: second surface identical to the main window", name);
        }
    }
    scenes.shutdown();
    device->destroy_window_surface(surface);
    SDL_DestroyWindow(second);
    device.reset();
    window.destroy();
    return failures == 0 ? 0 : 1;
}

// Imports a TTF/OTF as a shaping-enabled Latin + Arabic .efont. The runtime's own
// --font import has no range options, so the web smoke project (tools/web/render_game)
// builds its default font here; the web build only reads .efont files.
int font_command(const char* ttf, const char* out) {
    std::vector<std::byte> data;
    if (!vector::read_file(ttf, data)) {
        LOG_ERROR("cannot read '{}'", ttf);
        return 1;
    }
    render::FontImportOptions options;
    options.ranges = {{0x20, 0x7E}, {0x600, 0x6FF}, {0xFB50, 0xFDFF}, {0xFE70, 0xFEFF}};
    options.em_size = 32.0f;
    options.shaping = true;
    const auto font = render::import_font(data, "web_smoke_font", options);
    if (!font || !vector::write_file(out, render::serialize_font_atlas(*font))) return 1;
    LOG_INFO("Imported '{}' -> '{}' ({} glyphs)", ttf, out, font->glyphs.size());
    return 0;
}

int usage() {
    std::fputs("usage:\n"
               "  rhi_parity font <font.ttf> <out.efont>   (Latin + Arabic, shaping data kept)\n"
               "  rhi_parity render --backend gl|vulkan|webgpu [--gl-es] [--tier compatible|full] --out <dir>\n"
               "  rhi_parity frames --backend gl|vulkan|webgpu [--gl-es] [--count <n>] --out <dir>\n"
               "  rhi_parity surfaces --backend gl|vulkan|webgpu [--gl-es] --out <dir>\n"
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
    if (cmd == "surfaces") return surfaces(argc, argv);
    if (cmd == "compare") return argc >= 4 ? compare(argc, argv) : usage();
    if (cmd == "features") return features(argc, argv);
    if (cmd == "font") return argc >= 4 ? font_command(argv[2], argv[3]) : usage();
    return usage();
}
