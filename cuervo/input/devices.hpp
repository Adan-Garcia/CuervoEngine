#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>

#include "cuervo/math/vec.hpp"

namespace cuervo {

// Engine-owned device codes. The platform layer translates SDL codes into
// these, so the input mapping never depends on SDL and can be tested alone.

enum class Key : std::uint16_t {
    Unknown,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Space, Enter, Escape, Tab, Backspace,
    Left, Right, Up, Down,
    LeftShift, RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Minus, Equals, LeftBracket, RightBracket, Backslash, Semicolon, Apostrophe,
    Grave, Comma, Period, Slash,
    Insert, Delete, Home, End, PageUp, PageDown,
    Kp0, Kp1, Kp2, Kp3, Kp4, Kp5, Kp6, Kp7, Kp8, Kp9,
    KpPlus, KpMinus, KpMultiply, KpDivide, KpPeriod,
    Count
};

enum class MouseButton : std::uint8_t {
    Left, Middle, Right, X1, X2,
    Count
};

// Positional names (South = A on Xbox, Cross on PlayStation).
enum class PadButton : std::uint8_t {
    South, East, West, North,
    Back, Guide, Start,
    LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    Count
};

enum class PadAxis : std::uint8_t {
    LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger,
    Count
};

inline constexpr std::size_t key_count = static_cast<std::size_t>(Key::Count);
inline constexpr std::size_t pad_button_count = static_cast<std::size_t>(PadButton::Count);
inline constexpr std::size_t pad_axis_count = static_cast<std::size_t>(PadAxis::Count);
inline constexpr std::size_t mouse_button_count = static_cast<std::size_t>(MouseButton::Count);

// Raw device state, written by the platform layer as events arrive. Phase 0
// merges all connected gamepads into one (single local player).
//
// A press and release can both arrive between two ticks (a quick tap, or a
// slow frame). Presses are therefore latched: a key that went down since the
// last tick reads as down for that tick even if it is already up again, so
// `Input::pressed` never misses a tap. Wheel motion likewise accumulates until
// it is consumed. `InputMap::sample` clears both.
struct DeviceState {
    std::bitset<key_count> keys;
    std::bitset<pad_button_count> pad_buttons;
    std::array<float, pad_axis_count> pad_axes{}; // sticks [-1, 1], triggers [0, 1]

    std::bitset<mouse_button_count> mouse_buttons;
    vec2 mouse_position{0.0f, 0.0f}; // framebuffer pixels, origin top-left
    vec2 wheel{0.0f, 0.0f};          // notches since last consumed; +y = away from the user

    std::bitset<key_count> keys_tapped;          // went down since the last sample
    std::bitset<pad_button_count> buttons_tapped;
    std::bitset<mouse_button_count> mouse_tapped;

    bool key(Key k) const {
        const auto i = static_cast<std::size_t>(k);
        return keys[i] || keys_tapped[i];
    }
    bool button(PadButton b) const {
        const auto i = static_cast<std::size_t>(b);
        return pad_buttons[i] || buttons_tapped[i];
    }
    bool mouse(MouseButton b) const {
        const auto i = static_cast<std::size_t>(b);
        return mouse_buttons[i] || mouse_tapped[i];
    }
    float axis(PadAxis a) const { return pad_axes[static_cast<std::size_t>(a)]; }

    void set_key(Key k, bool down) {
        const auto i = static_cast<std::size_t>(k);
        keys[i] = down;
        if (down) {
            keys_tapped[i] = true;
        }
    }
    void set_button(PadButton b, bool down) {
        const auto i = static_cast<std::size_t>(b);
        pad_buttons[i] = down;
        if (down) {
            buttons_tapped[i] = true;
        }
    }
    void set_mouse_button(MouseButton b, bool down) {
        const auto i = static_cast<std::size_t>(b);
        mouse_buttons[i] = down;
        if (down) {
            mouse_tapped[i] = true;
        }
    }
    void set_axis(PadAxis a, float value) { pad_axes[static_cast<std::size_t>(a)] = value; }

    // For code that reads the wheel directly (editor, viewers) rather than
    // through an InputMap: returns the accumulated motion and resets it.
    vec2 take_wheel() {
        const vec2 motion = wheel;
        wheel = vec2(0.0f);
        return motion;
    }

    void clear_transients() {
        keys_tapped.reset();
        buttons_tapped.reset();
        mouse_tapped.reset();
        wheel = vec2(0.0f);
    }
};

} // namespace cuervo
