#pragma once

#include <cstddef>
#include <memory>

#include "cuervo/rhi/rhi.hpp"

namespace cuervo::gl {

// OpenGL 3.3 core / GLES 3.0 / WebGL2 backend.
std::unique_ptr<rhi::Device> create_device(Window& window, const rhi::DeviceOptions& options);

// Frame limiting (a fence per presented frame; see rhi/frame_fence_ring.hpp):
// the most submitted frames the device let queue so far (0 for a device that
// is not a GL one, or on the web build, which keeps no fences) and the bound.
std::size_t peak_frames_in_flight(const rhi::Device& device);
std::size_t frame_limit();

} // namespace cuervo::gl
