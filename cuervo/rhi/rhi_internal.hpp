#pragma once

// Helpers shared by the RHI backends; not part of the public RHI.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "cuervo/rhi/rhi.hpp"

namespace cuervo::rhi::detail {

// Checks an update_texture call: the region lies inside the texture and
// `data_size` is exactly its tightly packed size. Logs and returns false otherwise.
bool valid_texture_update(std::int32_t width, std::int32_t height, TextureFormat format, Rect region,
                          std::size_t data_size);

// Fills info.tier, info.features and info.limits from the hardware fields
// (max_tier, hardware_features, limits) and the requested tier gate.
void apply_tier_gate(DeviceInfo& info, Tier requested);

// Largest power of two <= min(requested, supported_max), at least 1.
std::uint32_t msaa_sample_count(std::uint32_t requested, std::uint32_t supported_max);

// True if `feature` may be used now; otherwise logs why (once per `reported` flag).
bool feature_allowed(const DeviceInfo& info, Feature feature, bool& reported);

// create_texture's size check against the device's (tier-gated) limit; logs on failure.
bool texture_size_allowed(const DeviceInfo& info, const TextureDesc& desc);

// create_render_target's checks (feature, size within the texture limit); logs on failure.
bool render_target_allowed(const DeviceInfo& info, const RenderTargetDesc& desc);

// Validates a whole TextureDesc (size, mip chain length, sRGB format, per-level
// data sizes, the TextureMips feature) and returns the resolved number of mip
// levels (>= 1), or 0 after logging why the texture is refused.
std::uint32_t texture_levels(const DeviceInfo& info, const TextureDesc& desc);

// Size of mip `level` of a width x height texture.
inline std::int32_t mip_extent(std::int32_t base, std::uint32_t level) {
    const std::int32_t v = base >> level;
    return v > 0 ? v : 1;
}

// Data for `level` from the desc (initial_levels, or initial_data for level 0);
// empty when the level starts zeroed.
std::span<const std::byte> texture_level_data(const TextureDesc& desc, std::uint32_t level);

// Writes tightly packed RGB8 rows (top row first) as a binary PPM.
bool write_ppm(const std::string& path, int width, int height, std::span<const std::uint8_t> rgb);

} // namespace cuervo::rhi::detail
