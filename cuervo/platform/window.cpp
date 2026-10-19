#include "cuervo/platform/window.hpp"

#include <SDL3/SDL.h>

#include <algorithm>

#include "cuervo/brand/brand.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/platform/sdl_keymap.hpp"

namespace cuervo {

Window::~Window() { destroy(); }

bool Window::create(const WindowDesc& desc) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    sdl_initialized_ = true;
    graphics_ = desc.graphics;

    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    switch (desc.graphics) {
        case GraphicsApi::OpenGL:
            gl_es_ = desc.gl_es || platform::os == platform::Os::Web;
            if (gl_es_) {
                // GLES 3.0 = WebGL2 in the browser (Emscripten maps it), or a
                // desktop driver's ES context (EGL/GLX_EXT_create_context_es2_profile).
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            } else {
                // GL 3.3 core: the desktop twin of GLES 3.0 / WebGL2 (DESIGN.md 4.4).
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            }
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
            // No multisampled default framebuffer: the RHI resolves its own MSAA
            // target into it with glBlitFramebuffer, which GLES/WebGL2 forbid
            // into a multisampled destination (WebGL: antialias = false).
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
            SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, 0);
            flags |= SDL_WINDOW_OPENGL;
            break;
        case GraphicsApi::Vulkan:
            flags |= SDL_WINDOW_VULKAN;
            break;
    }

    window_ = SDL_CreateWindow(desc.title.c_str(), desc.width, desc.height, flags);
    if (!window_) {
        LOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }
    if (desc.brand_icon) set_brand_icon();
    LOG_INFO("Window created ({}x{}, video driver: {}{})", desc.width, desc.height, SDL_GetCurrentVideoDriver(),
             desc.graphics == GraphicsApi::OpenGL ? (gl_es_ ? ", OpenGL ES 3.0" : ", OpenGL 3.3 core") : "");
    return true;
}

bool Window::set_icon_png(std::span<const std::uint8_t> png) {
    if (!window_) return false;
#if SDL_VERSION_ATLEAST(3, 4, 0)
    SDL_IOStream* io = SDL_IOFromConstMem(png.data(), png.size());
    SDL_Surface* icon = io ? SDL_LoadPNG_IO(io, true) : nullptr;
    if (!icon) {
        LOG_WARN("Window icon: can't decode the PNG: {}", SDL_GetError());
        return false;
    }
    const bool ok = SDL_SetWindowIcon(window_, icon);
    SDL_DestroySurface(icon);
    // Not every platform has window icons (Wayland compositors without
    // xdg-toplevel-icon, the web): not an error.
    if (!ok) LOG_DEBUG("Window icon not set: {}", SDL_GetError());
    return ok;
#else
    // SDL_LoadPNG arrived in SDL 3.4; the fetched fallback (3.2.10) has no PNG loader.
    (void)png;
    return false;
#endif
}

bool Window::set_brand_icon() { return set_icon_png(brand::app_icon_png()); }

void Window::destroy() {
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    if (sdl_initialized_) {
        SDL_Quit();
        sdl_initialized_ = false;
    }
}

bool Window::poll_events(DeviceState& devices) {
    bool keep_running = true;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (!handle_event(event, devices)) keep_running = false;
    }
    return keep_running;
}

bool Window::handle_event(const SDL_Event& event, DeviceState& devices) {
    bool keep_running = true;
    {
        switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                keep_running = false;
                break;

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (Key key = sdl::to_key(event.key.scancode); key != Key::Unknown) {
                    devices.set_key(key, event.type == SDL_EVENT_KEY_DOWN);
                }
                break;

            // SDL reports mouse coordinates in window points; the engine works in
            // framebuffer pixels (what Camera2D/ViewportFit consume).
            case SDL_EVENT_MOUSE_MOTION:
                devices.mouse_position = vec2(event.motion.x, event.motion.y) * SDL_GetWindowPixelDensity(window_);
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (auto button = sdl::to_mouse_button(event.button.button)) {
                    devices.set_mouse_button(*button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                }
                devices.mouse_position = vec2(event.button.x, event.button.y) * SDL_GetWindowPixelDensity(window_);
                break;

            case SDL_EVENT_MOUSE_WHEEL: {
                const float sign = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
                devices.wheel += vec2(event.wheel.x, event.wheel.y) * sign;
                devices.mouse_position = vec2(event.wheel.mouse_x, event.wheel.mouse_y) * SDL_GetWindowPixelDensity(window_);
                break;
            }

            case SDL_EVENT_WINDOW_FOCUS_LOST:
                // Keys released while unfocused never send key-up events.
                devices.keys.reset();
                devices.mouse_buttons.reset();
                break;

            case SDL_EVENT_GAMEPAD_ADDED:
                if (SDL_Gamepad* pad = SDL_OpenGamepad(event.gdevice.which)) {
                    LOG_INFO("Gamepad connected: {}", SDL_GetGamepadName(pad));
                }
                break;

            case SDL_EVENT_GAMEPAD_REMOVED:
                if (SDL_Gamepad* pad = SDL_GetGamepadFromID(event.gdevice.which)) {
                    SDL_CloseGamepad(pad);
                }
                devices.pad_buttons.reset();
                devices.buttons_tapped.reset();
                devices.pad_axes.fill(0.0f);
                LOG_INFO("Gamepad disconnected");
                break;

            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (auto button = sdl::to_pad_button(static_cast<SDL_GamepadButton>(event.gbutton.button))) {
                    devices.set_button(*button, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                }
                break;

            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (auto axis = sdl::to_pad_axis(static_cast<SDL_GamepadAxis>(event.gaxis.axis))) {
                    float value = std::max(static_cast<float>(event.gaxis.value) / 32767.0f, -1.0f);
                    devices.set_axis(*axis, value);
                }
                break;

            default:
                break;
        }
    }
    return keep_running;
}

ivec2 Window::pixel_size() const {
    int w = 0;
    int h = 0;
    if (window_) {
        SDL_GetWindowSizeInPixels(window_, &w, &h);
    }
    return {w, h};
}

} // namespace cuervo
