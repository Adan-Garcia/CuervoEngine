#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "cuervo/input/devices.hpp"
#include "cuervo/math/vec.hpp"

struct SDL_Window;
union SDL_Event;

namespace cuervo {

// Which graphics API the window must be prepared for. Kept here (not in rhi/)
// so the platform layer doesn't depend on the renderer.
enum class GraphicsApi { OpenGL, Vulkan };

struct WindowDesc {
    std::string title = "Cuervo Engine";
    int width = 1280;
    int height = 720;
    GraphicsApi graphics = GraphicsApi::OpenGL;
    bool resizable = true;
    // OpenGL only: request an OpenGL ES 3.0 context instead of desktop GL 3.3
    // core -- what WebGL2 is, so desktop runs can exercise the web's shader
    // path (GLSL ES 300) and limits. Always on in web builds.
    bool gl_es = false;
    // The Cuervo Mark app icon as the window icon (DESIGN.md 4.16, 8.1).
    bool brand_icon = true;
};

// The SDL3 window plus the event pump. One per process.
class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool create(const WindowDesc& desc);
    void destroy();

    // Drain pending OS events into `devices`. Returns false once the user has
    // asked to close the window.
    bool poll_events(DeviceState& devices);
    // One event into `devices` (what poll_events does per event), for hosts
    // that pump events themselves (the editor shares them with ImGui).
    // Returns false for a close request.
    bool handle_event(const SDL_Event& event, DeviceState& devices);

    // Sets the window icon from PNG bytes (SDL 3.4+; a no-op returning false
    // with older SDL). False when the platform has no window icons.
    bool set_icon_png(std::span<const std::uint8_t> png);
    // The embedded Cuervo app icon (create() does this unless desc.brand_icon is off).
    bool set_brand_icon();

    // Asks the OS for text events (SDL text input: IME, layouts, dead keys).
    // The game turns it on while a text field has the focus (UI.wants_text),
    // which also raises the on-screen keyboard on touch devices.
    void set_text_input(bool on);

    ivec2 pixel_size() const;
    GraphicsApi graphics() const { return graphics_; }
    // The GL context requested is OpenGL ES 3.0 (WindowDesc::gl_es or web).
    bool gl_es() const { return gl_es_; }
    SDL_Window* native() const { return window_; }

private:
    SDL_Window* window_ = nullptr;
    GraphicsApi graphics_ = GraphicsApi::OpenGL;
    bool gl_es_ = false;
    bool sdl_initialized_ = false;
};

} // namespace cuervo
