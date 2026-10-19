#pragma once

#include <memory>

#include "cuervo/rhi/rhi.hpp"

namespace cuervo::gl {

// OpenGL 3.3 core / GLES 3.0 / WebGL2 backend.
std::unique_ptr<rhi::Device> create_device(Window& window, const rhi::DeviceOptions& options);

} // namespace cuervo::gl
