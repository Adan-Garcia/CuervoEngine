#pragma once

#include <cstddef>
#include <vector>

#include "cuervo/rhi/rhi.hpp"

struct ImDrawData;
struct ImTextureData;

// Dear ImGui renderer on the engine's RHI (DESIGN.md 4.4, 4.13), so the
// editor draws the same way on Vulkan and GL with no backend-specific code:
// one pipeline (editor/shaders/imgui.*), the font atlas and any other ImGui
// textures as RHI textures (ImGuiBackendFlags_RendererHasTextures), all of a
// frame's vertices and 16-bit indices in two stream buffers, and a scissor
// per draw command. ImDrawCmd::VtxOffset is not used (the RHI has no base
// vertex on the Compatible tier); ImGui then splits large draw lists itself
// and each list binds its vertex range with a buffer offset instead.
//
// ImDrawList callbacks run in order between ImGui's draws (the scene view
// renders the engine's scene through one), after which ImGui's state is
// bound again.
namespace cuervo::editor {

class ImGuiRenderer {
public:
    ImGuiRenderer() = default;
    ~ImGuiRenderer();
    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

    // Sets the backend flags on the current ImGui context.
    bool init(rhi::Device& device);
    void shutdown();

    // Call between begin_frame and end_frame, after ImGui::Render().
    void render(ImDrawData* draw_data);

private:
    void update_texture(ImTextureData* tex);
    void bind_state();
    bool reserve(rhi::BufferHandle& buffer, std::size_t& capacity, std::size_t bytes, rhi::BufferKind kind);

    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle vertices_;
    rhi::BufferHandle indices_;
    rhi::BufferHandle globals_;
    std::size_t vertex_capacity_ = 0;
    std::size_t index_capacity_ = 0;
    std::vector<std::byte> staging_;
};

} // namespace cuervo::editor
