#include "cuervo/script/save_bindings.hpp"

#include <algorithm>
#include <format>

#include "cuervo/core/log.hpp"

namespace cuervo::script {

namespace {

namespace nv = pluma::native;
using pluma::NativeCall;
using pluma::Value;

#if CUERVO_FEATURE_AUDIO
// Forwards the audio part of Settings to an AudioSystem.
class AudioTarget final : public save::SettingsTarget {
public:
    explicit AudioTarget(audio::AudioSystem& a) : audio_(a) {}
    void set_bus_volume(std::size_t bus, float volume) override {
        if (bus < audio::bus_count) audio_.set_bus_volume(static_cast<audio::Bus>(bus), volume);
    }
    void set_muted(bool on) override { audio_.set_bus_muted(audio::Bus::Master, on); }

private:
    audio::AudioSystem& audio_;
};
#endif

std::string slot_arg(NativeCall& c, std::uint32_t i) {
    const std::string_view s = nv::str(c, i);
    if (!save::valid_slot_name(s)) {
        c.vm.panic(std::format("Save: bad slot name '{}' (1-64 letters, digits, '_' or '-')", s));
    }
    return std::string(s);
}

std::size_t bus_index(NativeCall& c, std::uint32_t i) {
    const std::string_view text = nv::name(c, i);
    for (std::size_t b = 0; b < save::settings_bus_count; ++b) {
        if (text == save::settings_bus_names[b]) return b;
    }
    c.vm.panic(std::format("Settings: unknown bus #{} (#master, #music, #sfx, #ui, #voice)", text));
    return 0;
}

Value i32(std::int64_t v) { return Value::integer(pluma::NumKind::I32, v); }

} // namespace

save::SaveStore& SaveContext::store() {
    if (!store_) store_ = std::make_shared<save::SaveStore>(save::SaveStore::for_game(game_));
    return *store_;
}

int SaveContext::apply() {
    int refused = 0;
#if CUERVO_FEATURE_AUDIO
    if (audio != nullptr) {
        AudioTarget a(*audio);
        save::apply_settings(settings, a);
    }
#endif
    if (target != nullptr) refused = save::apply_settings(settings, *target);
    return refused;
}

void bind_save(pluma::Bindings& b, SaveContext& ctx) {
    SaveContext* s = &ctx;

    b.add_native("Save.load", [s](NativeCall& c) {
        const std::string slot = slot_arg(c, 0);
        std::string error;
        save::LoadInfo info;
        auto data = s->store().load(slot, error, &info);
        if (!data) {
            if (error.find("no slot") == std::string::npos) LOG_WARN("Save.load('{}'): {}", slot, error);
            return Value::boolean(false);
        }
        s->open[slot] = {std::move(*data), info.version};
        return Value::boolean(true);
    });
    b.add_native("Save.commit", [s](NativeCall& c) {
        const std::string slot = slot_arg(c, 0);
        const auto version = static_cast<std::uint32_t>(std::max<std::int64_t>(nv::integer(c, 1), 1));
        SaveContext::Open& open = s->open[slot];
        std::string error;
        if (!s->store().save_versioned(slot, open.data, version, &error)) {
            LOG_WARN("Save.commit('{}'): {}", slot, error);
            return Value::boolean(false);
        }
        open.version = version;
        return Value::boolean(true);
    });
    b.add_native("Save.exists", [s](NativeCall& c) { return Value::boolean(s->store().exists(slot_arg(c, 0))); });
    b.add_native("Save.version", [s](NativeCall& c) {
        const auto it = s->open.find(slot_arg(c, 0));
        return i32(it == s->open.end() ? 0 : it->second.version);
    });
    b.add_native("Save.delete", [s](NativeCall& c) {
        const std::string slot = slot_arg(c, 0);
        s->open.erase(slot);
        return Value::boolean(s->store().remove(slot));
    });
    b.add_native("Save.discard", [s](NativeCall& c) {
        s->open.erase(slot_arg(c, 0));
        return Value{};
    });
    b.add_native("Save.has", [s](NativeCall& c) {
        const auto it = s->open.find(slot_arg(c, 0));
        return Value::boolean(it != s->open.end() && it->second.data.find(nv::str(c, 1)) != nullptr);
    });
    b.add_native("Save.remove_key", [s](NativeCall& c) {
        const auto it = s->open.find(slot_arg(c, 0));
        if (it != s->open.end()) {
            const auto field = it->second.data.fields.find(nv::str(c, 1));
            if (field != it->second.data.fields.end()) it->second.data.fields.erase(field);
        }
        return Value{};
    });

    b.add_native("Save.set_int", [s](NativeCall& c) {
        s->open[slot_arg(c, 0)].data.set(std::string(nv::str(c, 1)), nv::integer(c, 2));
        return Value{};
    });
    b.add_native("Save.set_float", [s](NativeCall& c) {
        s->open[slot_arg(c, 0)].data.set(std::string(nv::str(c, 1)), static_cast<double>(nv::f32(c, 2)));
        return Value{};
    });
    b.add_native("Save.set_bool", [s](NativeCall& c) {
        s->open[slot_arg(c, 0)].data.set(std::string(nv::str(c, 1)), nv::boolean(c, 2));
        return Value{};
    });
    b.add_native("Save.set_str", [s](NativeCall& c) {
        s->open[slot_arg(c, 0)].data.set(std::string(nv::str(c, 1)), std::string(nv::str(c, 2)));
        return Value{};
    });
    b.add_native("Save.get_int", [s](NativeCall& c) {
        const auto it = s->open.find(slot_arg(c, 0));
        return i32(it == s->open.end() ? nv::integer(c, 2) : it->second.data.get_int(nv::str(c, 1), nv::integer(c, 2)));
    });
    b.add_native("Save.get_float", [s](NativeCall& c) {
        const auto it = s->open.find(slot_arg(c, 0));
        const double fb = static_cast<double>(nv::f32(c, 2));
        return Value::f32(static_cast<float>(it == s->open.end() ? fb : it->second.data.get_float(nv::str(c, 1), fb)));
    });
    b.add_native("Save.get_bool", [s](NativeCall& c) {
        const auto it = s->open.find(slot_arg(c, 0));
        return Value::boolean(it == s->open.end() ? nv::boolean(c, 2)
                                                  : it->second.data.get_bool(nv::str(c, 1), nv::boolean(c, 2)));
    });
    b.add_native("Save.get_str", [s](NativeCall& c) {
        const auto it = s->open.find(slot_arg(c, 0));
        return nv::str_result(it == s->open.end() ? std::string(nv::str(c, 2))
                                                  : it->second.data.get_str(nv::str(c, 1), nv::str(c, 2)));
    });

    // ---- Settings ------------------------------------------------------------
    b.add_native("Settings.load", [s](NativeCall&) {
        std::string error;
        s->settings = save::load_settings(s->store(), &error);
        s->settings_loaded = true;
        s->apply();
        return Value::boolean(error.empty());
    });
    b.add_native("Settings.save", [s](NativeCall&) {
        std::string error;
        const bool ok = save::save_settings(s->store(), s->settings, &error);
        if (!ok) LOG_WARN("Settings.save: {}", error);
        return Value::boolean(ok);
    });
    b.add_native("Settings.apply", [s](NativeCall&) {
        s->apply();
        return Value{};
    });
    b.add_native("Settings.reset", [s](NativeCall&) {
        s->settings = save::Settings{};
        s->apply();
        return Value{};
    });
    b.add_native("Settings.fullscreen", [s](NativeCall&) { return Value::boolean(s->settings.fullscreen); });
    b.add_native("Settings.set_fullscreen", [s](NativeCall& c) {
        s->settings.fullscreen = nv::boolean(c, 0);
        s->apply();
        return Value{};
    });
    b.add_native("Settings.vsync", [s](NativeCall&) { return Value::boolean(s->settings.vsync); });
    b.add_native("Settings.set_vsync", [s](NativeCall& c) {
        s->settings.vsync = nv::boolean(c, 0);
        s->apply();
        return Value{};
    });
    b.add_native("Settings.ui_scale", [s](NativeCall&) { return Value::f32(s->settings.ui_scale); });
    b.add_native("Settings.set_ui_scale", [s](NativeCall& c) {
        s->settings.ui_scale = std::clamp(nv::f32(c, 0), 0.5f, 3.0f);
        s->apply();
        return Value{};
    });
    b.add_native("Settings.volume", [s](NativeCall& c) { return Value::f32(s->settings.volume[bus_index(c, 0)]); });
    b.add_native("Settings.set_volume", [s](NativeCall& c) {
        s->settings.volume[bus_index(c, 0)] = std::clamp(nv::f32(c, 1), 0.0f, 1.0f);
        s->apply();
        return Value{};
    });
    b.add_native("Settings.muted", [s](NativeCall&) { return Value::boolean(s->settings.muted); });
    b.add_native("Settings.set_muted", [s](NativeCall& c) {
        s->settings.muted = nv::boolean(c, 0);
        s->apply();
        return Value{};
    });
    b.add_native("Settings.set_binding", [s](NativeCall& c) {
        const std::string action(nv::name(c, 0));
        const std::string binding(nv::str(c, 1));
        if (s->target != nullptr && !s->target->set_binding(action, binding)) return Value::boolean(false);
        s->settings.bindings[action] = binding;
        return Value::boolean(true);
    });
    b.add_native("Settings.binding", [s](NativeCall& c) {
        const auto it = s->settings.bindings.find(std::string(nv::name(c, 0)));
        return nv::str_result(it == s->settings.bindings.end() ? std::string() : it->second);
    });
}

} // namespace cuervo::script
