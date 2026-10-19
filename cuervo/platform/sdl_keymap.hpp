#pragma once

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <optional>

#include "cuervo/input/devices.hpp"

namespace cuervo::sdl {

// Physical key position (scancode), so bindings don't move with keyboard layout.
Key to_key(SDL_Scancode scancode);
std::optional<PadButton> to_pad_button(SDL_GamepadButton button);
std::optional<PadAxis> to_pad_axis(SDL_GamepadAxis axis);
std::optional<MouseButton> to_mouse_button(Uint8 button);

} // namespace cuervo::sdl
