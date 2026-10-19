#pragma once

#include <memory>

#include "cuervo/rhi/rhi.hpp"

namespace cuervo::vk {

// Vulkan 1.3 backend (dynamic rendering + synchronization2). Desktop only;
// older hardware and the web use the GL backend.
std::unique_ptr<rhi::Device> create_device(Window& window, const rhi::DeviceOptions& options);

} // namespace cuervo::vk
