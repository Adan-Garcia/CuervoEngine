#include "editor/imgui_rhi.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "cuervo/core/log.hpp"
#include "shaders/imgui_frag.hpp"
#include "shaders/imgui_vert.hpp"

namespace cuervo::editor {

namespace {

static_assert(sizeof(ImDrawIdx) == 2, "the RHI pipeline draws ImGui with 16-bit indices");
static_assert(sizeof(ImDrawVert) == 20);

constexpr std::array<rhi::VertexAttribute, 3> vertex_attributes{{
    {0, rhi::VertexFormat::Float2, offsetof(ImDrawVert, pos)},
    {1, rhi::VertexFormat::Float2, offsetof(ImDrawVert, uv)},
    {2, rhi::VertexFormat::UByte4Norm, offsetof(ImDrawVert, col)}, // IM_COL32: r in the low byte
}};

struct Globals {
    float scale_offset[4];
};

rhi::TextureHandle handle_of(ImTextureID id) { return {static_cast<std::uint32_t>(id)}; }

} // namespace

ImGuiRenderer::~ImGuiRenderer() { shutdown(); }

bool ImGuiRenderer::init(rhi::Device& device) {
    shutdown();
    device_ = &device;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "cuervo_rhi";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    const std::array<rhi::VertexStream, 1> streams{{
        {sizeof(ImDrawVert), rhi::VertexRate::PerVertex, vertex_attributes},
    }};
    rhi::PipelineDesc desc;
    desc.vertex = shaders::imgui_vert;
    desc.fragment = shaders::imgui_frag;
    desc.streams = streams;
    desc.blend = rhi::BlendMode::Alpha;
    desc.debug_name = "imgui";
    pipeline_ = device.create_pipeline(desc);
    globals_ = device.create_buffer({rhi::BufferKind::Uniform, rhi::BufferUsage::Dynamic, sizeof(Globals), nullptr});
    if (!pipeline_ || !globals_) {
        LOG_ERROR("ImGui renderer: failed to create GPU resources");
        shutdown();
        return false;
    }
    return true;
}

void ImGuiRenderer::shutdown() {
    if (device_ == nullptr) return;
    if (ImGui::GetCurrentContext() != nullptr) {
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
            if (tex->TexID != ImTextureID_Invalid) {
                device_->destroy_texture(handle_of(tex->TexID));
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
        ImGuiIO& io = ImGui::GetIO();
        io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
        io.BackendRendererName = nullptr;
    }
    device_->destroy_pipeline(pipeline_);
    device_->destroy_buffer(vertices_);
    device_->destroy_buffer(indices_);
    device_->destroy_buffer(globals_);
    pipeline_ = {};
    vertices_ = indices_ = globals_ = {};
    vertex_capacity_ = index_capacity_ = 0;
    device_ = nullptr;
}

void ImGuiRenderer::update_texture(ImTextureData* tex) {
    if (tex->Status == ImTextureStatus_WantCreate) {
        if (tex->Format != ImTextureFormat_RGBA32) {
            LOG_ERROR("ImGui renderer: only RGBA32 textures are supported");
            return;
        }
        rhi::TextureDesc desc;
        desc.width = tex->Width;
        desc.height = tex->Height;
        desc.format = rhi::TextureFormat::RGBA8;
        desc.filter = rhi::Filter::Linear;
        desc.initial_data = tex->GetPixels();
        desc.debug_name = "imgui";
        const rhi::TextureHandle handle = device_->create_texture(desc);
        if (!handle) return; // logged by the device; ImGui retries next frame
        tex->SetTexID(static_cast<ImTextureID>(handle.id));
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        // ImGui only writes regions no earlier draw samples, so the RHI's
        // "update before drawing" rule (rhi.hpp) holds on both backends.
        const rhi::TextureHandle handle = handle_of(tex->TexID);
        for (const ImTextureRect& r : tex->Updates) {
            const std::size_t row = static_cast<std::size_t>(r.w) * 4;
            staging_.resize(row * r.h);
            for (int y = 0; y < r.h; ++y) {
                std::memcpy(staging_.data() + row * static_cast<std::size_t>(y), tex->GetPixelsAt(r.x, r.y + y), row);
            }
            device_->update_texture(handle, {r.x, r.y, r.w, r.h}, staging_);
        }
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        device_->destroy_texture(handle_of(tex->TexID)); // deferred by the RHI while in flight
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

bool ImGuiRenderer::reserve(rhi::BufferHandle& buffer, std::size_t& capacity, std::size_t bytes,
                            rhi::BufferKind kind) {
    if (bytes <= capacity && buffer) return true;
    std::size_t grown = std::max<std::size_t>(capacity * 2, 64 * 1024);
    while (grown < bytes) grown *= 2;
    device_->destroy_buffer(buffer);
    buffer = device_->create_buffer({kind, rhi::BufferUsage::Stream, grown, nullptr});
    capacity = buffer ? grown : 0;
    return static_cast<bool>(buffer);
}

void ImGuiRenderer::bind_state() {
    const ivec2 fb = device_->framebuffer_size();
    device_->set_viewport({0, 0, fb.x(), fb.y()});
    device_->bind_pipeline(pipeline_);
    device_->bind_uniform_buffer(0, globals_);
    device_->bind_index_buffer(indices_, rhi::IndexFormat::UInt16);
}

void ImGuiRenderer::render(ImDrawData* draw_data) {
    if (device_ == nullptr || draw_data == nullptr) return;
    if (draw_data->Textures != nullptr) {
        for (ImTextureData* tex : *draw_data->Textures) {
            if (tex->Status != ImTextureStatus_OK) update_texture(tex);
        }
    }
    const ImVec2 scale = draw_data->FramebufferScale;
    const ivec2 fb = device_->framebuffer_size();
    if (fb.x() <= 0 || fb.y() <= 0 || draw_data->TotalVtxCount == 0) return;

    // Every list's vertices and indices, each in one upload per frame
    // (Stream buffers: written once, then drawn from).
    const std::size_t vtx_bytes = static_cast<std::size_t>(draw_data->TotalVtxCount) * sizeof(ImDrawVert);
    const std::size_t idx_bytes = static_cast<std::size_t>(draw_data->TotalIdxCount) * sizeof(ImDrawIdx);
    if (!reserve(vertices_, vertex_capacity_, vtx_bytes, rhi::BufferKind::Vertex) ||
        !reserve(indices_, index_capacity_, idx_bytes, rhi::BufferKind::Index)) {
        return;
    }
    staging_.resize(std::max(vtx_bytes, idx_bytes));
    std::size_t at = 0;
    for (const ImDrawList* list : draw_data->CmdLists) {
        const std::size_t n = static_cast<std::size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        std::memcpy(staging_.data() + at, list->VtxBuffer.Data, n);
        at += n;
    }
    device_->update_buffer(vertices_, 0, std::span(staging_.data(), vtx_bytes));
    at = 0;
    for (const ImDrawList* list : draw_data->CmdLists) {
        const std::size_t n = static_cast<std::size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
        std::memcpy(staging_.data() + at, list->IdxBuffer.Data, n);
        at += n;
    }
    device_->update_buffer(indices_, 0, std::span(staging_.data(), idx_bytes));

    // Display coordinates -> NDC (y up on every backend, rhi.hpp).
    const ImVec2 pos = draw_data->DisplayPos;
    const ImVec2 size = draw_data->DisplaySize;
    const Globals g{{2.0f / size.x, -2.0f / size.y, -1.0f - pos.x * 2.0f / size.x, 1.0f + pos.y * 2.0f / size.y}};
    device_->update_buffer(globals_, 0, std::as_bytes(std::span(&g, 1)));

    bind_state();
    std::size_t vtx_offset = 0;
    std::uint32_t idx_offset = 0;
    for (const ImDrawList* list : draw_data->CmdLists) {
        device_->bind_vertex_buffer(0, vertices_, vtx_offset);
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback != nullptr) {
                if (cmd.UserCallback != ImGui::GetPlatformIO().DrawCallback_ResetRenderState) cmd.UserCallback(list, &cmd);
                bind_state();
                device_->bind_vertex_buffer(0, vertices_, vtx_offset);
                continue;
            }
            // Clip rectangle in framebuffer pixels (top-left origin, like ImGui).
            const float x0 = std::max((cmd.ClipRect.x - pos.x) * scale.x, 0.0f);
            const float y0 = std::max((cmd.ClipRect.y - pos.y) * scale.y, 0.0f);
            const float x1 = std::min((cmd.ClipRect.z - pos.x) * scale.x, static_cast<float>(fb.x()));
            const float y1 = std::min((cmd.ClipRect.w - pos.y) * scale.y, static_cast<float>(fb.y()));
            if (x1 <= x0 || y1 <= y0 || cmd.ElemCount == 0) continue;
            const auto ix0 = static_cast<std::int32_t>(x0);
            const auto iy0 = static_cast<std::int32_t>(y0);
            device_->set_scissor({ix0, iy0, static_cast<std::int32_t>(x1 + 0.5f) - ix0,
                                  static_cast<std::int32_t>(y1 + 0.5f) - iy0});
            device_->bind_texture(0, handle_of(cmd.GetTexID()));
            device_->draw_indexed(cmd.ElemCount, 1, idx_offset + cmd.IdxOffset);
        }
        vtx_offset += static_cast<std::size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        idx_offset += static_cast<std::uint32_t>(list->IdxBuffer.Size);
    }
    const ivec2 full = device_->framebuffer_size();
    device_->set_scissor({0, 0, full.x(), full.y()});
}

} // namespace cuervo::editor
