#include "cuervo/script/audio_bindings.hpp"

#include <format>
#include <optional>
#include <string>

#include "cuervo/core/name.hpp"
#include "cuervo/platform/platform.hpp"

namespace cuervo::script {

namespace {

namespace nv = pluma::native;
using pluma::NativeCall;
using pluma::Value;

audio::Bus bus_arg(NativeCall& c, std::uint32_t i) {
    const std::string_view text = nv::name(c, i);
    auto bus = audio::bus_from_name(text);
    if (!bus) c.vm.panic(std::format("Audio: unknown bus #{} (#master, #music, #sfx, #ui, #voice)", text));
    return *bus;
}

// play(..., volume, pitch, pan, bus, looping, fade, priority) arguments from index 1.
audio::PlayParams play_params(NativeCall& c) {
    audio::PlayParams p;
    p.volume = nv::f32(c, 1);
    p.pitch = nv::f32(c, 2);
    p.pan = nv::f32(c, 3);
    if (c.args[4].tag == pluma::Tag::Name) p.bus = bus_arg(c, 4);
    p.loop = nv::boolean(c, 5);
    p.fade_in = nv::f32(c, 6);
    if (pluma::is_num_tag(c.args[7].tag)) p.priority = static_cast<std::int32_t>(nv::integer(c, 7));
    return p;
}

Value voice_result(audio::VoiceHandle h) { return Value::integer(pluma::NumKind::I32, static_cast<std::int64_t>(h)); }

audio::VoiceHandle voice_arg(NativeCall& c, std::uint32_t i) {
    const std::int64_t v = nv::integer(c, i);
    return v > 0 ? static_cast<audio::VoiceHandle>(v) : 0;
}

// A path's sound, registered under the path itself on first use.
audio::SoundId sound_for_path(audio::AudioSystem& a, std::string_view path, const audio::SoundConfig& cfg = {}) {
    const Name key(path);
    if (auto id = a.find(key)) return *id;
    return a.load(key, std::string(path), cfg);
}

} // namespace

void bind_audio(pluma::Bindings& b, audio::AudioSystem& audio) {
    audio::AudioSystem* a = &audio;

    b.add_native("Audio.play", [a](NativeCall& c) {
        return voice_result(a->play(Name(nv::name(c, 0)), play_params(c)));
    });
    b.add_native("Audio.play_file", [a](NativeCall& c) {
        return voice_result(a->play(sound_for_path(*a, nv::str(c, 0)), play_params(c)));
    });
    b.add_native("Audio.stop", [a](NativeCall& c) {
        a->stop(voice_arg(c, 0), nv::f32(c, 1));
        return Value{};
    });
    b.add_native("Audio.set_voice", [a](NativeCall& c) {
        a->set_voice(voice_arg(c, 0), nv::f32(c, 1), nv::f32(c, 2), nv::f32(c, 3), nv::f32(c, 4));
        return Value{};
    });
    b.add_native("Audio.stop_all", [a](NativeCall& c) {
        a->stop_all(nv::f32(c, 0));
        return Value{};
    });

    b.add_native("Audio.load", [a](NativeCall& c) {
        const std::string path(nv::str(c, 0));
        std::string id = c.args[1].tag == pluma::Tag::Name ? std::string(nv::name(c, 1)) : std::string();
        if (id.empty()) id = platform::utf8(platform::path_from_utf8(path).stem());
        audio::SoundConfig cfg;
        cfg.bus = bus_arg(c, 2);
        cfg.volume = nv::f32(c, 3);
        const std::int64_t limit = nv::integer(c, 4);
        if (limit < 0) c.vm.panic("Audio.load: limit must be >= 0");
        cfg.max_instances = static_cast<std::uint32_t>(limit);
        cfg.priority = static_cast<std::int32_t>(nv::integer(c, 5));
        cfg.stream = nv::boolean(c, 6);
        a->load(Name(id), path, cfg);
        Value v;
        v.tag = pluma::Tag::Name;
        v.i = c.vm.intern(id);
        return v;
    });

    auto music_id = [a](NativeCall& c, std::uint32_t i) {
        const Name n(nv::name(c, i));
        return a->find(n).value_or(audio::no_sound);
    };
    b.add_native("Audio.music", [a, music_id](NativeCall& c) {
        a->play_music(music_id(c, 0), nv::f32(c, 1), nv::f32(c, 2));
        return Value{};
    });
    b.add_native("Audio.crossfade", [a, music_id](NativeCall& c) {
        a->play_music(music_id(c, 0), nv::f32(c, 1));
        return Value{};
    });
    b.add_native("Audio.stop_music", [a](NativeCall& c) {
        a->stop_music(nv::f32(c, 0));
        return Value{};
    });

    b.add_native("Audio.set_volume", [a](NativeCall& c) {
        a->set_bus_volume(bus_arg(c, 0), nv::f32(c, 1), nv::f32(c, 2));
        return Value{};
    });
    b.add_native("Audio.volume", [a](NativeCall& c) { return Value::f32(a->bus_volume(bus_arg(c, 0))); });
    b.add_native("Audio.set_muted", [a](NativeCall& c) {
        a->set_bus_muted(bus_arg(c, 0), nv::boolean(c, 1));
        return Value{};
    });
    b.add_native("Audio.muted", [a](NativeCall& c) { return Value::boolean(a->bus_muted(bus_arg(c, 0))); });
}

} // namespace cuervo::script
