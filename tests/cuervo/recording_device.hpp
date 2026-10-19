#pragma once

#include <doctest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "cuervo/rhi/rhi.hpp"

namespace cuervo::test {

// Records what a batcher asks of the RHI, so packing, uploads and draw calls
// can be checked without a GPU.
class RecordingDevice final : public rhi::Device {
public:
    struct Draw {
        std::uint32_t index_count, instance_count, first_index, first_instance;
        std::uint32_t pipeline;
        std::array<std::uint32_t, rhi::max_vertex_streams> vertex_buffers;
        std::uint32_t uniform_buffer;
        std::array<std::uint32_t, rhi::max_texture_bindings> textures;
    };

    std::map<std::uint32_t, std::vector<std::byte>> buffers;
    struct Stream {
        std::uint32_t stride;
        rhi::VertexRate rate;
        std::size_t attributes;
    };
    struct Pipeline {
        rhi::BlendMode blend;
        bool has_spirv, has_es300;
        std::vector<Stream> streams;
        std::vector<std::string> textures; // sampler names, fragment stage
    };
    struct Texture {
        rhi::TextureDesc desc;
        std::vector<std::byte> texels;
    };

    std::map<std::uint32_t, rhi::BufferDesc> descs;
    std::map<std::uint32_t, Texture> textures;
    std::vector<Pipeline> pipelines;
    std::vector<Draw> draws;
    int updates = 0;
    int texture_updates = 0;
    std::uint32_t next = 1;

    const rhi::DeviceInfo& info() const override { return info_; }
    void begin_frame(ivec2) override {}
    void clear(rhi::Color) override {}
    void end_frame() override {}
    ivec2 framebuffer_size() const override { return {1280, 720}; }

    rhi::BufferHandle create_buffer(const rhi::BufferDesc& desc) override {
        std::vector<std::byte> data(desc.size);
        if (desc.initial_data) {
            std::memcpy(data.data(), desc.initial_data, desc.size);
        }
        buffers[next] = std::move(data);
        descs[next] = desc;
        return {next++};
    }
    void update_buffer(rhi::BufferHandle b, std::size_t offset, std::span<const std::byte> data) override {
        ++updates;
        auto& storage = buffers.at(b.id);
        REQUIRE(offset + data.size() <= storage.size());
        std::memcpy(storage.data() + offset, data.data(), data.size());
    }
    void destroy_buffer(rhi::BufferHandle b) override { buffers.erase(b.id); }

    rhi::TextureHandle create_texture(const rhi::TextureDesc& desc) override {
        Texture texture{desc, {}};
        texture.texels.resize(static_cast<std::size_t>(desc.width) * static_cast<std::size_t>(desc.height) *
                              rhi::texture_format_size(desc.format));
        if (desc.initial_data) {
            std::memcpy(texture.texels.data(), desc.initial_data, texture.texels.size());
        }
        texture.desc.initial_data = nullptr;
        texture.desc.debug_name = {};
        textures[next] = std::move(texture);
        return {next++};
    }
    void update_texture(rhi::TextureHandle t, rhi::Rect region, std::span<const std::byte> data) override {
        ++texture_updates;
        Texture& texture = textures.at(t.id);
        const std::size_t texel = rhi::texture_format_size(texture.desc.format);
        REQUIRE(data.size() == static_cast<std::size_t>(region.width) * static_cast<std::size_t>(region.height) * texel);
        for (std::int32_t row = 0; row < region.height; ++row) {
            const std::size_t dst = (static_cast<std::size_t>(region.y + row) * static_cast<std::size_t>(texture.desc.width) +
                                     static_cast<std::size_t>(region.x)) * texel;
            std::memcpy(texture.texels.data() + dst, data.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(region.width) * texel,
                        static_cast<std::size_t>(region.width) * texel);
        }
    }
    void destroy_texture(rhi::TextureHandle t) override { textures.erase(t.id); }

    rhi::PipelineHandle create_pipeline(const rhi::PipelineDesc& desc) override {
        // The desc's spans only live for the call: copy what the tests check.
        Pipeline p;
        p.blend = desc.blend;
        p.has_spirv = !desc.vertex.spirv.empty() && !desc.fragment.spirv.empty();
        p.has_es300 = !desc.vertex.glsl_es300.empty() && !desc.fragment.glsl_es300.empty();
        for (const rhi::VertexStream& s : desc.streams) {
            p.streams.push_back({s.stride, s.rate, s.attributes.size()});
        }
        for (const rhi::TextureBinding& t : desc.fragment.textures) {
            p.textures.emplace_back(t.name);
        }
        pipelines.push_back(p);
        return {next++};
    }
    void destroy_pipeline(rhi::PipelineHandle) override {}

    void set_viewport(rhi::Rect) override {}
    void set_scissor(rhi::Rect) override {}
    void bind_pipeline(rhi::PipelineHandle p) override { pipeline_ = p.id; }
    void bind_vertex_buffer(std::uint32_t stream, rhi::BufferHandle b, std::size_t) override {
        vertex_[stream] = b.id;
    }
    void bind_index_buffer(rhi::BufferHandle, rhi::IndexFormat) override {}
    void bind_uniform_buffer(std::uint32_t, rhi::BufferHandle b, std::size_t, std::size_t) override {
        uniform_ = b.id;
    }
    void bind_texture(std::uint32_t binding, rhi::TextureHandle t) override { textures_bound_[binding] = t.id; }
    void draw(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override { FAIL("unexpected draw()"); }
    void draw_indexed(std::uint32_t count, std::uint32_t instances, std::uint32_t first,
                      std::uint32_t first_instance) override {
        draws.push_back({count, instances, first, first_instance, pipeline_, vertex_, uniform_, textures_bound_});
    }
    void draw_indexed_base_vertex(std::uint32_t count, std::uint32_t instances, std::uint32_t first, std::int32_t,
                                  std::uint32_t first_instance) override {
        draws.push_back({count, instances, first, first_instance, pipeline_, vertex_, uniform_, textures_bound_});
    }
    bool capture_frame(std::string) override { return false; }

private:
    rhi::DeviceInfo info_;
    std::uint32_t pipeline_ = 0;
    std::array<std::uint32_t, rhi::max_vertex_streams> vertex_{};
    std::uint32_t uniform_ = 0;
    std::array<std::uint32_t, rhi::max_texture_bindings> textures_bound_{};
};

} // namespace cuervo::test
