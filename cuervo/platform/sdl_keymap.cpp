#include "cuervo/platform/sdl_keymap.hpp"

namespace cuervo::sdl {

Key to_key(SDL_Scancode scancode) {
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
        return static_cast<Key>(static_cast<int>(Key::A) + (scancode - SDL_SCANCODE_A));
    }
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
        return static_cast<Key>(static_cast<int>(Key::Num1) + (scancode - SDL_SCANCODE_1));
    }
    if (scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_9) {
        return static_cast<Key>(static_cast<int>(Key::Kp1) + (scancode - SDL_SCANCODE_KP_1));
    }
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) {
        return static_cast<Key>(static_cast<int>(Key::F1) + (scancode - SDL_SCANCODE_F1));
    }
    switch (scancode) {
        case SDL_SCANCODE_0:         return Key::Num0;
        case SDL_SCANCODE_SPACE:     return Key::Space;
        case SDL_SCANCODE_RETURN:    return Key::Enter;
        case SDL_SCANCODE_KP_ENTER:  return Key::Enter;
        case SDL_SCANCODE_ESCAPE:    return Key::Escape;
        case SDL_SCANCODE_TAB:       return Key::Tab;
        case SDL_SCANCODE_BACKSPACE: return Key::Backspace;
        case SDL_SCANCODE_LEFT:      return Key::Left;
        case SDL_SCANCODE_RIGHT:     return Key::Right;
        case SDL_SCANCODE_UP:        return Key::Up;
        case SDL_SCANCODE_DOWN:      return Key::Down;
        case SDL_SCANCODE_LSHIFT:    return Key::LeftShift;
        case SDL_SCANCODE_RSHIFT:    return Key::RightShift;
        case SDL_SCANCODE_LCTRL:     return Key::LeftCtrl;
        case SDL_SCANCODE_RCTRL:     return Key::RightCtrl;
        case SDL_SCANCODE_LALT:      return Key::LeftAlt;
        case SDL_SCANCODE_RALT:      return Key::RightAlt;
        case SDL_SCANCODE_MINUS:        return Key::Minus;
        case SDL_SCANCODE_EQUALS:       return Key::Equals;
        case SDL_SCANCODE_LEFTBRACKET:  return Key::LeftBracket;
        case SDL_SCANCODE_RIGHTBRACKET: return Key::RightBracket;
        case SDL_SCANCODE_BACKSLASH:    return Key::Backslash;
        case SDL_SCANCODE_SEMICOLON:    return Key::Semicolon;
        case SDL_SCANCODE_APOSTROPHE:   return Key::Apostrophe;
        case SDL_SCANCODE_GRAVE:        return Key::Grave;
        case SDL_SCANCODE_COMMA:        return Key::Comma;
        case SDL_SCANCODE_PERIOD:       return Key::Period;
        case SDL_SCANCODE_SLASH:        return Key::Slash;
        case SDL_SCANCODE_INSERT:       return Key::Insert;
        case SDL_SCANCODE_DELETE:       return Key::Delete;
        case SDL_SCANCODE_HOME:         return Key::Home;
        case SDL_SCANCODE_END:          return Key::End;
        case SDL_SCANCODE_PAGEUP:       return Key::PageUp;
        case SDL_SCANCODE_PAGEDOWN:     return Key::PageDown;
        case SDL_SCANCODE_KP_0:         return Key::Kp0;
        case SDL_SCANCODE_KP_PLUS:      return Key::KpPlus;
        case SDL_SCANCODE_KP_MINUS:     return Key::KpMinus;
        case SDL_SCANCODE_KP_MULTIPLY:  return Key::KpMultiply;
        case SDL_SCANCODE_KP_DIVIDE:    return Key::KpDivide;
        case SDL_SCANCODE_KP_PERIOD:    return Key::KpPeriod;
        default:                     return Key::Unknown;
    }
}

std::optional<PadButton> to_pad_button(SDL_GamepadButton button) {
    switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH:          return PadButton::South;
        case SDL_GAMEPAD_BUTTON_EAST:           return PadButton::East;
        case SDL_GAMEPAD_BUTTON_WEST:           return PadButton::West;
        case SDL_GAMEPAD_BUTTON_NORTH:          return PadButton::North;
        case SDL_GAMEPAD_BUTTON_BACK:           return PadButton::Back;
        case SDL_GAMEPAD_BUTTON_GUIDE:          return PadButton::Guide;
        case SDL_GAMEPAD_BUTTON_START:          return PadButton::Start;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return PadButton::LeftStick;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return PadButton::RightStick;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return PadButton::LeftShoulder;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return PadButton::RightShoulder;
        case SDL_GAMEPAD_BUTTON_DPAD_UP:        return PadButton::DpadUp;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return PadButton::DpadDown;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return PadButton::DpadLeft;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return PadButton::DpadRight;
        default:                                return std::nullopt;
    }
}

std::optional<PadAxis> to_pad_axis(SDL_GamepadAxis axis) {
    switch (axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:         return PadAxis::LeftX;
        case SDL_GAMEPAD_AXIS_LEFTY:         return PadAxis::LeftY;
        case SDL_GAMEPAD_AXIS_RIGHTX:        return PadAxis::RightX;
        case SDL_GAMEPAD_AXIS_RIGHTY:        return PadAxis::RightY;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:  return PadAxis::LeftTrigger;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return PadAxis::RightTrigger;
        default:                             return std::nullopt;
    }
}

std::optional<MouseButton> to_mouse_button(Uint8 button) {
    switch (button) {
        case SDL_BUTTON_LEFT:   return MouseButton::Left;
        case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
        case SDL_BUTTON_RIGHT:  return MouseButton::Right;
        case SDL_BUTTON_X1:     return MouseButton::X1;
        case SDL_BUTTON_X2:     return MouseButton::X2;
        default:                return std::nullopt;
    }
}

} // namespace cuervo::sdl
