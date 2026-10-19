#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "cuervo/math/affine2.hpp"
#include "cuervo/rhi/rhi.hpp"
#include "cuervo/vector/mesh.hpp"

// Vector mesh renderer (DESIGN.md 4.4 "SVG art"). Meshes (a whole SVG
// document merged into one vector::VectorMesh) are uploaded once into static
// GPU buffers; each frame the caller submits instances (mesh + transform +
// tint) in paint order, and flush() draws them:
//   - one instanced draw per run of consecutive instances of the same
//     single-batch mesh (the common case: N copies of a sprite = 1 draw);
//   - a mesh with several material batches (more gradients than one 16 KiB
//     paint window holds) draws per instance, batch by batch, to keep
//     painter's order between overlapping instances.
// Per-vertex color or gradient paint is evaluated in mesh.frag; edges are
// anti-aliased by the device's MSAA (rhi::DeviceOptions::msaa_samples).
//
// Same conventions as Camera2D: world units, y down, radians clockwise.
namespace cuervo::render {

struct MeshHandle {
    std::uint32_t id = 0;
    explicit operator bool() const { return id != 0; }
    friend bool operator==(MeshHandle, MeshHandle) = default;
};

// GPU instance layout; must match mesh.vert (stream 1).
struct MeshInstance {
    float row0[3]; // a, c, tx
    float row1[3]; // b, d, ty
    std::uint32_t tint = 0xFFFFFFFFu; // RGBA8 straight alpha
};
static_assert(sizeof(MeshInstance) == 28);

// std140 block `Globals` of mesh.vert.
struct MeshGlobals {
    float row0[4];
    float row1[4];
};
static_assert(sizeof(MeshGlobals) == 32);

MeshInstance make_mesh_instance(const Affine2& transform, rhi::Color tint);

class MeshBatcher {
public:
    struct DrawCall {
        std::uint32_t mesh = 0;
        std::uint32_t batch = 0;
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t first_instance = 0;
        std::uint32_t instance_count = 0;
    };

    MeshBatcher() = default;
    ~MeshBatcher();
    MeshBatcher(const MeshBatcher&) = delete;
    MeshBatcher& operator=(const MeshBatcher&) = delete;

    // Without init() the batcher still records meshes and plans draw calls
    // (for tests) but uploads and draws nothing.
    bool init(rhi::Device& device);
    void shutdown();

    MeshHandle create_mesh(const vector::VectorMesh& mesh);
    // Replaces a mesh's contents (e.g. a new zoom level or an edited path).
    // Old GPU buffers are freed by the RHI once frames in flight are done.
    void update_mesh(MeshHandle handle, const vector::VectorMesh& mesh);
    void destroy_mesh(MeshHandle handle);

    void clear() { instances_.clear(); meshes_of_instances_.clear(); }
    void draw(MeshHandle mesh, const Affine2& transform, rhi::Color tint = {1.0f, 1.0f, 1.0f, 1.0f});

    // The draw calls flush() would issue for the current instances.
    std::vector<DrawCall> plan() const;

    // Uploads the instances and draws them into the current viewport. Call
    // between begin_frame and end_frame. Instances are kept; clear() first
    // when building the next frame.
    void flush(const Affine2& world_to_ndc);

    std::size_t last_draw_calls() const { return last_draw_calls_; }
    std::size_t instance_count() const { return instances_.size(); }

private:
    struct GpuMesh {
        rhi::BufferHandle vertices;
        rhi::BufferHandle indices;
        rhi::BufferHandle paints; // one paint window per batch, at batch * paint_stride_
        std::vector<vector::MeshBatch> batches; // paints dropped after upload; ranges kept
        bool has_paints = false;
    };

    void upload(GpuMesh& gpu, const vector::VectorMesh& mesh);
    void release(GpuMesh& gpu);
    bool ensure_instance_capacity(std::size_t count);

    rhi::Device* device_ = nullptr;
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle globals_;
    rhi::BufferHandle instance_buffer_;
    rhi::BufferHandle empty_paints_; // bound for batches without gradients
    std::size_t instance_capacity_ = 0;
    std::size_t paint_stride_ = 16384;
    std::unordered_map<std::uint32_t, GpuMesh> meshes_;
    std::uint32_t next_mesh_ = 1;
    std::vector<MeshInstance> instances_;
    std::vector<std::uint32_t> meshes_of_instances_;
    std::size_t last_draw_calls_ = 0;
};

} // namespace cuervo::render
