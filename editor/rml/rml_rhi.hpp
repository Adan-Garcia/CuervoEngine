#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "cuervo/assets/image.hpp"
#include "cuervo/rhi/rhi.hpp"

// RmlUi renderer on the engine's RHI (DESIGN.md 4.4, 4.13 spike): the same pipeline and shaders as
// the ImGui renderer (editor/shaders/imgui.*), so Vulkan and GL share one code path with no
// backend-specific calls. RmlUi's vertices are premultiplied, so the pipeline blends with
// BlendMode::Premultiplied. Geometry is kept on the CPU after CompileGeometry; RenderGeometry
// appends it (translated) to one per-frame stream, and end_frame() uploads once and issues the
// draws with their scissor rects. Not implemented (spike): transforms, clip masks, layers,
// filters, shaders (RmlUi then skips box-shadow, filter, gradient decorators).
namespace cuervo::editor {

// Images the shell makes in memory (an SVG rasterized, a frame cropped from a packed atlas): register one
// under a key that changes with its pixels, then use "mem:<key>" as an <img> source. A few are kept.
void rml_set_memory_image(const std::string& key, const assets::Image& image);

class RmlRenderer final : public Rml::RenderInterface {
public:
    ~RmlRenderer() override;
    bool init(rhi::Device& device);
    void shutdown();
    // Pixel ratio the icon rasterizer targets ("icon:name:size" sources are drawn at size * ratio).
    void set_pixel_ratio(float ratio) { ratio_ = ratio; }
    void begin_frame();
    void end_frame();
    std::size_t draw_calls() const { return last_draws_; }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
    Rml::TextureHandle make_texture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions, rhi::Filter filter);
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

private:
    struct Geometry {
        std::vector<Rml::Vertex> vertices;
        std::vector<int> indices;
    };
    struct Draw {
        std::uint32_t texture = 0;
        std::uint32_t first_index = 0;
        std::uint32_t count = 0;
        bool scissor = false;
        Rml::Rectanglei rect;
    };
    bool reserve(rhi::BufferHandle& buffer, std::size_t& capacity, std::size_t bytes, rhi::BufferKind kind);

    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle vertices_, indices_, globals_;
    rhi::TextureHandle white_;
    std::size_t vertex_capacity_ = 0, index_capacity_ = 0;
    std::unordered_map<Rml::CompiledGeometryHandle, Geometry> geometries_;
    Rml::CompiledGeometryHandle next_geometry_ = 1;
    std::vector<Rml::Vertex> frame_vertices_;
    std::vector<std::uint32_t> frame_indices_;
    std::vector<Draw> draws_;
    bool scissor_on_ = false;
    Rml::Rectanglei scissor_;
    float ratio_ = 1.0f;
    std::size_t last_draws_ = 0;
};

// Rasterizes the editor's vector icon `name` (editor/icons.cpp) to a premultiplied white RGBA
// bitmap of `px` x `px`, for tinting with RCSS `image-color`.
std::vector<std::uint8_t> rasterize_icon(std::string_view name, int px);

} // namespace cuervo::editor
