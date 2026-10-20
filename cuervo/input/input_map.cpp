#include "cuervo/input/input_map.hpp"

#include <algorithm>
#include <cmath>

namespace cuervo {

bool InputMap::set_action(Name name, std::initializer_list<Binding> bindings) {
    return set_action(name, std::span<const Binding>(bindings.begin(), bindings.size()));
}

bool InputMap::set_action(Name name, std::span<const Binding> bindings) {
    if (auto index = action_index(name)) {
        actions_[*index].bindings.assign(bindings.begin(), bindings.end());
        return true;
    }
    if (actions_.size() == InputFrame::max_actions) {
        return false;
    }
    actions_.push_back({name, {bindings.begin(), bindings.end()}});
    return true;
}

bool InputMap::set_axis(Name name, std::initializer_list<AxisSource> sources) {
    return set_axis(name, std::span<const AxisSource>(sources.begin(), sources.size()));
}

bool InputMap::set_axis(Name name, std::span<const AxisSource> sources) {
    if (auto index = axis_index(name)) {
        axes_[*index].sources.assign(sources.begin(), sources.end());
        return true;
    }
    if (axes_.size() == InputFrame::max_axes) {
        return false;
    }
    axes_.push_back({name, {sources.begin(), sources.end()}});
    return true;
}

std::optional<std::size_t> InputMap::action_index(Name name) const {
    for (std::size_t i = 0; i < actions_.size(); ++i) {
        if (actions_[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

std::vector<std::string> InputMap::action_names() const {
    std::vector<std::string> out;
    for (const Action& a : actions_) out.emplace_back(a.name.str());
    return out;
}

std::vector<std::string> InputMap::axis_names() const {
    std::vector<std::string> out;
    for (const Axis& a : axes_) out.emplace_back(a.name.str());
    return out;
}

std::optional<std::size_t> InputMap::axis_index(Name name) const {
    for (std::size_t i = 0; i < axes_.size(); ++i) {
        if (axes_[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

bool InputMap::binding_held(const DeviceState& devices, Binding binding) const {
    switch (binding.source) {
        case Binding::Source::Key:
            return devices.key(static_cast<Key>(binding.code));
        case Binding::Source::PadButton:
            return devices.button(static_cast<PadButton>(binding.code));
        case Binding::Source::PadAxisPositive:
            return devices.axis(static_cast<PadAxis>(binding.code)) >= digital_threshold;
        case Binding::Source::PadAxisNegative:
            return devices.axis(static_cast<PadAxis>(binding.code)) <= -digital_threshold;
        case Binding::Source::MouseButton:
            return devices.mouse(static_cast<MouseButton>(binding.code));
        case Binding::Source::WheelUp:
            return devices.wheel.y() > 0.0f;
        case Binding::Source::WheelDown:
            return devices.wheel.y() < 0.0f;
    }
    return false;
}

float InputMap::axis_value(const DeviceState& devices, const AxisSource& source) const {
    if (source.kind == AxisSource::Kind::Digital) {
        float value = 0.0f;
        if (binding_held(devices, source.negative)) value -= 1.0f;
        if (binding_held(devices, source.positive)) value += 1.0f;
        return value;
    }
    if (source.kind == AxisSource::Kind::Wheel) {
        return std::clamp(devices.wheel.y(), -1.0f, 1.0f);
    }
    float value = devices.axis(source.analog);
    if (std::fabs(value) < analog_dead_zone) {
        return 0.0f;
    }
    // Rescale so the output starts at 0 just past the dead zone.
    float magnitude = (std::fabs(value) - analog_dead_zone) / (1.0f - analog_dead_zone);
    return std::copysign(std::min(magnitude, 1.0f), value);
}

InputFrame InputMap::sample(DeviceState& devices) const {
    InputFrame frame;
    for (std::size_t i = 0; i < actions_.size(); ++i) {
        for (const Binding& binding : actions_[i].bindings) {
            if (binding_held(devices, binding)) {
                frame.held |= std::uint64_t{1} << i;
                break;
            }
        }
    }
    for (std::size_t i = 0; i < axes_.size(); ++i) {
        // The source pushed furthest wins (keyboard and stick don't add up).
        float best = 0.0f;
        for (const AxisSource& source : axes_[i].sources) {
            float value = axis_value(devices, source);
            if (std::fabs(value) > std::fabs(best)) {
                best = value;
            }
        }
        frame.axes[i] = static_cast<std::int16_t>(std::lround(std::clamp(best, -1.0f, 1.0f) * 32767.0f));
    }
    if (!devices.typed.empty()) {
        frame.text = static_cast<std::uint32_t>(devices.typed.front());
        devices.typed.erase(0, 1);
    }
    devices.clear_transients();
    return frame;
}

bool Input::held(Name action) const {
    auto index = map_->action_index(action);
    return index && bit(current_, *index);
}

bool Input::pressed(Name action) const {
    auto index = map_->action_index(action);
    return index && bit(current_, *index) && !bit(previous_, *index);
}

bool Input::released(Name action) const {
    auto index = map_->action_index(action);
    return index && !bit(current_, *index) && bit(previous_, *index);
}

float Input::axis(Name axis) const {
    auto index = map_->axis_index(axis);
    return index ? static_cast<float>(current_.axes[*index]) / 32767.0f : 0.0f;
}

} // namespace cuervo
