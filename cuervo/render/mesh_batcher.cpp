#include "cuervo/render/mesh_batcher.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "cuervo/core/log.hpp"
#include "cuervo/render/sdf_batcher.hpp" // pack_rgba8
#include "shaders/mesh_frag.hpp"
#include "shaders/mesh_vert.hpp"

namespace cuervo::render {

namespace {

constexpr std::size_t paint_window_bytes = vector::paint_window_vec4 * 16;

static_assert(offsetof(vector::MeshVertex, position) == 0);
static_assert(offsetof(vector::MeshVertex, color) == 8);
static_assert(offsetof(vector::MeshVertex, paint) == 12);
static_assert(offsetof(MeshInstance, row1) == 12);
static_assert(offsetof(MeshInstance, tint) == 24);

constexpr std::array<rhi::VertexAttribute, 3> vertex_attributes{{
    {0, rhi::VertexFormat::Float2, offsetof(vector::MeshVertex, position)},
    {1, rhi::VertexFormat::UByte4Norm, offsetof(vector::MeshVertex, color)},
    {2, rhi::VertexFormat::UInt1, offsetof(vector::MeshVertex, paint)},
}};
constexpr std::array<rhi::VertexAttribute, 3> instance_attributes{{
    {3, rhi::VertexFormat::Float3, offsetof(MeshInstance, row0)},
    {4, rhi::VertexFormat::Float3, offsetof(MeshInstance, row1)},
    {5, rhi::VertexFormat::UByte4Norm, offsetof(MeshInstance, tint)},
}};

} // namespace

MeshInstance make_mesh_instance(const Affine2& m, rhi::Color tint) {
    return {{m.a, m.c, m.tx}, {m.b, m.d, m.ty}, pack_rgba8(tint)};
}

MeshBatcher::~MeshBatcher() { shutdown(); }

bool MeshBatcher::init(rhi::Device& device) {
    shutdown();
    device_ = &device;
    const std::size_t alignment = std::max<std::size_t>(device.info().uniform_offset_alignment, 1);
    paint_stride_ = (paint_window_bytes + alignment - 1) / alignment * alignment;

    const std::array<rhi::VertexStream, 2> streams{{
        {sizeof(vector::MeshVertex), rhi::VertexRate::PerVertex, vertex_attributes},
        {sizeof(MeshInstance), rhi::VertexRate::PerInstance, instance_attributes},
    }};
    rhi::PipelineDesc desc;
    desc.vertex = shaders::mesh_vert;
    desc.fragment = shaders::mesh_frag;
    desc.streams = streams;
    desc.blend = rhi::BlendMode::Premultiplied;
    desc.debug_name = "vector_mesh";
    pipeline_ = device.create_pipeline(desc);
    globals_ = device.create_buffer({rhi::BufferKind::Uniform, rhi::BufferUsage::Dynamic, sizeof(MeshGlobals), nullptr});
    const std::vector<std::byte> zeros(paint_window_bytes);
    empty_paints_ =
        device.create_buffer({rhi::BufferKind::Uniform, rhi::BufferUsage::Static, paint_window_bytes, zeros.data()});
    if (!pipeline_ || !globals_ || !empty_paints_ || !ensure_instance_capacity(256)) {
        LOG_ERROR("MeshBatcher: failed to create GPU resources");
        shutdown();
        return false;
    }
    return true;
}

void MeshBatcher::shutdown() {
    for (auto& [id, gpu] : meshes_) {
        release(gpu);
    }
    if (device_) {
        device_->destroy_pipeline(pipeline_);
        device_->destroy_buffer(globals_);
        device_->destroy_buffer(instance_buffer_);
        device_->destroy_buffer(empty_paints_);
    }
    pipeline_ = {};
    globals_ = instance_buffer_ = empty_paints_ = {};
    instance_capacity_ = 0;
    device_ = nullptr;
}

void MeshBatcher::release(GpuMesh& gpu) {
    if (device_) {
        device_->destroy_buffer(gpu.vertices);
        device_->destroy_buffer(gpu.indices);
        device_->destroy_buffer(gpu.paints);
    }
    gpu.vertices = gpu.indices = gpu.paints = {};
}

void MeshBatcher::upload(GpuMesh& gpu, const vector::VectorMesh& mesh) {
    release(gpu);
    gpu.batches.clear();
    gpu.has_paints = false;
    for (const vector::MeshBatch& batch : mesh.batches) {
        if (batch.index_count == 0) {
            continue;
        }
        gpu.batches.push_back({batch.first_index, batch.index_count, {}});
        gpu.has_paints = gpu.has_paints || !batch.paints.empty();
    }
    if (!device_ || mesh.indices.empty()) {
        return;
    }
    gpu.vertices = device_->create_buffer({rhi::BufferKind::Vertex, rhi::BufferUsage::Static,
                                           mesh.vertices.size() * sizeof(vector::MeshVertex), mesh.vertices.data()});
    gpu.indices = device_->create_buffer({rhi::BufferKind::Index, rhi::BufferUsage::Static,
                                          mesh.indices.size() * sizeof(std::uint32_t), mesh.indices.data()});
    if (gpu.has_paints) {
        // Each batch's window sits at an aligned offset and is a full 16 KiB:
        // the bound range must cover the shader's whole uniform block.
        std::vector<std::byte> paints(paint_stride_ * gpu.batches.size());
        std::size_t slot = 0;
        for (const vector::MeshBatch& batch : mesh.batches) {
            if (batch.index_count == 0) {
                continue;
            }
            std::memcpy(paints.data() + slot * paint_stride_, batch.paints.data(),
                        std::min(batch.paints.size() * 16, paint_window_bytes));
            ++slot;
        }
        gpu.paints = device_->create_buffer(
            {rhi::BufferKind::Uniform, rhi::BufferUsage::Static, paints.size(), paints.data()});
    }
}

MeshHandle MeshBatcher::create_mesh(const vector::VectorMesh& mesh) {
    MeshHandle handle{next_mesh_++};
    upload(meshes_[handle.id], mesh);
    return handle;
}

void MeshBatcher::update_mesh(MeshHandle handle, const vector::VectorMesh& mesh) {
    auto it = meshes_.find(handle.id);
    if (it == meshes_.end()) {
        LOG_ERROR("MeshBatcher::update_mesh: invalid mesh {}", handle.id);
        return;
    }
    upload(it->second, mesh);
}

void MeshBatcher::destroy_mesh(MeshHandle handle) {
    if (auto it = meshes_.find(handle.id); it != meshes_.end()) {
        release(it->second);
        meshes_.erase(it);
    }
}

void MeshBatcher::draw(MeshHandle mesh, const Affine2& transform, rhi::Color tint) {
    if (!meshes_.contains(mesh.id)) {
        return;
    }
    instances_.push_back(make_mesh_instance(transform, tint));
    meshes_of_instances_.push_back(mesh.id);
}

std::vector<MeshBatcher::DrawCall> MeshBatcher::plan() const {
    std::vector<DrawCall> calls;
    std::size_t i = 0;
    while (i < meshes_of_instances_.size()) {
        const std::uint32_t id = meshes_of_instances_[i];
        const GpuMesh& gpu = meshes_.at(id);
        std::size_t run = 1;
        if (gpu.batches.size() == 1) {
            // Same single-batch mesh drawn back to back: one instanced draw.
            while (i + run < meshes_of_instances_.size() && meshes_of_instances_[i + run] == id) {
                ++run;
            }
        }
        for (std::uint32_t b = 0; b < gpu.batches.size(); ++b) {
            calls.push_back({id, b, gpu.batches[b].first_index, gpu.batches[b].index_count,
                             static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(run)});
        }
        i += run;
    }
    return calls;
}

bool MeshBatcher::ensure_instance_capacity(std::size_t count) {
    if (instance_buffer_ && count <= instance_capacity_) {
        return true;
    }
    const std::size_t capacity = std::max({count, instance_capacity_ * 2, std::size_t{256}});
    device_->destroy_buffer(instance_buffer_);
    instance_buffer_ = device_->create_buffer(
        {rhi::BufferKind::Vertex, rhi::BufferUsage::Stream, capacity * sizeof(MeshInstance), nullptr});
    instance_capacity_ = instance_buffer_ ? capacity : 0;
    return static_cast<bool>(instance_buffer_);
}

void MeshBatcher::flush(const Affine2& m) {
    last_draw_calls_ = 0;
    if (!device_ || instances_.empty() || !ensure_instance_capacity(instances_.size())) {
        return;
    }
    const MeshGlobals globals{{m.a, m.c, m.tx, 0.0f}, {m.b, m.d, m.ty, 0.0f}};
    device_->update_buffer(globals_, 0, std::as_bytes(std::span(&globals, 1)));
    device_->update_buffer(instance_buffer_, 0, std::as_bytes(std::span(instances_)));

    device_->bind_pipeline(pipeline_);
    device_->bind_vertex_buffer(1, instance_buffer_);
    device_->bind_uniform_buffer(0, globals_);
    std::uint32_t bound_mesh = 0;
    for (const DrawCall& call : plan()) {
        const GpuMesh& gpu = meshes_.at(call.mesh);
        if (!gpu.vertices || !gpu.indices) {
            continue;
        }
        if (bound_mesh != call.mesh) {
            device_->bind_vertex_buffer(0, gpu.vertices);
            device_->bind_index_buffer(gpu.indices, rhi::IndexFormat::UInt32);
            bound_mesh = call.mesh;
        }
        if (gpu.has_paints) {
            device_->bind_uniform_buffer(1, gpu.paints, call.batch * paint_stride_, paint_window_bytes);
        } else {
            device_->bind_uniform_buffer(1, empty_paints_, 0, paint_window_bytes);
        }
        device_->draw_indexed(call.index_count, call.instance_count, call.first_index, call.first_instance);
        ++last_draw_calls_;
    }
}

} // namespace cuervo::render
