#include "cuervo/rhi/rhi.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>

#include <SDL3/SDL.h>

#include "cuervo/rhi/rhi_internal.hpp"

#include "cuervo/core/log.hpp"
#include "cuervo/platform/window.hpp"
#include "cuervo/rhi/gl/gl_device.hpp"
#ifdef CUERVO_HAS_VULKAN
#include "cuervo/rhi/vulkan/vk_device.hpp"
#endif
#include "cuervo/rhi/webgpu/webgpu_device.hpp"

#ifndef CUERVO_HAS_WEBGPU
// Without the backend (CUERVO_WEBGPU=OFF) tools still link its frame-limit queries.
namespace cuervo::webgpu {
std::size_t peak_frames_in_flight(const rhi::Device&) { return 0; }
std::size_t frame_limit() { return 0; }
} // namespace cuervo::webgpu
#endif

namespace cuervo::rhi {

std::unique_ptr<Device> create_device(Backend backend, Window& window, const DeviceOptions& options) {
    switch (backend) {
        case Backend::OpenGL:
            return gl::create_device(window, options);
        case Backend::Vulkan:
#ifdef CUERVO_HAS_VULKAN
            return vk::create_device(window, options);
#else
            LOG_ERROR("This build has no Vulkan backend (CUERVO_VULKAN=OFF)");
            return nullptr;
#endif
        case Backend::WebGPU:
#ifdef CUERVO_HAS_WEBGPU
            return webgpu::create_device(window, options);
#else
            LOG_ERROR("This build has no WebGPU backend (CUERVO_WEBGPU=OFF)");
            return nullptr;
#endif
    }
    return nullptr;
}

Backend default_backend() {
    if (backend_compiled_in(Backend::Vulkan)) return Backend::Vulkan;
#if defined(CUERVO_HAS_WEBGPU) && defined(__EMSCRIPTEN__)
    if (webgpu::browser_device_ready()) return Backend::WebGPU;
#endif
    return Backend::OpenGL;
}

bool parse_backend(std::string_view text, Backend& out) {
    if (text == "gl" || text == "opengl") {
        out = Backend::OpenGL;
    } else if (text == "vulkan" || text == "vk") {
        out = Backend::Vulkan;
    } else if (text == "webgpu" || text == "wgpu") {
        out = Backend::WebGPU;
    } else {
        return false;
    }
    return true;
}

GraphicsApi graphics_api(Backend backend) {
    switch (backend) {
        case Backend::OpenGL: return GraphicsApi::OpenGL;
        case Backend::Vulkan: return GraphicsApi::Vulkan;
        case Backend::WebGPU: return GraphicsApi::WebGPU;
    }
    return GraphicsApi::OpenGL;
}

std::uint32_t full_mip_chain_length(std::int32_t width, std::int32_t height) {
    std::int32_t largest = width > height ? width : height;
    std::uint32_t levels = 1;
    while (largest > 1) {
        largest >>= 1;
        ++levels;
    }
    return levels;
}

std::size_t texture_format_size(TextureFormat format) {
    switch (format) {
        case TextureFormat::RGBA8: return 4;
        case TextureFormat::R8:    return 1;
    }
    return 4;
}

std::string_view tier_name(Tier tier) { return tier == Tier::Full ? "full" : "compatible"; }

bool parse_tier(std::string_view text, Tier& out) {
    if (text == "compatible") {
        out = Tier::Compatible;
        return true;
    }
    if (text == "full") {
        out = Tier::Full;
        return true;
    }
    return false;
}

namespace {

struct FeatureEntry {
    std::string_view name;
    Tier tier;
    bool required;
};

// Indexed by Feature. Keep in sync with the enum (static_assert below).
constexpr FeatureEntry feature_table[] = {
    {"instanced draws", Tier::Compatible, true},
    {"32-bit indices", Tier::Compatible, true},
    {"uniform buffers", Tier::Compatible, true},
    {"textures", Tier::Compatible, true},
    {"integer vertex attributes", Tier::Compatible, true},
    {"points and lines", Tier::Compatible, true},
    {"MSAA", Tier::Compatible, false},
    {"frame capture", Tier::Compatible, false},
    {"texture mips", Tier::Compatible, true},
    {"render targets", Tier::Compatible, true},
    {"base-vertex draws", Tier::Full, true},
    {"multiple window surfaces", Tier::Compatible, false},
};
static_assert(std::size(feature_table) == static_cast<std::size_t>(Feature::Count));

} // namespace

Tier feature_tier(Feature feature) { return feature_table[static_cast<std::size_t>(feature)].tier; }
std::string_view feature_name(Feature feature) { return feature_table[static_cast<std::size_t>(feature)].name; }
bool feature_required(Feature feature) { return feature_table[static_cast<std::size_t>(feature)].required; }

Limits compatible_limits() { return Limits{}; }

TierReport check_tier(const DeviceInfo& info, Tier tier) {
    TierReport report;
    report.gated_down = info.tier < tier;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(Feature::Count); ++i) {
        const auto f = static_cast<Feature>(i);
        if (feature_tier(f) > tier || info.hardware_features.has(f)) continue;
        (feature_required(f) ? report.missing_required : report.missing_optional).push_back(f);
    }
    // Only the Compatible guarantees have numbers; Full has no fixed minimums yet.
    const Limits want = compatible_limits();
    const Limits& have = info.limits;
    auto low = [&](std::string_view what, std::uint32_t value, std::uint32_t needed) {
        if (value < needed) report.limit_warnings.push_back(std::format("{} is {} (tier guarantees {})", what, value, needed));
    };
    low("max texture size", have.max_texture_size, want.max_texture_size);
    low("max uniform block size", have.max_uniform_block_size, want.max_uniform_block_size);
    low("max vertex attributes", have.max_vertex_attributes, want.max_vertex_attributes);
    return report;
}

namespace {
void multi_surface_unsupported(const char* what) {
    LOG_WARN("{}: multiple window surfaces (Feature::MultiSurface) are not supported by this device; "
             "it stays single-window",
             what);
}
} // namespace

SurfaceHandle Device::create_window_surface(SDL_Window*) {
    multi_surface_unsupported("create_window_surface");
    return {};
}
void Device::destroy_window_surface(SurfaceHandle) {}
void Device::begin_frame_surface(SurfaceHandle, ivec2) { multi_surface_unsupported("begin_frame_surface"); }
void Device::end_frame_surface() {}

SDL_Window* create_surface_window(Backend backend, const char* title, int width, int height, bool resizable) {
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (resizable) flags |= SDL_WINDOW_RESIZABLE;
    switch (backend) {
        case Backend::OpenGL: flags |= SDL_WINDOW_OPENGL; break;
        case Backend::Vulkan: flags |= SDL_WINDOW_VULKAN; break;
        case Backend::WebGPU:
            multi_surface_unsupported("create_surface_window");
            return nullptr;
    }
    SDL_Window* window = SDL_CreateWindow(title, width, height, flags);
    if (!window) LOG_ERROR("create_surface_window: SDL_CreateWindow failed: {}", SDL_GetError());
    return window;
}

bool backend_compiled_in(Backend backend) {
    switch (backend) {
        case Backend::OpenGL:
            return true;
        case Backend::Vulkan:
#ifdef CUERVO_HAS_VULKAN
            return true;
#else
            return false;
#endif
        case Backend::WebGPU:
#ifdef CUERVO_HAS_WEBGPU
            return true;
#else
            return false;
#endif
    }
    return false;
}

std::string_view backend_name(Backend backend) {
    switch (backend) {
        case Backend::OpenGL: return "OpenGL";
        case Backend::Vulkan: return "Vulkan";
        case Backend::WebGPU: return "WebGPU";
    }
    return "unknown";
}

} // namespace cuervo::rhi

namespace cuervo::rhi::detail {

bool valid_texture_update(std::int32_t width, std::int32_t height, TextureFormat format, Rect region,
                          std::size_t data_size) {
    if (region.x < 0 || region.y < 0 || region.width < 0 || region.height < 0 || region.x > width - region.width ||
        region.y > height - region.height) {
        LOG_ERROR("update_texture: region {}x{} at ({}, {}) outside a {}x{} texture", region.width, region.height,
                  region.x, region.y, width, height);
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(region.width) * static_cast<std::size_t>(region.height) *
                                 texture_format_size(format);
    if (data_size != expected) {
        LOG_ERROR("update_texture: {} bytes for a {}x{} region (expected {})", data_size, region.width, region.height,
                  expected);
        return false;
    }
    return true;
}

std::span<const std::byte> texture_level_data(const TextureDesc& desc, std::uint32_t level) {
    if (!desc.initial_levels.empty()) {
        return level < desc.initial_levels.size() ? desc.initial_levels[level] : std::span<const std::byte>{};
    }
    if (level == 0 && desc.initial_data) {
        const std::size_t n = static_cast<std::size_t>(desc.width) * static_cast<std::size_t>(desc.height) *
                              texture_format_size(desc.format);
        return {static_cast<const std::byte*>(desc.initial_data), n};
    }
    return {};
}

bool render_target_allowed(const DeviceInfo& info, const RenderTargetDesc& desc) {
    if (!info.features.has(Feature::RenderTargets)) {
        LOG_ERROR("create_render_target '{}': render targets are not available on this device", desc.debug_name);
        return false;
    }
    TextureDesc as_texture;
    as_texture.width = desc.width;
    as_texture.height = desc.height;
    as_texture.debug_name = desc.debug_name;
    return texture_size_allowed(info, as_texture);
}

std::uint32_t texture_levels(const DeviceInfo& info, const TextureDesc& desc) {
    if (!texture_size_allowed(info, desc)) return 0;
    const std::uint32_t full = full_mip_chain_length(desc.width, desc.height);
    const std::uint32_t levels = desc.mip_levels == 0 ? full : desc.mip_levels;
    if (levels > full) {
        LOG_ERROR("create_texture '{}': {} mip levels for a {}x{} texture (max {})", desc.debug_name, levels,
                  desc.width, desc.height, full);
        return 0;
    }
    if (desc.srgb && desc.format != TextureFormat::RGBA8) {
        LOG_ERROR("create_texture '{}': only RGBA8 textures can be sRGB", desc.debug_name);
        return 0;
    }
    if ((levels > 1 || desc.srgb) && !info.features.has(Feature::TextureMips)) {
        LOG_ERROR("create_texture '{}': mip chains and sRGB textures are not available on this device",
                  desc.debug_name);
        return 0;
    }
    if (desc.initial_levels.size() > levels) {
        LOG_ERROR("create_texture '{}': {} levels of data for {} mip levels", desc.debug_name,
                  desc.initial_levels.size(), levels);
        return 0;
    }
    for (std::uint32_t i = 0; i < desc.initial_levels.size(); ++i) {
        const std::size_t expected = static_cast<std::size_t>(mip_extent(desc.width, i)) *
                                     static_cast<std::size_t>(mip_extent(desc.height, i)) *
                                     texture_format_size(desc.format);
        if (!desc.initial_levels[i].empty() && desc.initial_levels[i].size() != expected) {
            LOG_ERROR("create_texture '{}': level {} has {} bytes (expected {})", desc.debug_name, i,
                      desc.initial_levels[i].size(), expected);
            return 0;
        }
    }
    return levels;
}

bool write_ppm(const std::string& path, int width, int height, std::span<const std::uint8_t> rgb) {
    if (rgb.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3) {
        LOG_ERROR("write_ppm: {} bytes for a {}x{} image", rgb.size(), width, height);
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        LOG_ERROR("Cannot write screenshot {}", path);
        return false;
    }
    out << "P6\n" << width << ' ' << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    LOG_INFO("Wrote screenshot {} ({}x{})", path, width, height);
    return static_cast<bool>(out);
}

} // namespace cuervo::rhi::detail

namespace cuervo::rhi::detail {

void apply_tier_gate(DeviceInfo& info, Tier requested) {
    info.tier = std::min(requested, info.max_tier);
    info.features = {};
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(Feature::Count); ++i) {
        const auto f = static_cast<Feature>(i);
        info.features.set(f, info.hardware_features.has(f) && feature_tier(f) <= info.tier);
    }
    if (info.tier == Tier::Compatible) {
        // Behave like the guaranteed web: never more than the tier promises.
        const Limits c = compatible_limits();
        info.limits.max_texture_size = std::min(info.limits.max_texture_size, c.max_texture_size);
        info.limits.max_msaa_samples = std::min(info.limits.max_msaa_samples, c.max_msaa_samples);
        info.limits.max_uniform_block_size = std::min(info.limits.max_uniform_block_size, c.max_uniform_block_size);
        info.limits.max_vertex_attributes = std::min(info.limits.max_vertex_attributes, c.max_vertex_attributes);
        // Alignment is a minimum the caller must honor: larger is stricter.
        info.limits.uniform_offset_alignment = std::max(info.limits.uniform_offset_alignment, c.uniform_offset_alignment);
    }
    info.uniform_offset_alignment = info.limits.uniform_offset_alignment;
}

std::uint32_t msaa_sample_count(std::uint32_t requested, std::uint32_t supported_max) {
    std::uint32_t samples = 1;
    while (samples * 2 <= std::min(requested, supported_max)) samples *= 2;
    return samples;
}

bool feature_allowed(const DeviceInfo& info, Feature feature, bool& reported) {
    if (info.features.has(feature)) return true;
    if (!reported) {
        reported = true;
        if (info.hardware_features.has(feature)) {
            LOG_ERROR("RHI: {} is a {}-tier feature; this device is gated to the {} tier (render_tier in "
                      "project.toml)",
                      feature_name(feature), tier_name(feature_tier(feature)), tier_name(info.tier));
        } else {
            LOG_ERROR("RHI: {} is not available on the {} backend ({} tier)", feature_name(feature),
                      backend_name(info.backend), tier_name(feature_tier(feature)));
        }
    }
    return false;
}

bool texture_size_allowed(const DeviceInfo& info, const TextureDesc& desc) {
    const auto max = static_cast<std::int32_t>(info.limits.max_texture_size);
    if (desc.width > 0 && desc.height > 0 && desc.width <= max && desc.height <= max) return true;
    LOG_ERROR("create_texture '{}': bad size {}x{} (max {}{})", desc.debug_name, desc.width, desc.height, max,
              info.tier == Tier::Compatible ? " on the compatible tier" : "");
    return false;
}

} // namespace cuervo::rhi::detail
