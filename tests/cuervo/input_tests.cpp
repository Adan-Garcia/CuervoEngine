#include <doctest.h>

#include <vector>

#include "cuervo/input/input_map.hpp"

using namespace cuervo;

namespace {

InputMap make_map() {
    InputMap map;
    map.set_action("shoot"_name, {Binding::key(Key::Z), Binding::button(PadButton::South)});
    map.set_action("focus"_name, {Binding::key(Key::LeftShift), Binding::axis_positive(PadAxis::RightTrigger)});
    map.set_axis("move_x"_name, {AxisSource::digital(Binding::key(Key::Left), Binding::key(Key::Right)),
                                 AxisSource::stick(PadAxis::LeftX)});
    return map;
}

} // namespace

TEST_CASE("Actions are held when any binding is active") {
    InputMap map = make_map();
    DeviceState devices;
    CHECK(map.sample(devices).held == 0);

    devices.set_key(Key::Z, true);
    CHECK(map.sample(devices).held == 0b01);

    devices.set_key(Key::Z, false);
    devices.set_button(PadButton::South, true);
    CHECK(map.sample(devices).held == 0b01);

    devices.set_axis(PadAxis::RightTrigger, 0.8f);
    CHECK(map.sample(devices).held == 0b11);
}

TEST_CASE("pressed and released compare against the previous frame") {
    InputMap map = make_map();
    Input input(map);
    DeviceState devices;

    devices.set_key(Key::Z, true);
    input.push(map.sample(devices));
    CHECK(input.pressed("shoot"_name));
    CHECK(input.held("shoot"_name));

    input.push(map.sample(devices));
    CHECK_FALSE(input.pressed("shoot"_name));
    CHECK(input.held("shoot"_name));

    devices.set_key(Key::Z, false);
    input.push(map.sample(devices));
    CHECK(input.released("shoot"_name));
    CHECK_FALSE(input.held("shoot"_name));

    CHECK_FALSE(input.held("unmapped"_name));
}

TEST_CASE("Axes: digital, analog dead zone, strongest source wins") {
    InputMap map = make_map();
    Input input(map);
    DeviceState devices;

    devices.set_key(Key::Right, true);
    input.push(map.sample(devices));
    CHECK(input.axis("move_x"_name) == 1.0f);

    devices.set_key(Key::Left, true); // both directions cancel out
    input.push(map.sample(devices));
    CHECK(input.axis("move_x"_name) == 0.0f);

    devices.set_key(Key::Left, false);
    devices.set_key(Key::Right, false);
    devices.set_axis(PadAxis::LeftX, 0.1f); // inside the dead zone
    input.push(map.sample(devices));
    CHECK(input.axis("move_x"_name) == 0.0f);

    devices.set_axis(PadAxis::LeftX, -0.6f);
    input.push(map.sample(devices));
    CHECK(input.axis("move_x"_name) == doctest::Approx(-0.5f).epsilon(1e-3));
}

TEST_CASE("Recorded frames replay identically") {
    InputMap map = make_map();
    DeviceState devices;
    std::vector<InputFrame> recording;

    for (int tick = 0; tick < 10; ++tick) {
        devices.set_key(Key::Z, tick % 3 == 0);
        devices.set_axis(PadAxis::LeftX, static_cast<float>(tick) * 0.1f - 0.5f);
        recording.push_back(map.sample(devices));
    }

    Input live(map);
    Input replay(map);
    devices = {};
    for (int tick = 0; tick < 10; ++tick) {
        devices.set_key(Key::Z, tick % 3 == 0);
        devices.set_axis(PadAxis::LeftX, static_cast<float>(tick) * 0.1f - 0.5f);
        live.push(map.sample(devices));
        replay.push(recording[static_cast<std::size_t>(tick)]);
        CHECK(live.frame() == replay.frame());
        CHECK(live.pressed("shoot"_name) == replay.pressed("shoot"_name));
    }
}

TEST_CASE("Action limit is enforced") {
    InputMap map;
    for (std::size_t i = 0; i < InputFrame::max_actions; ++i) {
        CHECK(map.set_action(Name("action" + std::to_string(i)), {Binding::key(Key::A)}));
    }
    CHECK_FALSE(map.set_action("one_too_many"_name, {Binding::key(Key::A)}));
    CHECK(map.set_action("action0"_name, {Binding::key(Key::B)})); // replacing is fine
}

TEST_CASE("A tap shorter than one tick still registers as one press") {
    InputMap map;
    map.set_action("jump"_name, {Binding::key(Key::Space), Binding::button(PadButton::South)});
    Input input(map);
    DeviceState devices;

    // Key goes down and up between two ticks (fast tap or slow frame).
    devices.set_key(Key::Space, true);
    devices.set_key(Key::Space, false);
    input.push(map.sample(devices));
    CHECK(input.pressed("jump"_name));

    // The tap is consumed: the next tick sees the key released.
    input.push(map.sample(devices));
    CHECK_FALSE(input.held("jump"_name));
    CHECK(input.released("jump"_name));

    // Same for gamepad buttons.
    devices.set_button(PadButton::South, true);
    devices.set_button(PadButton::South, false);
    input.push(map.sample(devices));
    CHECK(input.pressed("jump"_name));
}

TEST_CASE("A tap survives frames that run zero ticks") {
    InputMap map;
    map.set_action("jump"_name, {Binding::key(Key::Space)});
    DeviceState devices;

    devices.set_key(Key::Space, true);
    devices.set_key(Key::Space, false);
    // No sample happens this frame; the latch must persist until the next tick.
    CHECK(devices.key(Key::Space));
    CHECK(map.sample(devices).held == 1);
    CHECK(map.sample(devices).held == 0);
}

TEST_CASE("Mouse buttons bind to actions and latch taps") {
    InputMap map;
    map.set_action("fire"_name, {Binding::mouse(MouseButton::Left)});
    Input input(map);
    DeviceState devices;

    devices.set_mouse_button(MouseButton::Left, true);
    input.push(map.sample(devices));
    CHECK(input.pressed("fire"_name));

    devices.set_mouse_button(MouseButton::Left, false);
    input.push(map.sample(devices));
    CHECK(input.released("fire"_name));

    devices.set_mouse_button(MouseButton::Left, true);
    devices.set_mouse_button(MouseButton::Left, false);
    input.push(map.sample(devices));
    CHECK(input.pressed("fire"_name));
}

TEST_CASE("Wheel motion drives actions and axes for exactly one tick") {
    InputMap map;
    map.set_action("next"_name, {Binding::wheel_up()});
    map.set_action("prev"_name, {Binding::wheel_down()});
    map.set_axis("zoom"_name, {AxisSource::wheel()});
    Input input(map);
    DeviceState devices;

    devices.wheel += vec2(0.0f, 3.0f);
    input.push(map.sample(devices));
    CHECK(input.pressed("next"_name));
    CHECK_FALSE(input.held("prev"_name));
    CHECK(input.axis("zoom"_name) == doctest::Approx(1.0f)); // clamped

    input.push(map.sample(devices)); // consumed
    CHECK_FALSE(input.held("next"_name));
    CHECK(input.axis("zoom"_name) == 0.0f);

    devices.wheel += vec2(0.0f, -0.5f);
    input.push(map.sample(devices));
    CHECK(input.held("prev"_name));
    CHECK(input.axis("zoom"_name) == doctest::Approx(-0.5f).epsilon(1e-3));
}

TEST_CASE("take_wheel returns and resets accumulated motion") {
    DeviceState devices;
    devices.wheel += vec2(1.0f, 2.0f);
    devices.wheel += vec2(0.0f, 1.0f);
    const vec2 motion = devices.take_wheel();
    CHECK(motion.x() == 1.0f);
    CHECK(motion.y() == 3.0f);
    CHECK(devices.take_wheel().y() == 0.0f);
}
