#pragma once

#include <cstddef>
#include <memory>

#include "cuervo/rhi/rhi.hpp"

namespace cuervo::webgpu {

// WebGPU backend (DESIGN.md 6, milestone 10; experimental): wgpu-native on
// desktop, Emscripten's emdawnwebgpu in the browser. Compatible tier only.
// Without a native window surface (SDL's offscreen video driver) it renders
// into an offscreen target, so captures work headless.
std::unique_ptr<rhi::Device> create_device(Window& window, const rhi::DeviceOptions& options);

// Frame limiting, as gl::peak_frames_in_flight / gl::frame_limit: the most
// submitted frames the device let queue so far (0 for a device that is not a
// WebGPU one, in the browser, which paces frames itself, or in a build without
// the backend) and the bound.
std::size_t peak_frames_in_flight(const rhi::Device& device);
std::size_t frame_limit();

#ifdef __EMSCRIPTEN__
// Whether the page requested a WebGPU adapter and device before main()
// (web/webgpu_init.js, `Module.preinitializedWebGPUDevice`). The browser's
// requests are promises, which a WASM main can't wait for without JSPI, so
// the page makes them; without a device the player uses WebGL2.
bool browser_device_ready();
#endif

} // namespace cuervo::webgpu
