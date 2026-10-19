#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <span>
#include <type_traits>
#include <vector>

#include "cuervo/core/name.hpp"
#include "cuervo/input/devices.hpp"

namespace cuervo {

// One physical input that can drive an action or one side of an axis.
struct Binding {
    enum class Source : std::uint8_t {
        Key, PadButton, PadAxisPositive, PadAxisNegative, MouseButton, WheelUp, WheelDown
    };
    Source source = Source::Key;
    std::uint16_t code = 0;

    static constexpr Binding key(Key k) { return {Source::Key, static_cast<std::uint16_t>(k)}; }
    static constexpr Binding button(PadButton b) { return {Source::PadButton, static_cast<std::uint16_t>(b)}; }
    // An analog axis pushed past the threshold in the given direction.
    static constexpr Binding axis_positive(PadAxis a) { return {Source::PadAxisPositive, static_cast<std::uint16_t>(a)}; }
    static constexpr Binding axis_negative(PadAxis a) { return {Source::PadAxisNegative, static_cast<std::uint16_t>(a)}; }
    static constexpr Binding mouse(MouseButton b) { return {Source::MouseButton, static_cast<std::uint16_t>(b)}; }
    // Held for the tick in which the wheel moved in that direction.
    static constexpr Binding wheel_up() { return {Source::WheelUp, 0}; }
    static constexpr Binding wheel_down() { return {Source::WheelDown, 0}; }
};

// A source for a 1D axis value in [-1, 1].
struct AxisSource {
    enum class Kind : std::uint8_t { Digital, Analog, Wheel };
    Kind kind = Kind::Digital;
    Binding negative{}; // Digital: held -> -1
    Binding positive{}; // Digital: held -> +1
    PadAxis analog{};   // Analog: stick/trigger value with dead zone

    static constexpr AxisSource digital(Binding neg, Binding pos) { return {Kind::Digital, neg, pos, {}}; }
    static constexpr AxisSource stick(PadAxis a) { return {Kind::Analog, {}, {}, a}; }
    // Vertical wheel motion this tick, clamped to [-1, 1].
    static constexpr AxisSource wheel() { return {Kind::Wheel, {}, {}, {}}; }
};

// One tick of mapped input (DESIGN.md 4.8). Plain data with no pointers, so a
// replay is just `seed + std::vector<InputFrame>`. Axis values are quantized
// to i16 at sampling time so playback reproduces them exactly.
struct InputFrame {
    static constexpr std::size_t max_actions = 64;
    static constexpr std::size_t max_axes = 8;

    std::uint64_t held = 0; // bit i = action i is held
    std::array<std::int16_t, max_axes> axes{};
    // The mouse pointer in whole virtual-space pixels (UI hit tests,
    // DESIGN.md 4.10). Filled in by the game runner, which knows the
    // viewport mapping; recorded like everything else so replays click the
    // same buttons.
    std::array<std::int16_t, 2> pointer{};

    friend bool operator==(const InputFrame&, const InputFrame&) = default;
};
static_assert(std::is_trivially_copyable_v<InputFrame>);
static_assert(sizeof(InputFrame) == 32);

// Project input map: named actions and axes and their bindings.
class InputMap {
public:
    float analog_dead_zone = 0.2f;   // analog axes below this read as 0
    float digital_threshold = 0.5f;  // analog axes beyond this count as "held"

    // Adds or replaces an action. Returns false if the 64-action limit is hit.
    bool set_action(Name name, std::initializer_list<Binding> bindings);
    bool set_action(Name name, std::span<const Binding> bindings); // bindings read from project files
    // Adds or replaces an axis. Returns false if the 8-axis limit is hit.
    bool set_axis(Name name, std::initializer_list<AxisSource> sources);
    bool set_axis(Name name, std::span<const AxisSource> sources);

    std::optional<std::size_t> action_index(Name name) const;
    std::optional<std::size_t> axis_index(Name name) const;
    // Names in index order (bit i of InputFrame::held is action i); replays
    // store them to detect a project whose actions moved.
    std::vector<std::string> action_names() const;
    std::vector<std::string> axis_names() const;

    // Turn raw device state into one tick of mapped input, then clear the
    // device's latched taps and wheel motion so each counts for exactly one tick.
    InputFrame sample(DeviceState& devices) const;

private:
    struct Action { Name name; std::vector<Binding> bindings; };
    struct Axis { Name name; std::vector<AxisSource> sources; };

    bool binding_held(const DeviceState& devices, Binding binding) const;
    float axis_value(const DeviceState& devices, const AxisSource& source) const;

    std::vector<Action> actions_;
    std::vector<Axis> axes_;
};

// What gameplay code queries: the current and previous frame of mapped input.
class Input {
public:
    explicit Input(const InputMap& map) : map_(&map) {}

    // Advance one tick. Feed either live samples or recorded frames.
    void push(const InputFrame& frame) { previous_ = current_; current_ = frame; }

    bool held(Name action) const;
    bool pressed(Name action) const;   // went down this tick
    bool released(Name action) const;  // went up this tick
    float axis(Name axis) const;       // [-1, 1]

    const InputFrame& frame() const { return current_; }

private:
    static bool bit(const InputFrame& frame, std::size_t index) { return (frame.held >> index) & 1u; }

    const InputMap* map_;
    InputFrame current_{};
    InputFrame previous_{};
};

} // namespace cuervo
