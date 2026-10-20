#include "cuervo/rhi/vulkan/vk_device.hpp"

#include <SDL3/SDL_vulkan.h>
#include <volk.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cuervo/core/log.hpp"
#include "cuervo/platform/window.hpp"
#include "cuervo/rhi/rhi_internal.hpp"

namespace cuervo::vk {

namespace {

constexpr std::uint32_t frames_in_flight = 2;
constexpr std::uint32_t required_api = VK_API_VERSION_1_3;
constexpr const char* validation_layer = "VK_LAYER_KHRONOS_validation";

#ifdef NDEBUG
constexpr bool want_validation = false;
#else
constexpr bool want_validation = true;
#endif

std::string string_VkResult(VkResult result) {
    switch (result) {
#define CUERVO_VK_RESULT(name) case name: return #name;
        CUERVO_VK_RESULT(VK_SUCCESS)
        CUERVO_VK_RESULT(VK_NOT_READY)
        CUERVO_VK_RESULT(VK_TIMEOUT)
        CUERVO_VK_RESULT(VK_INCOMPLETE)
        CUERVO_VK_RESULT(VK_SUBOPTIMAL_KHR)
        CUERVO_VK_RESULT(VK_ERROR_OUT_OF_HOST_MEMORY)
        CUERVO_VK_RESULT(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        CUERVO_VK_RESULT(VK_ERROR_INITIALIZATION_FAILED)
        CUERVO_VK_RESULT(VK_ERROR_DEVICE_LOST)
        CUERVO_VK_RESULT(VK_ERROR_MEMORY_MAP_FAILED)
        CUERVO_VK_RESULT(VK_ERROR_LAYER_NOT_PRESENT)
        CUERVO_VK_RESULT(VK_ERROR_EXTENSION_NOT_PRESENT)
        CUERVO_VK_RESULT(VK_ERROR_FEATURE_NOT_PRESENT)
        CUERVO_VK_RESULT(VK_ERROR_INCOMPATIBLE_DRIVER)
        CUERVO_VK_RESULT(VK_ERROR_SURFACE_LOST_KHR)
        CUERVO_VK_RESULT(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
        CUERVO_VK_RESULT(VK_ERROR_OUT_OF_DATE_KHR)
#undef CUERVO_VK_RESULT
        default: return std::format("VkResult({})", static_cast<int>(result));
    }
}

// Logs and bails out of a bool-returning function.
#define VK_TRY(expr)                                                    \
    do {                                                                \
        if (VkResult vk_try_result = (expr); vk_try_result != VK_SUCCESS) { \
            LOG_ERROR("{} failed: {}", #expr, string_VkResult(vk_try_result)); \
            return false;                                               \
        }                                                               \
    } while (0)

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        LOG_ERROR("[vulkan] {}", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG_WARN("[vulkan] {}", data->pMessage);
    } else {
        LOG_DEBUG("[vulkan] {}", data->pMessage);
    }
    return VK_FALSE;
}

std::string version_string(std::uint32_t version) {
    return std::format("Vulkan {}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                       VK_API_VERSION_PATCH(version));
}

bool has_layer(const char* name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::ranges::any_of(layers, [&](const auto& l) { return std::strcmp(l.layerName, name) == 0; });
}

bool has_instance_extension(const char* name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
    return std::ranges::any_of(extensions,
                               [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
}

bool has_device_extension(VkPhysicalDevice gpu, const char* name) {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, extensions.data());
    return std::ranges::any_of(extensions,
                               [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
}

VkBufferUsageFlags to_vk_usage(rhi::BufferKind kind) {
    switch (kind) {
        case rhi::BufferKind::Vertex:  return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        case rhi::BufferKind::Index:   return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        case rhi::BufferKind::Uniform: return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                   VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

VkFormat to_vk_format(rhi::VertexFormat format) {
    switch (format) {
        case rhi::VertexFormat::Float1:     return VK_FORMAT_R32_SFLOAT;
        case rhi::VertexFormat::Float2:     return VK_FORMAT_R32G32_SFLOAT;
        case rhi::VertexFormat::Float3:     return VK_FORMAT_R32G32B32_SFLOAT;
        case rhi::VertexFormat::Float4:     return VK_FORMAT_R32G32B32A32_SFLOAT;
        case rhi::VertexFormat::UByte4Norm: return VK_FORMAT_R8G8B8A8_UNORM;
        case rhi::VertexFormat::UInt1:      return VK_FORMAT_R32_UINT;
    }
    return VK_FORMAT_R32G32B32A32_SFLOAT;
}

VkPrimitiveTopology to_vk_topology(rhi::Topology topology) {
    switch (topology) {
        case rhi::Topology::TriangleList:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case rhi::Topology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case rhi::Topology::LineList:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case rhi::Topology::LineStrip:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case rhi::Topology::PointList:     return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

// Same factors as the GL backend (gl_device.cpp bind_pipeline).
VkPipelineColorBlendAttachmentState to_vk_blend(rhi::BlendMode mode) {
    VkPipelineColorBlendAttachmentState state{};
    state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    switch (mode) {
        case rhi::BlendMode::None:
            state.blendEnable = VK_FALSE;
            break;
        case rhi::BlendMode::Alpha:
            state.blendEnable = VK_TRUE;
            state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case rhi::BlendMode::Premultiplied:
            state.blendEnable = VK_TRUE;
            state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case rhi::BlendMode::Additive:
            state.blendEnable = VK_TRUE;
            state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
    }
    return state;
}

// One VkBuffer + its memory. `last_used` is the submission serial of the last
// frame that bound it; the GPU is done with it once that serial completes.
struct Allocation {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
    std::uint64_t last_used = 0;
};

// Static buffers have one version. Dynamic/Stream buffers are renamed: an
// update that would overwrite a version the GPU may still read switches to a
// free version (allocating one if needed), so updates never race the GPU.
// Steady state is frames_in_flight + 1 versions per buffer updated every frame.
struct Buffer {
    rhi::BufferKind kind = rhi::BufferKind::Vertex;
    rhi::BufferUsage usage = rhi::BufferUsage::Static;
    std::size_t size = 0;
    std::vector<Allocation> versions;
    std::uint32_t current = 0;
    // CPU copy of the contents (Dynamic only): copied forward whole on rename,
    // so it never reads back mapped memory (uncached/write-combined: reading
    // 2 MB of it cost ~100 ms a frame). Stream buffers keep none: at 76 MB a
    // frame the extra copy cost 4-7 ms.
    std::vector<std::byte> shadow;
    // Stream only: serial of the frame that last wrote `current`, and the byte
    // range written in that frame. Renaming a version written earlier in the
    // same frame copies that range forward, so a partial update after a draw
    // keeps that frame's other bytes (as GL does).
    std::uint64_t written = 0;
    std::size_t written_begin = 0;
    std::size_t written_end = 0;
};

struct Pipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::uint32_t stream_count = 0;
};

// Device-local sampled image. `initialized` turns true once an upload or clear
// has been recorded, after which the image lives in SHADER_READ_ONLY_OPTIMAL
// between transfers.
struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::int32_t width = 0;
    std::int32_t height = 0;
    rhi::TextureFormat format = rhi::TextureFormat::RGBA8;
    rhi::Filter filter = rhi::Filter::Linear;
    bool initialized = false;
    std::uint64_t last_used = 0; // submission serial of the last frame that sampled it
};

// A texture write waiting for the next submitted frame: a copy from a staging
// buffer, or (no staging buffer) a clear to transparent black.
struct PendingUpload {
    std::uint32_t texture = 0;
    rhi::Rect region{};
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation staging_allocation = VK_NULL_HANDLE;
};

// A resource the GPU may still be using; freed once submission `serial` has completed.
struct PendingDelete {
    std::uint64_t serial = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE; // of `buffer` or `image`
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

VkFormat to_vk_format(rhi::TextureFormat format) {
    switch (format) {
        case rhi::TextureFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
        case rhi::TextureFormat::R8:    return VK_FORMAT_R8_UNORM;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

// Stages that may sample a texture (both shader stages see set 1).
constexpr VkPipelineStageFlags2 texture_read_stages =
    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    // Texture uploads queued since the last submission, recorded at end_frame
    // and submitted ahead of `cmd` in the same vkQueueSubmit2.
    VkCommandBuffer upload_cmd = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
    std::uint64_t serial = 0; // submission serial last recorded in this slot
    // Uniform-buffer descriptor sets are allocated per draw from these and
    // recycled wholesale when the slot comes around again.
    std::vector<VkDescriptorPool> descriptor_pools;
    std::size_t descriptor_pool_index = 0;
};

struct UniformBinding {
    std::uint32_t buffer = 0;
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct VertexBinding {
    std::uint32_t buffer = 0;
    std::size_t offset = 0;
};

// The exact descriptors last written, to skip redundant descriptor sets.
struct ResolvedUniform {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize range = 0;
    friend bool operator==(const ResolvedUniform&, const ResolvedUniform&) = default;
};

struct ResolvedTexture {
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    friend bool operator==(const ResolvedTexture&, const ResolvedTexture&) = default;
};

constexpr std::uint32_t descriptor_sets_per_pool = 256;

class VulkanDevice final : public rhi::Device {
public:
    VulkanDevice(SDL_Window* window, const rhi::DeviceOptions& options) : window_(window), options_(options) {}

    ~VulkanDevice() override {
        if (device_) {
            vkDeviceWaitIdle(device_);
            for (auto& [id, buffer] : buffers_) {
                for (Allocation& version : buffer.versions) {
                    vmaDestroyBuffer(allocator_, version.buffer, version.allocation);
                }
            }
            for (auto& [id, pipeline] : pipelines_) {
                vkDestroyPipeline(device_, pipeline.pipeline, nullptr);
            }
            for (auto& [id, texture] : textures_) {
                vkDestroyImageView(device_, texture.view, nullptr);
                vmaDestroyImage(allocator_, texture.image, texture.allocation);
            }
            for (PendingUpload& upload : pending_uploads_) {
                if (upload.staging) {
                    vmaDestroyBuffer(allocator_, upload.staging, upload.staging_allocation);
                }
            }
            completed_serial_ = UINT64_MAX;
            collect_garbage();
            for (VkSampler sampler : samplers_) {
                vkDestroySampler(device_, sampler, nullptr);
            }
            vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
            vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
            vkDestroyDescriptorSetLayout(device_, texture_set_layout_, nullptr);
            destroy_swapchain(swapchain_);
            for (Frame& frame : frames_) {
                for (VkDescriptorPool pool : frame.descriptor_pools) {
                    vkDestroyDescriptorPool(device_, pool, nullptr);
                }
                vkDestroyFence(device_, frame.in_flight, nullptr);
                vkDestroySemaphore(device_, frame.image_available, nullptr);
                vkDestroyCommandPool(device_, frame.pool, nullptr);
            }
            if (allocator_) {
                vmaDestroyAllocator(allocator_);
            }
            vkDestroyDevice(device_, nullptr);
        }
        if (surface_) {
            SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
        }
        if (messenger_) {
            vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        }
        if (instance_) {
            vkDestroyInstance(instance_, nullptr);
        }
    }

    bool init() {
        return create_instance() && create_surface() && pick_gpu() && create_device() &&
               choose_surface_format() && create_allocator() && create_frames() && create_layouts();
    }

    const rhi::DeviceInfo& info() const override { return info_; }

    void begin_frame(ivec2 framebuffer_size) override {
        if (frame_active_) {
            LOG_WARN("begin_frame called twice without end_frame");
            return;
        }
        skip_frame_ = true;

        const VkExtent2D wanted{static_cast<std::uint32_t>(std::max(framebuffer_size.x(), 0)),
                                static_cast<std::uint32_t>(std::max(framebuffer_size.y(), 0))};
        if (wanted.width == 0 || wanted.height == 0) {
            return; // minimized
        }
        if (swapchain_dirty_ || !swapchain_.handle || wanted.width != swapchain_.extent.width ||
            wanted.height != swapchain_.extent.height) {
            if (!recreate_swapchain(wanted)) {
                return;
            }
        }

        Frame& frame = frames_[frame_index_];
        vkWaitForFences(device_, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);
        completed_serial_ = std::max(completed_serial_, frame.serial);
        collect_garbage();
        for (VkDescriptorPool pool : frame.descriptor_pools) {
            vkResetDescriptorPool(device_, pool, 0);
        }
        frame.descriptor_pool_index = 0;

        VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_.handle, UINT64_MAX, frame.image_available,
                                                  VK_NULL_HANDLE, &image_index_);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchain_dirty_ = true;
            return;
        }
        if (acquired == VK_SUBOPTIMAL_KHR) {
            swapchain_dirty_ = true; // still usable this frame
        } else if (acquired != VK_SUCCESS) {
            LOG_ERROR("vkAcquireNextImageKHR failed: {}", string_VkResult(acquired));
            return;
        }

        vkResetFences(device_, 1, &frame.in_flight);
        vkResetCommandBuffer(frame.cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(frame.cmd, &begin);

        image_barrier(frame.cmd, swapchain_.images[image_index_], VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        if (swapchain_.msaa_image) {
            // One MSAA image serves every frame in flight: wait for the previous
            // frame's attachment writes (same queue) before overwriting it.
            image_barrier(frame.cmd, swapchain_.msaa_image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
        }

        skip_frame_ = false;
        frame_active_ = true;
        rendering_ = false;
        pending_clear_.reset();

        // Reset draw state (rhi.hpp: state resets at every begin_frame).
        current_pipeline_ = 0;
        bound_pipeline_ = VK_NULL_HANDLE;
        vertex_bindings_ = {};
        index_buffer_ = 0;
        uniform_bindings_ = {};
        written_uniforms_ = {};
        texture_bindings_ = {};
        written_textures_ = {};
        const rhi::Rect full = full_rect();
        set_viewport(full);
        set_scissor(full);
    }

    void clear(rhi::Color color) override {
        if (!frame_active_) {
            return;
        }
        if (!rendering_ && scissor_ == full_rect()) {
            // Folded into the render pass's load op: free on tiled GPUs.
            pending_clear_ = color;
            return;
        }
        begin_rendering();
        VkClearAttachment attachment{};
        attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachment.colorAttachment = 0;
        attachment.clearValue.color = {{color.r, color.g, color.b, color.a}};
        const VkRect2D area = clamped_scissor();
        if (area.extent.width == 0 || area.extent.height == 0) {
            return;
        }
        VkClearRect rect{area, 0, 1};
        vkCmdClearAttachments(frames_[frame_index_].cmd, 1, &attachment, 1, &rect);
    }

    void end_frame() override {
        if (skip_frame_ || !frame_active_) {
            skip_frame_ = false;
            capture_path_.clear();
            return;
        }
        Frame& frame = frames_[frame_index_];
        begin_rendering(); // applies a pending clear even if nothing was drawn
        vkCmdEndRendering(frame.cmd);
        rendering_ = false;

        Allocation readback{};
        const bool capture = !capture_path_.empty() && record_readback(frame.cmd, readback);
        const VkImageLayout layout = capture ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                             : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        const VkPipelineStageFlags2 stage = capture ? VK_PIPELINE_STAGE_2_COPY_BIT
                                                    : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        const VkAccessFlags2 access = capture ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        image_barrier(frame.cmd, swapchain_.images[image_index_], layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, stage,
                      access, VK_PIPELINE_STAGE_2_NONE, 0);
        vkEndCommandBuffer(frame.cmd);

        // One "render finished" semaphore per swapchain image: the presentation
        // engine may still hold the previous one when this frame slot comes around.
        VkSemaphore render_finished = swapchain_.render_finished[image_index_];

        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = frame.image_available;
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = render_finished;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        // Queued texture uploads run first, in the same submission.
        std::array<VkCommandBufferSubmitInfo, 2> cmd_infos{};
        std::uint32_t cmd_count = 0;
        if (record_uploads(frame.upload_cmd)) {
            cmd_infos[cmd_count] = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            cmd_infos[cmd_count++].commandBuffer = frame.upload_cmd;
        }
        cmd_infos[cmd_count] = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cmd_infos[cmd_count++].commandBuffer = frame.cmd;

        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = cmd_count;
        submit.pCommandBufferInfos = cmd_infos.data();
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        if (VkResult r = vkQueueSubmit2(queue_, 1, &submit, frame.in_flight); r != VK_SUCCESS) {
            LOG_ERROR("vkQueueSubmit2 failed: {}", string_VkResult(r));
        }
        frame.serial = ++submitted_serial_;
        for (const PendingDelete& staging : recorded_staging_) {
            pending_deletes_.push_back({frame.serial, staging.buffer, staging.allocation, VK_NULL_HANDLE,
                                        VK_NULL_HANDLE, VK_NULL_HANDLE});
        }
        recorded_staging_.clear();

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_finished;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_.handle;
        present.pImageIndices = &image_index_;
        VkResult presented = vkQueuePresentKHR(queue_, &present);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
            swapchain_dirty_ = true;
        } else if (presented != VK_SUCCESS) {
            LOG_ERROR("vkQueuePresentKHR failed: {}", string_VkResult(presented));
        }

        if (capture) {
            finish_readback(readback);
        }
        capture_path_.clear();

        frame_index_ = (frame_index_ + 1) % frames_in_flight;
        frame_active_ = false;
    }

    ivec2 framebuffer_size() const override {
        return {static_cast<std::int32_t>(swapchain_.extent.width), static_cast<std::int32_t>(swapchain_.extent.height)};
    }

    // TODO(phase 2b): Static buffers should go through a staging copy into
    // device-local memory. For now every buffer is host-visible and persistently
    // mapped (ReBAR/UMA make this fast anyway).
    rhi::BufferHandle create_buffer(const rhi::BufferDesc& desc) override {
        Buffer buffer;
        buffer.kind = desc.kind;
        buffer.usage = desc.usage;
        buffer.size = desc.size;
        Allocation first;
        if (!allocate(buffer, first)) {
            return {};
        }
        buffer.versions.push_back(first);
        if (desc.usage == rhi::BufferUsage::Dynamic) {
            buffer.shadow.resize(desc.size);
        }
        if (desc.initial_data && desc.size > 0) {
            std::memcpy(first.mapped, desc.initial_data, desc.size);
            vmaFlushAllocation(allocator_, first.allocation, 0, desc.size);
            if (!buffer.shadow.empty()) {
                std::memcpy(buffer.shadow.data(), desc.initial_data, desc.size);
            }
        }

        rhi::BufferHandle handle{next_id_++};
        buffers_.emplace(handle.id, std::move(buffer));
        return handle;
    }

    void update_buffer(rhi::BufferHandle handle, std::size_t offset, std::span<const std::byte> data) override {
        auto it = buffers_.find(handle.id);
        if (it == buffers_.end()) {
            LOG_ERROR("update_buffer: invalid buffer handle {}", handle.id);
            return;
        }
        Buffer& buffer = it->second;
        if (offset > buffer.size || data.size() > buffer.size - offset) {
            LOG_ERROR("update_buffer: {} bytes at offset {} overflows a {}-byte buffer", data.size(), offset,
                      buffer.size);
            return;
        }
        if (data.empty()) {
            return;
        }
        const std::uint64_t recording = submitted_serial_ + 1;
        // A Stream update that lands beside what this frame already wrote (a
        // batcher's next flush range) cannot touch bytes a recorded draw reads,
        // and the version is already this frame's: write in place instead of
        // renaming, which would cost another full-size allocation and copy
        // (DESIGN.md 4.4: Stream contract).
        const bool beside_written = buffer.usage == rhi::BufferUsage::Stream && buffer.written == recording &&
                                    (offset >= buffer.written_end || offset + data.size() <= buffer.written_begin);
        if (buffer.usage != rhi::BufferUsage::Static && !beside_written &&
            buffer.versions[buffer.current].last_used > completed_serial_) {
            // The GPU may still read the current version (in flight, or bound by
            // a draw already recorded this frame): rename.
            const std::uint32_t previous = buffer.current;
            if (!rename(buffer)) {
                return;
            }
            const bool covers_written = offset <= buffer.written_begin && offset + data.size() >= buffer.written_end;
            if (buffer.usage == rhi::BufferUsage::Stream && buffer.written == recording && !covers_written &&
                buffer.written_end > buffer.written_begin) {
                // A second update in one frame that overlaps what this frame
                // already wrote (and does not replace all of it): keep the earlier
                // writes. Rare (batchers write beside, or over, their own range),
                // so it reads the old version's mapped memory instead of every
                // update paying for a CPU shadow copy of the whole buffer.
                Allocation& fresh = buffer.versions[buffer.current];
                const std::size_t begin = buffer.written_begin;
                const std::size_t count = buffer.written_end - begin;
                std::memcpy(static_cast<std::byte*>(fresh.mapped) + begin,
                            static_cast<const std::byte*>(buffer.versions[previous].mapped) + begin, count);
                vmaFlushAllocation(allocator_, fresh.allocation, begin, count);
            }
        }
        if (buffer.usage == rhi::BufferUsage::Stream) {
            const std::size_t end = offset + data.size();
            if (buffer.written != recording) {
                buffer.written_begin = offset;
                buffer.written_end = end;
            } else {
                buffer.written_begin = std::min(buffer.written_begin, offset);
                buffer.written_end = std::max(buffer.written_end, end);
            }
        }
        buffer.written = recording;
        if (!buffer.shadow.empty()) {
            std::memcpy(buffer.shadow.data() + offset, data.data(), data.size());
        }
        Allocation& version = buffer.versions[buffer.current];
        std::memcpy(static_cast<std::byte*>(version.mapped) + offset, data.data(), data.size());
        vmaFlushAllocation(allocator_, version.allocation, offset, data.size());
    }

    void destroy_buffer(rhi::BufferHandle handle) override {
        auto it = buffers_.find(handle.id);
        if (it == buffers_.end()) {
            return;
        }
        for (const Allocation& version : it->second.versions) {
            // The frame being recorded (or one in flight) may still use it.
            pending_deletes_.push_back({version.last_used, version.buffer, version.allocation, VK_NULL_HANDLE});
        }
        buffers_.erase(it);
        collect_garbage();
    }

    rhi::TextureHandle create_texture(const rhi::TextureDesc& desc) override {
        if (!rhi::detail::texture_size_allowed(info_, desc)) {
            return {};
        }
        Texture texture;
        texture.width = desc.width;
        texture.height = desc.height;
        texture.format = desc.format;
        texture.filter = desc.filter;

        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = to_vk_format(desc.format);
        image_info.extent = {static_cast<std::uint32_t>(desc.width), static_cast<std::uint32_t>(desc.height), 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (VkResult r = vmaCreateImage(allocator_, &image_info, &alloc_info, &texture.image, &texture.allocation,
                                        nullptr);
            r != VK_SUCCESS) {
            LOG_ERROR("create_texture '{}': vmaCreateImage failed: {}", desc.debug_name, string_VkResult(r));
            return {};
        }
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = texture.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = image_info.format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (VkResult r = vkCreateImageView(device_, &view_info, nullptr, &texture.view); r != VK_SUCCESS) {
            LOG_ERROR("create_texture '{}': vkCreateImageView failed: {}", desc.debug_name, string_VkResult(r));
            vmaDestroyImage(allocator_, texture.image, texture.allocation);
            return {};
        }

        rhi::TextureHandle handle{next_id_++};
        textures_.emplace(handle.id, texture);
        // Every texture gets an initial write (data or a clear) queued, so it is
        // in SHADER_READ_ONLY_OPTIMAL before any draw of the same submission samples it.
        const rhi::Rect full{0, 0, desc.width, desc.height};
        if (desc.initial_data) {
            const std::size_t size = static_cast<std::size_t>(desc.width) * static_cast<std::size_t>(desc.height) *
                                     rhi::texture_format_size(desc.format);
            queue_upload(handle.id, full, {static_cast<const std::byte*>(desc.initial_data), size});
        } else {
            pending_uploads_.push_back({handle.id, full, VK_NULL_HANDLE, VK_NULL_HANDLE});
        }
        return handle;
    }

    void update_texture(rhi::TextureHandle handle, rhi::Rect region, std::span<const std::byte> data) override {
        auto it = textures_.find(handle.id);
        if (it == textures_.end()) {
            LOG_ERROR("update_texture: invalid texture handle {}", handle.id);
            return;
        }
        const Texture& texture = it->second;
        if (!rhi::detail::valid_texture_update(texture.width, texture.height, texture.format, region, data.size())) {
            return;
        }
        if (region.width == 0 || region.height == 0) {
            return;
        }
        queue_upload(handle.id, region, data);
    }

    void destroy_texture(rhi::TextureHandle handle) override {
        auto it = textures_.find(handle.id);
        if (it == textures_.end()) {
            return;
        }
        // Uploads not recorded yet never reach the GPU: drop them now.
        std::erase_if(pending_uploads_, [&](const PendingUpload& upload) {
            if (upload.texture != handle.id) {
                return false;
            }
            if (upload.staging) {
                vmaDestroyBuffer(allocator_, upload.staging, upload.staging_allocation);
            }
            return true;
        });
        for (std::uint32_t& bound : texture_bindings_) {
            if (bound == handle.id) {
                bound = 0;
            }
        }
        const Texture& texture = it->second;
        pending_deletes_.push_back(
            {texture.last_used, VK_NULL_HANDLE, texture.allocation, VK_NULL_HANDLE, texture.image, texture.view});
        textures_.erase(it);
        collect_garbage();
    }

    rhi::PipelineHandle create_pipeline(const rhi::PipelineDesc& desc) override {
        if (desc.streams.size() > rhi::max_vertex_streams) {
            LOG_ERROR("Pipeline '{}': {} vertex streams (max {})", desc.debug_name, desc.streams.size(),
                      rhi::max_vertex_streams);
            return {};
        }
        if (desc.vertex.spirv.empty() || desc.fragment.spirv.empty()) {
            LOG_ERROR("Pipeline '{}': missing SPIR-V", desc.debug_name);
            return {};
        }
        VkShaderModule vs = create_shader_module(desc.vertex.spirv);
        VkShaderModule fs = create_shader_module(desc.fragment.spirv);

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;
        for (std::uint32_t i = 0; i < desc.streams.size(); ++i) {
            const rhi::VertexStream& stream = desc.streams[i];
            bindings.push_back({i, stream.stride,
                                stream.rate == rhi::VertexRate::PerInstance ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                                            : VK_VERTEX_INPUT_RATE_VERTEX});
            for (const rhi::VertexAttribute& attribute : stream.attributes) {
                attributes.push_back({attribute.location, i, to_vk_format(attribute.format), attribute.offset});
            }
        }
        VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertex_input.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
        vertex_input.pVertexBindingDescriptions = bindings.data();
        vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = to_vk_topology(desc.topology);

        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = samples_; // every pipeline matches the device's MSAA target

        const VkPipelineColorBlendAttachmentState blend_attachment = to_vk_blend(desc.blend);
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;

        const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;

        // Dynamic rendering: the pipeline only needs the attachment format.
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &surface_format_.format;

        VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        create.pNext = &rendering;
        create.stageCount = static_cast<std::uint32_t>(stages.size());
        create.pStages = stages.data();
        create.pVertexInputState = &vertex_input;
        create.pInputAssemblyState = &assembly;
        create.pViewportState = &viewport;
        create.pRasterizationState = &raster;
        create.pMultisampleState = &multisample;
        create.pColorBlendState = &blend;
        create.pDynamicState = &dynamic;
        create.layout = pipeline_layout_;

        Pipeline pipeline;
        pipeline.stream_count = static_cast<std::uint32_t>(desc.streams.size());
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        if (vs && fs) {
            result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline.pipeline);
        }
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        if (result != VK_SUCCESS) {
            LOG_ERROR("Pipeline '{}': vkCreateGraphicsPipelines failed: {}", desc.debug_name, string_VkResult(result));
            return {};
        }
        rhi::PipelineHandle handle{next_id_++};
        pipelines_.emplace(handle.id, pipeline);
        return handle;
    }

    void destroy_pipeline(rhi::PipelineHandle handle) override {
        auto it = pipelines_.find(handle.id);
        if (it == pipelines_.end()) {
            return;
        }
        if (current_pipeline_ == handle.id) {
            current_pipeline_ = 0;
        }
        pending_deletes_.push_back(
            {submitted_serial_ + (frame_active_ ? 1 : 0), VK_NULL_HANDLE, VK_NULL_HANDLE, it->second.pipeline});
        pipelines_.erase(it);
        collect_garbage();
    }

    void set_viewport(rhi::Rect r) override {
        if (!frame_active_) {
            return;
        }
        // Negative height (core since Vulkan 1.1) flips y so NDC +1 is the top,
        // like GL: one projection matrix renders identically on both backends.
        VkViewport viewport{static_cast<float>(r.x), static_cast<float>(r.y + r.height), static_cast<float>(r.width),
                            -static_cast<float>(r.height), 0.0f, 1.0f};
        vkCmdSetViewport(frames_[frame_index_].cmd, 0, 1, &viewport);
    }

    void set_scissor(rhi::Rect r) override {
        if (!frame_active_) {
            return;
        }
        scissor_ = r;
        const VkRect2D rect = clamped_scissor();
        vkCmdSetScissor(frames_[frame_index_].cmd, 0, 1, &rect);
    }

    void bind_pipeline(rhi::PipelineHandle handle) override {
        if (!pipelines_.contains(handle.id)) {
            LOG_ERROR("bind_pipeline: invalid pipeline handle {}", handle.id);
            return;
        }
        current_pipeline_ = handle.id;
    }

    void bind_vertex_buffer(std::uint32_t stream, rhi::BufferHandle buffer, std::size_t offset) override {
        if (stream >= rhi::max_vertex_streams) {
            LOG_ERROR("bind_vertex_buffer: stream {} out of range", stream);
            return;
        }
        vertex_bindings_[stream] = {buffer.id, offset};
    }

    void bind_index_buffer(rhi::BufferHandle buffer, rhi::IndexFormat format) override {
        index_buffer_ = buffer.id;
        index_format_ = format;
    }

    void bind_uniform_buffer(std::uint32_t binding, rhi::BufferHandle buffer, std::size_t offset,
                             std::size_t size) override {
        if (binding >= rhi::max_uniform_bindings) {
            LOG_ERROR("bind_uniform_buffer: binding {} out of range", binding);
            return;
        }
        uniform_bindings_[binding] = {buffer.id, offset, size};
    }

    void bind_texture(std::uint32_t binding, rhi::TextureHandle texture) override {
        if (binding >= rhi::max_texture_bindings || !textures_.contains(texture.id)) {
            LOG_ERROR("bind_texture: invalid texture {} or binding {}", texture.id, binding);
            return;
        }
        texture_bindings_[binding] = texture.id;
    }

    void draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex,
              std::uint32_t first_instance) override {
        if (vertex_count == 0 || instance_count == 0 || !prepare_draw()) {
            return;
        }
        vkCmdDraw(frames_[frame_index_].cmd, vertex_count, instance_count, first_vertex, first_instance);
    }

    void draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
                      std::uint32_t first_instance) override {
        if (index_count == 0 || instance_count == 0 || !prepare_draw()) {
            return;
        }
        Allocation* index = use_buffer(index_buffer_);
        if (!index) {
            LOG_ERROR("draw_indexed: no index buffer bound");
            return;
        }
        VkCommandBuffer cmd = frames_[frame_index_].cmd;
        vkCmdBindIndexBuffer(cmd, index->buffer, 0,
                             index_format_ == rhi::IndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, index_count, instance_count, first_index, 0, first_instance);
    }

    void draw_indexed_base_vertex(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index,
                                  std::int32_t base_vertex, std::uint32_t first_instance) override {
        // Tier::Full: refused (logged once) when the device is gated to Compatible.
        if (!rhi::detail::feature_allowed(info_, rhi::Feature::BaseVertexDraws, base_vertex_reported_)) {
            return;
        }
        if (index_count == 0 || instance_count == 0 || !prepare_draw()) {
            return;
        }
        Allocation* index = use_buffer(index_buffer_);
        if (!index) {
            LOG_ERROR("draw_indexed_base_vertex: no index buffer bound");
            return;
        }
        VkCommandBuffer cmd = frames_[frame_index_].cmd;
        vkCmdBindIndexBuffer(cmd, index->buffer, 0,
                             index_format_ == rhi::IndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, index_count, instance_count, first_index, base_vertex, first_instance);
    }

    bool capture_frame(std::string path) override {
        if (!(swapchain_.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
            LOG_ERROR("capture_frame: the swapchain images can't be copied from on this surface");
            return false;
        }
        capture_path_ = std::move(path);
        return true;
    }

private:
    struct Swapchain {
        VkSwapchainKHR handle = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        std::vector<VkImage> images;
        std::vector<VkImageView> views;
        std::vector<VkSemaphore> render_finished;
        VkImageUsageFlags usage = 0;
        // Multisampled color target (msaa_samples > 1), resolved into the
        // swapchain image when rendering ends.
        VkImage msaa_image = VK_NULL_HANDLE;
        VmaAllocation msaa_allocation = VK_NULL_HANDLE;
        VkImageView msaa_view = VK_NULL_HANDLE;
    };

    rhi::Rect full_rect() const {
        return {0, 0, static_cast<std::int32_t>(swapchain_.extent.width),
                static_cast<std::int32_t>(swapchain_.extent.height)};
    }

    // Vulkan rejects scissors outside the framebuffer; GL just clips them.
    VkRect2D clamped_scissor() const {
        const std::int32_t w = static_cast<std::int32_t>(swapchain_.extent.width);
        const std::int32_t h = static_cast<std::int32_t>(swapchain_.extent.height);
        const std::int32_t x0 = std::clamp(scissor_.x, 0, w);
        const std::int32_t y0 = std::clamp(scissor_.y, 0, h);
        const std::int32_t x1 = std::clamp(scissor_.x + std::max(scissor_.width, 0), x0, w);
        const std::int32_t y1 = std::clamp(scissor_.y + std::max(scissor_.height, 0), y0, h);
        return {{x0, y0}, {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}};
    }

    bool allocate(const Buffer& buffer, Allocation& out) {
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = std::max<std::size_t>(buffer.size, 1);
        buffer_info.usage = to_vk_usage(buffer.kind) | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        if (buffer.usage == rhi::BufferUsage::Stream &&
            buffer.size * (frames_in_flight + 1) > bar_budget_ / 2) {
            // Without Resizable BAR the CPU-writable VRAM window is 256 MB. A
            // stream this large (all its versions) would crowd it out, and once
            // the window is full each write into it ran ~15x slower (45 ms for
            // 72 MB): system memory, which the GPU reads over PCIe, is steady.
            alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        }
        alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocated{};
        if (VkResult r = vmaCreateBuffer(allocator_, &buffer_info, &alloc_info, &out.buffer, &out.allocation,
                                         &allocated);
            r != VK_SUCCESS) {
            LOG_ERROR("vmaCreateBuffer failed: {}", string_VkResult(r));
            return false;
        }
        out.mapped = allocated.pMappedData;
        out.last_used = 0;
        return true;
    }

    // Switches `buffer` to a version the GPU is done with.
    bool rename(Buffer& buffer) {
        std::uint32_t next = static_cast<std::uint32_t>(buffer.versions.size());
        for (std::uint32_t i = 0; i < buffer.versions.size(); ++i) {
            if (i != buffer.current && buffer.versions[i].last_used <= completed_serial_) {
                next = i;
                break;
            }
        }
        if (next == buffer.versions.size()) {
            Allocation fresh;
            if (!allocate(buffer, fresh)) {
                return false;
            }
            buffer.versions.push_back(fresh);
            if (buffer.versions.size() > 2 * frames_in_flight + 2) {
                LOG_DEBUG("Buffer has {} versions: updated after use many times in one frame?",
                          buffer.versions.size());
            }
        }
        buffer.current = next;
        if (buffer.usage == rhi::BufferUsage::Dynamic) { // contents persist: copy them forward
            Allocation& version = buffer.versions[next];
            std::memcpy(version.mapped, buffer.shadow.data(), buffer.size);
            vmaFlushAllocation(allocator_, version.allocation, 0, buffer.size);
        }
        return true;
    }

    // Resolves a buffer handle to the version this frame's commands will read,
    // and marks it as used by the frame being recorded.
    Allocation* use_buffer(std::uint32_t id) {
        auto it = buffers_.find(id);
        if (it == buffers_.end()) {
            return nullptr;
        }
        Allocation& version = it->second.versions[it->second.current];
        version.last_used = submitted_serial_ + 1;
        return &version;
    }

    // Copies `data` into a fresh staging buffer; the copy into the image is
    // recorded at the next end_frame (record_uploads).
    void queue_upload(std::uint32_t texture, rhi::Rect region, std::span<const std::byte> data) {
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = data.size();
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        PendingUpload upload{texture, region, VK_NULL_HANDLE, VK_NULL_HANDLE};
        VmaAllocationInfo allocated{};
        if (VkResult r = vmaCreateBuffer(allocator_, &buffer_info, &alloc_info, &upload.staging,
                                         &upload.staging_allocation, &allocated);
            r != VK_SUCCESS) {
            LOG_ERROR("update_texture: staging vmaCreateBuffer failed: {}", string_VkResult(r));
            return;
        }
        std::memcpy(allocated.pMappedData, data.data(), data.size());
        vmaFlushAllocation(allocator_, upload.staging_allocation, 0, data.size());
        pending_uploads_.push_back(upload);
    }

    // Records every queued texture write into `cmd`. Returns false if there
    // was nothing to record. Staging buffers move to recorded_staging_ and are
    // freed once this frame's submission completes.
    bool record_uploads(VkCommandBuffer cmd) {
        if (pending_uploads_.empty()) {
            return false;
        }
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        for (const PendingUpload& upload : pending_uploads_) {
            auto it = textures_.find(upload.texture);
            if (it == textures_.end()) { // destroy_texture already freed its uploads; defensive
                continue;
            }
            Texture& texture = it->second;
            // Earlier submissions may still sample the image: an execution
            // dependency on their shader reads covers the write-after-read hazard.
            image_barrier(cmd, texture.image,
                          texture.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, texture_read_stages, 0,
                          VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT);
            if (upload.staging) {
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageOffset = {upload.region.x, upload.region.y, 0};
                region.imageExtent = {static_cast<std::uint32_t>(upload.region.width),
                                      static_cast<std::uint32_t>(upload.region.height), 1};
                vkCmdCopyBufferToImage(cmd, upload.staging, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &region);
                recorded_staging_.push_back({0, upload.staging, upload.staging_allocation});
            } else {
                const VkClearColorValue black{};
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(cmd, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            }
            image_barrier(cmd, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          texture_read_stages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            texture.initialized = true;
            // The upload itself uses the image: destroy_texture must wait for
            // this submission even if no draw ever samples the texture.
            texture.last_used = submitted_serial_ + 1;
        }
        pending_uploads_.clear();
        vkEndCommandBuffer(cmd);
        return true;
    }

    VkShaderModule create_shader_module(std::span<const std::uint32_t> spirv) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = spirv.size_bytes();
        info.pCode = spirv.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (VkResult r = vkCreateShaderModule(device_, &info, nullptr, &module); r != VK_SUCCESS) {
            LOG_ERROR("vkCreateShaderModule failed: {}", string_VkResult(r));
            return VK_NULL_HANDLE;
        }
        return module;
    }

    // Every pipeline shares one layout (rhi.hpp ShaderSource contract), both
    // sets visible to both stages:
    //   set 0 = max_uniform_bindings uniform buffers,
    //   set 1 = max_texture_bindings combined image samplers.
    bool create_layouts() {
        const auto make_set_layout = [&](VkDescriptorType type, std::uint32_t count, VkDescriptorSetLayout& out) {
            std::array<VkDescriptorSetLayoutBinding, std::max(rhi::max_uniform_bindings, rhi::max_texture_bindings)>
                bindings{};
            for (std::uint32_t i = 0; i < count; ++i) {
                bindings[i].binding = i;
                bindings[i].descriptorType = type;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            set_info.bindingCount = count;
            set_info.pBindings = bindings.data();
            VK_TRY(vkCreateDescriptorSetLayout(device_, &set_info, nullptr, &out));
            return true;
        };
        if (!make_set_layout(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, rhi::max_uniform_bindings, set_layout_) ||
            !make_set_layout(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, rhi::max_texture_bindings,
                             texture_set_layout_)) {
            return false;
        }

        const std::array<VkDescriptorSetLayout, 2> set_layouts{set_layout_, texture_set_layout_};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.setLayoutCount = static_cast<std::uint32_t>(set_layouts.size());
        layout_info.pSetLayouts = set_layouts.data();
        VK_TRY(vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_));

        // One immutable-state sampler per filter; addressing is always clamp
        // to edge and there is one mip level (rhi.hpp TextureDesc).
        for (const rhi::Filter filter : {rhi::Filter::Nearest, rhi::Filter::Linear}) {
            const VkFilter vk_filter = filter == rhi::Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sampler_info.magFilter = vk_filter;
            sampler_info.minFilter = vk_filter;
            sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler_info.maxLod = 0.0f;
            VK_TRY(vkCreateSampler(device_, &sampler_info, nullptr, &samplers_[static_cast<std::size_t>(filter)]));
        }

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu_, &props);
        // Capabilities (rhi::Feature): everything the RHI exposes is core
        // Vulkan 1.3; only MSAA and frame capture depend on the hardware/surface.
        rhi::Limits& limits = info_.limits;
        limits.max_texture_size = props.limits.maxImageDimension2D;
        limits.uniform_offset_alignment =
            static_cast<std::uint32_t>(std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 1));
        limits.max_uniform_block_size = props.limits.maxUniformBufferRange;
        limits.max_vertex_attributes = std::min(props.limits.maxVertexInputAttributes, rhi::max_vertex_attributes);
        const VkSampleCountFlags supported = props.limits.framebufferColorSampleCounts;
        limits.max_msaa_samples = 1;
        while (limits.max_msaa_samples < 64 && (supported & (limits.max_msaa_samples * 2))) {
            limits.max_msaa_samples *= 2;
        }
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
        using rhi::Feature;
        for (Feature f : {Feature::InstancedDraws, Feature::IndexUInt32, Feature::UniformBuffers, Feature::Textures,
                          Feature::IntegerAttributes, Feature::PointsAndLines, Feature::BaseVertexDraws}) {
            info_.hardware_features.set(f);
        }
        info_.hardware_features.set(Feature::Msaa, limits.max_msaa_samples >= 2);
        info_.hardware_features.set(Feature::FrameCapture, (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0);
        rhi::detail::apply_tier_gate(info_, options_.tier);

        // Largest supported power of two <= the requested MSAA sample count.
        const std::uint32_t samples = rhi::detail::msaa_sample_count(options_.msaa_samples, info_.limits.max_msaa_samples);
        samples_ = static_cast<VkSampleCountFlagBits>(samples);
        info_.msaa_samples = samples;
        return true;
    }

    VkDescriptorSet allocate_descriptor_set(VkDescriptorSetLayout layout) {
        Frame& frame = frames_[frame_index_];
        for (;;) {
            if (frame.descriptor_pool_index == frame.descriptor_pools.size()) {
                const std::array<VkDescriptorPoolSize, 2> sizes{{
                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, descriptor_sets_per_pool * rhi::max_uniform_bindings},
                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, descriptor_sets_per_pool * rhi::max_texture_bindings},
                }};
                VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                pool_info.maxSets = descriptor_sets_per_pool;
                pool_info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
                pool_info.pPoolSizes = sizes.data();
                VkDescriptorPool pool = VK_NULL_HANDLE;
                if (VkResult r = vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool); r != VK_SUCCESS) {
                    LOG_ERROR("vkCreateDescriptorPool failed: {}", string_VkResult(r));
                    return VK_NULL_HANDLE;
                }
                frame.descriptor_pools.push_back(pool);
            }
            VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            info.descriptorPool = frame.descriptor_pools[frame.descriptor_pool_index];
            info.descriptorSetCount = 1;
            info.pSetLayouts = &layout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkResult r = vkAllocateDescriptorSets(device_, &info, &set);
            if (r == VK_SUCCESS) {
                return set;
            }
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
                LOG_ERROR("vkAllocateDescriptorSets failed: {}", string_VkResult(r));
                return VK_NULL_HANDLE;
            }
            ++frame.descriptor_pool_index; // this pool is full; move on to (or create) the next
        }
    }

    // Binds the current pipeline, vertex buffers and uniform buffers.
    bool prepare_draw() {
        if (!frame_active_) {
            return false;
        }
        auto it = pipelines_.find(current_pipeline_);
        if (it == pipelines_.end()) {
            LOG_ERROR("draw: no pipeline bound");
            return false;
        }
        begin_rendering();
        VkCommandBuffer cmd = frames_[frame_index_].cmd;
        const Pipeline& pipeline = it->second;
        if (bound_pipeline_ != pipeline.pipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
            bound_pipeline_ = pipeline.pipeline;
        }

        std::array<VkBuffer, rhi::max_vertex_streams> vertex_buffers{};
        std::array<VkDeviceSize, rhi::max_vertex_streams> vertex_offsets{};
        for (std::uint32_t s = 0; s < pipeline.stream_count; ++s) {
            Allocation* version = use_buffer(vertex_bindings_[s].buffer);
            if (!version) {
                LOG_ERROR("draw: no vertex buffer bound to stream {}", s);
                return false;
            }
            vertex_buffers[s] = version->buffer;
            vertex_offsets[s] = vertex_bindings_[s].offset;
        }
        if (pipeline.stream_count > 0) {
            vkCmdBindVertexBuffers(cmd, 0, pipeline.stream_count, vertex_buffers.data(), vertex_offsets.data());
        }

        std::array<ResolvedUniform, rhi::max_uniform_bindings> resolved{};
        bool any_uniform = false;
        for (std::uint32_t b = 0; b < rhi::max_uniform_bindings; ++b) {
            const UniformBinding& binding = uniform_bindings_[b];
            if (!binding.buffer) {
                continue;
            }
            auto buffer = buffers_.find(binding.buffer);
            if (buffer == buffers_.end()) {
                LOG_ERROR("draw: invalid uniform buffer {} at binding {}", binding.buffer, b);
                return false;
            }
            const std::size_t size = buffer->second.size;
            const std::size_t range = binding.size ? binding.size : size - std::min(binding.offset, size);
            resolved[b] = {use_buffer(binding.buffer)->buffer, binding.offset, range};
            any_uniform = true;
        }
        if (any_uniform && resolved != written_uniforms_) {
            VkDescriptorSet set = allocate_descriptor_set(set_layout_);
            if (!set) {
                return false;
            }
            std::array<VkDescriptorBufferInfo, rhi::max_uniform_bindings> infos{};
            std::array<VkWriteDescriptorSet, rhi::max_uniform_bindings> writes{};
            std::uint32_t count = 0;
            for (std::uint32_t b = 0; b < rhi::max_uniform_bindings; ++b) {
                if (!resolved[b].buffer) {
                    continue;
                }
                infos[count] = {resolved[b].buffer, resolved[b].offset, resolved[b].range};
                writes[count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[count].dstSet = set;
                writes[count].dstBinding = b;
                writes[count].descriptorCount = 1;
                writes[count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[count].pBufferInfo = &infos[count];
                ++count;
            }
            vkUpdateDescriptorSets(device_, count, writes.data(), 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);
            written_uniforms_ = resolved;
        }
        return bind_textures(cmd);
    }

    // Set 1: the bound textures, written into a fresh descriptor set only when
    // the bindings changed since the last draw.
    bool bind_textures(VkCommandBuffer cmd) {
        std::array<ResolvedTexture, rhi::max_texture_bindings> resolved{};
        bool any = false;
        for (std::uint32_t b = 0; b < rhi::max_texture_bindings; ++b) {
            auto it = textures_.find(texture_bindings_[b]);
            if (it == textures_.end()) {
                continue;
            }
            Texture& texture = it->second;
            texture.last_used = submitted_serial_ + 1;
            resolved[b] = {texture.view, samplers_[static_cast<std::size_t>(texture.filter)]};
            any = true;
        }
        if (!any || resolved == written_textures_) {
            return true;
        }
        VkDescriptorSet set = allocate_descriptor_set(texture_set_layout_);
        if (!set) {
            return false;
        }
        std::array<VkDescriptorImageInfo, rhi::max_texture_bindings> infos{};
        std::array<VkWriteDescriptorSet, rhi::max_texture_bindings> writes{};
        std::uint32_t count = 0;
        for (std::uint32_t b = 0; b < rhi::max_texture_bindings; ++b) {
            if (!resolved[b].view) {
                continue;
            }
            infos[count] = {resolved[b].sampler, resolved[b].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[count].dstSet = set;
            writes[count].dstBinding = b;
            writes[count].descriptorCount = 1;
            writes[count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[count].pImageInfo = &infos[count];
            ++count;
        }
        vkUpdateDescriptorSets(device_, count, writes.data(), 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 1, 1, &set, 0, nullptr);
        written_textures_ = resolved;
        return true;
    }

    // Copies the swapchain image into a host-visible buffer (debug captures).
    // Leaves the image in TRANSFER_SRC_OPTIMAL.
    bool record_readback(VkCommandBuffer cmd, Allocation& out) {
        const VkExtent2D extent = swapchain_.extent;
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo allocated{};
        if (VkResult r = vmaCreateBuffer(allocator_, &buffer_info, &alloc_info, &out.buffer, &out.allocation,
                                         &allocated);
            r != VK_SUCCESS) {
            LOG_ERROR("capture: vmaCreateBuffer failed: {}", string_VkResult(r));
            return false;
        }
        out.mapped = allocated.pMappedData;

        image_barrier(cmd, swapchain_.images[image_index_], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, swapchain_.images[image_index_], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.buffer,
                               1, &region);

        VkMemoryBarrier2 to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        to_host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &to_host;
        vkCmdPipelineBarrier2(cmd, &dependency);
        return true;
    }

    void finish_readback(Allocation& readback) {
        vkQueueWaitIdle(queue_); // debug path: a stall is fine
        completed_serial_ = submitted_serial_;
        vmaInvalidateAllocation(allocator_, readback.allocation, 0, VK_WHOLE_SIZE);

        const auto w = static_cast<std::size_t>(swapchain_.extent.width);
        const auto h = static_cast<std::size_t>(swapchain_.extent.height);
        const bool bgra = swapchain_.format == VK_FORMAT_B8G8R8A8_UNORM || swapchain_.format == VK_FORMAT_B8G8R8A8_SRGB;
        const auto* src = static_cast<const std::uint8_t*>(readback.mapped);
        std::vector<std::uint8_t> rgb(w * h * 3);
        for (std::size_t i = 0; i < w * h; ++i) {
            rgb[i * 3 + 0] = src[i * 4 + (bgra ? 2 : 0)];
            rgb[i * 3 + 1] = src[i * 4 + 1];
            rgb[i * 3 + 2] = src[i * 4 + (bgra ? 0 : 2)];
        }
        rhi::detail::write_ppm(capture_path_, static_cast<int>(w), static_cast<int>(h), rgb);
        vmaDestroyBuffer(allocator_, readback.buffer, readback.allocation);
    }

    // Picked once at startup: pipelines are created against this format.
    bool choose_surface_format() {
        std::uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &format_count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &format_count, formats.data());
        if (formats.empty()) {
            LOG_ERROR("Surface reports no formats");
            return false;
        }
        // UNORM, not SRGB: matches the GL backend's default framebuffer so both
        // backends show identical colors. Color management is a later decision.
        surface_format_ = formats[0];
        for (const VkSurfaceFormatKHR& f : formats) {
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                surface_format_ = f;
                break;
            }
        }

        present_mode_ = VK_PRESENT_MODE_FIFO_KHR; // VSync, always available; matches GL
        if (!options_.vsync) {
            std::uint32_t mode_count = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &mode_count, nullptr);
            std::vector<VkPresentModeKHR> modes(mode_count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(gpu_, surface_, &mode_count, modes.data());
            for (VkPresentModeKHR wanted : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR}) {
                if (std::ranges::find(modes, wanted) != modes.end()) {
                    present_mode_ = wanted;
                    break;
                }
            }
        }
        return true;
    }


    bool create_instance() {
        auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        if (!get_proc) {
            LOG_ERROR("No Vulkan loader: {}", SDL_GetError());
            return false;
        }
        volkInitializeCustom(get_proc);

        std::uint32_t loader_version = volkGetInstanceVersion();
        if (loader_version < required_api) {
            LOG_ERROR("Vulkan loader is {}, need 1.3", version_string(loader_version));
            return false;
        }

        std::uint32_t sdl_count = 0;
        const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
        if (!sdl_extensions) {
            LOG_ERROR("SDL_Vulkan_GetInstanceExtensions failed: {}", SDL_GetError());
            return false;
        }
        std::vector<const char*> extensions(sdl_extensions, sdl_extensions + sdl_count);
        std::vector<const char*> layers;

        bool debug_utils = false;
        if (want_validation) {
            if (has_layer(validation_layer)) {
                layers.push_back(validation_layer);
                LOG_INFO("Vulkan validation enabled ({})", validation_layer);
            } else {
                LOG_WARN("Vulkan validation layer not installed (Arch: pacman -S vulkan-validation-layers)");
            }
            if (has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                debug_utils = true;
            }
        }

        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Cuervo";
        app.pEngineName = "Cuervo Engine";
        app.apiVersion = required_api;

        VkDebugUtilsMessengerCreateInfoEXT messenger_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger_info.pfnUserCallback = debug_callback;

        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app;
        create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        create.ppEnabledExtensionNames = extensions.data();
        create.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        create.ppEnabledLayerNames = layers.data();
        create.pNext = debug_utils ? &messenger_info : nullptr; // also covers create/destroy
        VK_TRY(vkCreateInstance(&create, nullptr, &instance_));
        volkLoadInstanceOnly(instance_);

        if (debug_utils) {
            VK_TRY(vkCreateDebugUtilsMessengerEXT(instance_, &messenger_info, nullptr, &messenger_));
        }
        return true;
    }

    bool create_surface() {
        if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
            LOG_ERROR("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
            return false;
        }
        return true;
    }

    // Picks the best GPU with Vulkan 1.3, dynamic rendering, synchronization2,
    // swapchain support and a queue that can both draw and present.
    bool pick_gpu() {
        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        std::vector<VkPhysicalDevice> gpus(count);
        vkEnumeratePhysicalDevices(instance_, &count, gpus.data());

        int best_score = -1;
        for (VkPhysicalDevice gpu : gpus) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(gpu, &props);

            VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &features13;
            vkGetPhysicalDeviceFeatures2(gpu, &features);

            const char* rejected = nullptr;
            std::optional<std::uint32_t> family = find_queue_family(gpu);
            if (props.apiVersion < required_api) {
                rejected = "no Vulkan 1.3";
            } else if (!features13.dynamicRendering || !features13.synchronization2) {
                rejected = "missing dynamicRendering/synchronization2";
            } else if (!has_device_extension(gpu, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                rejected = "no swapchain extension";
            } else if (!family) {
                rejected = "no graphics+present queue";
            }
            if (rejected) {
                LOG_DEBUG("Skipping GPU {}: {}", props.deviceName, rejected);
                continue;
            }

            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? 1
                                                                                     : 0;
            if (score > best_score) {
                best_score = score;
                gpu_ = gpu;
                queue_family_ = *family;
                version_ = version_string(props.apiVersion);
                renderer_ = props.deviceName;
            }
        }
        if (!gpu_) {
            LOG_ERROR("No GPU supports Vulkan 1.3 with presentation to this window");
            return false;
        }
        return true;
    }

    std::optional<std::uint32_t> find_queue_family(VkPhysicalDevice gpu) const {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
        for (std::uint32_t i = 0; i < count; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface_, &present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                return i;
            }
        }
        return std::nullopt;
    }

    bool create_device() {
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;

        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        create.pNext = &features13;
        create.queueCreateInfoCount = 1;
        create.pQueueCreateInfos = &queue_info;
        create.enabledExtensionCount = 1;
        create.ppEnabledExtensionNames = extensions;
        VK_TRY(vkCreateDevice(gpu_, &create, nullptr, &device_));
        volkLoadDevice(device_);
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

        info_.backend = rhi::Backend::Vulkan;
        info_.max_tier = rhi::Tier::Full;
        info_.api_version = version_;
        info_.renderer = renderer_;
        return true;
    }

    bool create_allocator() {
        VmaVulkanFunctions functions{};
        functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

        VmaAllocatorCreateInfo create{};
        create.vulkanApiVersion = required_api;
        create.instance = instance_;
        create.physicalDevice = gpu_;
        create.device = device_;
        create.pVulkanFunctions = &functions;
        VK_TRY(vmaCreateAllocator(&create, &allocator_));

        // The largest memory heap the CPU can write that also lives on the GPU:
        // VRAM itself with Resizable BAR (or on integrated GPUs), else the small BAR window.
        const VkPhysicalDeviceMemoryProperties* memory = nullptr;
        vmaGetMemoryProperties(allocator_, &memory);
        constexpr VkMemoryPropertyFlags both = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        bar_budget_ = 0;
        for (std::uint32_t t = 0; t < memory->memoryTypeCount; ++t) {
            if ((memory->memoryTypes[t].propertyFlags & both) == both) {
                bar_budget_ = std::max<std::size_t>(bar_budget_, memory->memoryHeaps[memory->memoryTypes[t].heapIndex].size);
            }
        }
        return true;
    }

    bool create_frames() {
        for (Frame& frame : frames_) {
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = queue_family_;
            VK_TRY(vkCreateCommandPool(device_, &pool, nullptr, &frame.pool));

            VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            alloc.commandPool = frame.pool;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = 1;
            VK_TRY(vkAllocateCommandBuffers(device_, &alloc, &frame.cmd));
            VK_TRY(vkAllocateCommandBuffers(device_, &alloc, &frame.upload_cmd));

            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_TRY(vkCreateSemaphore(device_, &semaphore, nullptr, &frame.image_available));

            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VK_TRY(vkCreateFence(device_, &fence, nullptr, &frame.in_flight));
        }
        return true;
    }

    bool recreate_swapchain(VkExtent2D wanted) {
        vkDeviceWaitIdle(device_);

        VkSurfaceCapabilitiesKHR caps;
        VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps));
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) { // the surface size is ours to pick (Wayland)
            extent.width = std::clamp(wanted.width, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(wanted.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (extent.width == 0 || extent.height == 0) {
            return false;
        }

        const VkSurfaceFormatKHR format = surface_format_;

        std::uint32_t image_count = caps.minImageCount + 1;
        if (caps.maxImageCount > 0) {
            image_count = std::min(image_count, caps.maxImageCount);
        }

        VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        create.surface = surface_;
        create.minImageCount = image_count;
        create.imageFormat = format.format;
        create.imageColorSpace = format.colorSpace;
        create.imageExtent = extent;
        create.imageArrayLayers = 1;
        // TRANSFER_SRC (when offered) lets capture_frame read the image back.
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        create.imageUsage = usage;
        create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.preTransform = caps.currentTransform;
        create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create.presentMode = present_mode_;
        create.clipped = VK_TRUE;
        create.oldSwapchain = swapchain_.handle;

        Swapchain next;
        next.format = format.format;
        next.extent = extent;
        next.usage = usage;
        VK_TRY(vkCreateSwapchainKHR(device_, &create, nullptr, &next.handle));
        destroy_swapchain(swapchain_);
        swapchain_ = std::move(next);
        swapchain_dirty_ = false;

        std::uint32_t count = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_.handle, &count, nullptr);
        swapchain_.images.resize(count);
        vkGetSwapchainImagesKHR(device_, swapchain_.handle, &count, swapchain_.images.data());

        for (VkImage image : swapchain_.images) {
            VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view_info.image = image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = swapchain_.format;
            view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view = VK_NULL_HANDLE;
            VK_TRY(vkCreateImageView(device_, &view_info, nullptr, &view));
            swapchain_.views.push_back(view);

            VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore semaphore = VK_NULL_HANDLE;
            VK_TRY(vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore));
            swapchain_.render_finished.push_back(semaphore);
        }

        if (samples_ != VK_SAMPLE_COUNT_1_BIT && !create_msaa_target()) {
            return false;
        }

        LOG_INFO("Swapchain {}x{}, {} images, VkFormat {}, {}x MSAA", extent.width, extent.height, count,
                 static_cast<int>(swapchain_.format), info_.msaa_samples);
        return true;
    }

    bool create_msaa_target() {
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = swapchain_.format;
        image_info.extent = {swapchain_.extent.width, swapchain_.extent.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = samples_;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Transient: only ever resolved, never stored, so tilers can keep it on chip.
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VK_TRY(vmaCreateImage(allocator_, &image_info, &alloc_info, &swapchain_.msaa_image, &swapchain_.msaa_allocation,
                              nullptr));
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = swapchain_.msaa_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = swapchain_.format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_TRY(vkCreateImageView(device_, &view_info, nullptr, &swapchain_.msaa_view));
        return true;
    }

    void destroy_swapchain(Swapchain& swapchain) {
        for (VkSemaphore semaphore : swapchain.render_finished) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
        for (VkImageView view : swapchain.views) {
            vkDestroyImageView(device_, view, nullptr);
        }
        if (swapchain.msaa_view) {
            vkDestroyImageView(device_, swapchain.msaa_view, nullptr);
        }
        if (swapchain.msaa_image) {
            vmaDestroyImage(allocator_, swapchain.msaa_image, swapchain.msaa_allocation);
        }
        if (swapchain.handle) {
            vkDestroySwapchainKHR(device_, swapchain.handle, nullptr);
        }
        swapchain = {};
    }

    void begin_rendering() {
        if (rendering_) {
            return;
        }
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = swapchain_.views[image_index_];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = pending_clear_ ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (swapchain_.msaa_view) {
            // Render multisampled, resolve (average) into the swapchain image
            // when rendering ends; the samples themselves are never stored.
            color.imageView = swapchain_.msaa_view;
            color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            color.resolveImageView = swapchain_.views[image_index_];
            color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        if (pending_clear_) {
            color.clearValue.color = {{pending_clear_->r, pending_clear_->g, pending_clear_->b, pending_clear_->a}};
        }

        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, swapchain_.extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(frames_[frame_index_].cmd, &rendering);
        rendering_ = true;
        pending_clear_.reset();
    }

    void collect_garbage() {
        std::erase_if(pending_deletes_, [&](const PendingDelete& pending) {
            if (pending.serial > completed_serial_) {
                return false;
            }
            if (pending.buffer) {
                vmaDestroyBuffer(allocator_, pending.buffer, pending.allocation);
            }
            if (pending.pipeline) {
                vkDestroyPipeline(device_, pending.pipeline, nullptr);
            }
            if (pending.view) {
                vkDestroyImageView(device_, pending.view, nullptr);
            }
            if (pending.image) {
                vmaDestroyImage(allocator_, pending.image, pending.allocation);
            }
            return true;
        });
    }

    SDL_Window* window_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;

    Swapchain swapchain_;
    bool swapchain_dirty_ = false;
    std::array<Frame, frames_in_flight> frames_{};
    std::size_t bar_budget_ = 0; // bytes of CPU-writable device-local memory (see allocate)
    std::uint32_t frame_index_ = 0;
    std::uint32_t image_index_ = 0;
    bool frame_active_ = false;
    bool skip_frame_ = false;
    bool rendering_ = false;
    std::optional<rhi::Color> pending_clear_;

    // Submissions complete in order on our single queue, so one counter is
    // enough to know when a deleted resource is no longer in use.
    std::uint64_t submitted_serial_ = 0;
    std::uint64_t completed_serial_ = 0;
    std::vector<PendingDelete> pending_deletes_;

    std::unordered_map<std::uint32_t, Buffer> buffers_;
    std::unordered_map<std::uint32_t, Pipeline> pipelines_;
    std::unordered_map<std::uint32_t, Texture> textures_;
    std::vector<PendingUpload> pending_uploads_;
    std::vector<PendingDelete> recorded_staging_; // staging buffers of the frame being submitted
    std::uint32_t next_id_ = 1;

    rhi::DeviceOptions options_;
    VkSurfaceFormatKHR surface_format_{};
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout texture_set_layout_ = VK_NULL_HANDLE;
    std::array<VkSampler, 2> samplers_{}; // indexed by rhi::Filter
    bool base_vertex_reported_ = false;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;

    // Draw state; reset in begin_frame.
    std::uint32_t current_pipeline_ = 0;
    VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
    std::array<VertexBinding, rhi::max_vertex_streams> vertex_bindings_{};
    std::uint32_t index_buffer_ = 0;
    rhi::IndexFormat index_format_ = rhi::IndexFormat::UInt16;
    std::array<UniformBinding, rhi::max_uniform_bindings> uniform_bindings_{};
    std::array<ResolvedUniform, rhi::max_uniform_bindings> written_uniforms_{};
    std::array<std::uint32_t, rhi::max_texture_bindings> texture_bindings_{};
    std::array<ResolvedTexture, rhi::max_texture_bindings> written_textures_{};
    rhi::Rect scissor_{};
    std::string capture_path_;

    rhi::DeviceInfo info_;
    std::string version_;
    std::string renderer_;
};

} // namespace

std::unique_ptr<rhi::Device> create_device(Window& window, const rhi::DeviceOptions& options) {
    if (window.graphics() != GraphicsApi::Vulkan) {
        LOG_ERROR("The window was not created for Vulkan");
        return nullptr;
    }
    auto device = std::make_unique<VulkanDevice>(window.native(), options);
    if (!device->init()) {
        return nullptr;
    }
    LOG_INFO("Vulkan device: {} ({}), {} tier", device->info().api_version, device->info().renderer,
             rhi::tier_name(device->info().tier));
    return device;
}

} // namespace cuervo::vk
