// cuervo/audio: mixer math, voice limiting and stealing, streaming, the
// command queue, the null backend and trigger determinism (DESIGN.md 4.9).

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "cuervo/audio/audio_system.hpp"
#include "cuervo/audio/command_queue.hpp"
#include "cuervo/audio/mixer.hpp"
#include "cuervo/core/hash.hpp"
#include "cuervo/script/project.hpp"
#include "cuervo/script/script_host.hpp"

using namespace cuervo;
using namespace cuervo::audio;
namespace fs = std::filesystem;

namespace {

// A decoded sound of `frames` frames where every sample is `value`.
std::unique_ptr<SoundData> dc_sound(std::size_t frames, float value, SoundConfig cfg = {}) {
    return make_sound(std::vector<float>(frames * channels, value), cfg);
}

// A sound whose left channel counts frames: 0, 1, 2, ... (scaled), right = -left.
std::unique_ptr<SoundData> ramp_sound(std::size_t frames, float scale = 0.001f) {
    std::vector<float> s(frames * channels);
    for (std::size_t i = 0; i < frames; ++i) {
        s[i * 2] = static_cast<float>(i) * scale;
        s[i * 2 + 1] = -static_cast<float>(i) * scale;
    }
    return make_sound(std::move(s));
}

Command play_cmd(const SoundData* s, VoiceHandle h, std::int32_t priority = 0) {
    Command c;
    c.type = Command::Type::Play;
    c.sound = s;
    c.voice = h;
    c.bus = s->config.bus;
    c.priority = priority;
    return c;
}

Command stop_cmd(VoiceHandle h, float fade) {
    Command c;
    c.type = Command::Type::Stop;
    c.voice = h;
    c.fade = fade;
    return c;
}

Command bus_cmd(Bus b, float volume, bool muted = false) {
    Command c;
    c.type = Command::Type::SetBus;
    c.bus = b;
    c.volume = volume;
    c.muted = muted;
    return c;
}

std::vector<float> mix(Mixer& m, std::uint32_t frames) {
    std::vector<float> out(static_cast<std::size_t>(frames) * channels);
    m.mix(out.data(), frames);
    return out;
}

std::vector<VoiceHandle> handles(const Mixer& m) {
    std::vector<VoiceHandle> h;
    for (const auto& v : m.voices()) h.push_back(v.handle);
    return h;
}

// A stream that counts frames 0, 1, 2 ... `length`-1 (left = i * 0.0001, below the output clamp).
class CountingStream final : public StreamSource {
public:
    explicit CountingStream(std::uint64_t length) : length_(length) {}
    std::uint64_t read(float* out, std::uint64_t frames) override {
        std::uint64_t n = std::min(frames, length_ - pos_);
        for (std::uint64_t i = 0; i < n; ++i) {
            out[i * 2] = static_cast<float>(pos_ + i) * 0.0001f;
            out[i * 2 + 1] = 0.0f;
        }
        pos_ += n;
        return n;
    }
    bool rewind() override {
        pos_ = 0;
        ++rewinds;
        return true;
    }
    int rewinds = 0;

private:
    std::uint64_t length_;
    std::uint64_t pos_ = 0;
};

} // namespace

TEST_CASE("audio: ramps and fade lengths") {
    Ramp r;
    CHECK(r.value == 1.0f);
    r.to(0.0f, 4);
    CHECK(r.next() == doctest::Approx(0.75f));
    CHECK(r.next() == doctest::Approx(0.5f));
    CHECK(r.next() == doctest::Approx(0.25f));
    CHECK(r.next() == 0.0f); // lands exactly on the target
    CHECK(r.done());
    CHECK(r.next() == 0.0f);
    r.to(1.0f, 0);
    CHECK(r.value == 1.0f);

    CHECK(fade_frames(0.5f) == 24000);
    CHECK(fade_frames(0.0f, 240) == 240);
    CHECK(fade_frames(-1.0f) == 0);
}

TEST_CASE("audio: constant-power pan, unity at the center") {
    float l = 0.0f, r = 0.0f;
    pan_gains(0.0f, l, r);
    CHECK(l == 1.0f);
    CHECK(r == 1.0f);
    pan_gains(-1.0f, l, r);
    CHECK(l == doctest::Approx(std::sqrt(2.0f)));
    CHECK(r == doctest::Approx(0.0f).epsilon(1e-6));
    pan_gains(1.0f, l, r);
    CHECK(l == doctest::Approx(0.0f).epsilon(1e-6));
    for (float p : {-0.7f, -0.2f, 0.3f, 0.9f}) {
        pan_gains(p, l, r);
        CHECK(l * l + r * r == doctest::Approx(2.0f)); // constant power
    }
    pan_gains(5.0f, l, r); // clamped
    CHECK(r == doctest::Approx(std::sqrt(2.0f)));
}

TEST_CASE("audio: mixer gain is voice x sound x bus x master") {
    SoundConfig cfg;
    cfg.volume = 0.5f;
    auto s = dc_sound(10000, 0.8f, cfg);
    Mixer m;
    Command p = play_cmd(s.get(), 1);
    p.volume = 0.5f;
    m.apply(p);
    m.apply(bus_cmd(Bus::Sfx, 0.5f));
    m.apply(bus_cmd(Bus::Master, 0.5f));
    mix(m, 240); // the bus changes ramp over the declick time
    CHECK(m.bus_gain(Bus::Sfx) == 0.5f);
    auto out = mix(m, 16);
    const float expected = 0.8f * 0.5f * 0.5f * 0.5f * 0.5f;
    for (float x : out) CHECK(x == doctest::Approx(expected));

    // Muting ramps to silence; unmuting restores the volume.
    m.apply(bus_cmd(Bus::Sfx, 0.5f, true));
    mix(m, 240);
    for (float x : mix(m, 8)) CHECK(x == 0.0f);
    m.apply(bus_cmd(Bus::Sfx, 0.5f, false));
    mix(m, 240);
    CHECK(mix(m, 1)[0] == doctest::Approx(expected));
}

TEST_CASE("audio: pitch 1 reproduces samples, pitch changes rate with interpolation") {
    auto s = ramp_sound(1000);
    Mixer m;
    m.apply(play_cmd(s.get(), 1));
    auto out = mix(m, 8);
    for (int i = 0; i < 8; ++i) {
        CHECK(out[i * 2] == static_cast<float>(i) * 0.001f);
        CHECK(out[i * 2 + 1] == -static_cast<float>(i) * 0.001f);
    }

    Mixer half;
    Command p = play_cmd(s.get(), 1);
    p.pitch = 0.5f;
    half.apply(p);
    out = mix(half, 5);
    CHECK(out[0] == doctest::Approx(0.0f));
    CHECK(out[2] == doctest::Approx(0.0005f)); // halfway between frames 0 and 1
    CHECK(out[4] == doctest::Approx(0.001f));
    CHECK(out[8] == doctest::Approx(0.002f));

    // Pitch 2 plays a 1000-frame sound in 500 frames.
    Mixer fast;
    p.pitch = 2.0f;
    fast.apply(p);
    mix(fast, 499);
    CHECK(fast.playing(1));
    mix(fast, 1);
    CHECK_FALSE(fast.playing(1));
    CHECK(fast.active_voices() == 0);

    // Clamped to the supported range.
    Mixer clamp;
    p.pitch = 100.0f;
    clamp.apply(p);
    mix(clamp, static_cast<std::uint32_t>(1000 / max_pitch));
    CHECK_FALSE(clamp.playing(1));
}

TEST_CASE("audio: looping wraps, one-shots end and free their voice") {
    auto s = ramp_sound(100);
    Mixer m;
    Command p = play_cmd(s.get(), 7);
    p.loop = true;
    m.apply(p);
    m.apply(play_cmd(s.get(), 8));
    auto out = mix(m, 250);
    // Frame 150 of the loop is source frame 50; the one-shot ended at 100.
    CHECK(out[150 * 2] == doctest::Approx(0.05f));
    CHECK(out[50 * 2] == doctest::Approx(0.1f)); // both voices at frame 50
    CHECK(m.playing(7));
    CHECK_FALSE(m.playing(8));
}

TEST_CASE("audio: fades in, fades out to a stop") {
    auto s = dc_sound(100000, 1.0f);
    Mixer m;
    Command p = play_cmd(s.get(), 1);
    p.fade = 0.01f; // 480 frames
    m.apply(p);
    auto out = mix(m, 480);
    CHECK(out[0] == doctest::Approx(1.0f / 480.0f));
    CHECK(out[239 * 2] == doctest::Approx(0.5f));
    CHECK(out[479 * 2] == 1.0f);

    m.apply(stop_cmd(1, 0.01f));
    CHECK(m.voices()[0].stopping);
    out = mix(m, 479);
    CHECK(m.playing(1));
    CHECK(out[239 * 2] == doctest::Approx(0.5f));
    mix(m, 1);
    CHECK_FALSE(m.playing(1));

    // A stop without a fade still ramps over the declick time (~5 ms).
    m.apply(play_cmd(s.get(), 2));
    m.apply(stop_cmd(2, 0.0f));
    mix(m, 239);
    CHECK(m.playing(2));
    mix(m, 1);
    CHECK_FALSE(m.playing(2));
}

TEST_CASE("audio: per-sound limit steals that sound's oldest voice") {
    SoundConfig cfg;
    cfg.max_instances = 3;
    auto hit = dc_sound(48000, 0.1f, cfg);
    auto other = dc_sound(48000, 0.1f);
    Mixer m(MixerConfig{8});
    m.apply(play_cmd(hit.get(), 1));
    m.apply(play_cmd(other.get(), 2));
    m.apply(play_cmd(hit.get(), 3));
    m.apply(play_cmd(hit.get(), 4));
    CHECK(handles(m) == std::vector<VoiceHandle>{1, 2, 3, 4});
    m.apply(play_cmd(hit.get(), 5)); // 4th hit: steals hit #1, not `other`
    CHECK(handles(m) == std::vector<VoiceHandle>{2, 3, 4, 5});
    m.apply(play_cmd(hit.get(), 6));
    CHECK(handles(m) == std::vector<VoiceHandle>{2, 4, 5, 6});
    CHECK(m.stolen() == 2);
    CHECK(m.dropped() == 0);
}

TEST_CASE("audio: global limit steals by stopping, priority, then age") {
    SoundConfig cfg;
    cfg.max_instances = 0;
    auto s = dc_sound(48000, 0.1f, cfg);
    Mixer m(MixerConfig{3});
    m.apply(play_cmd(s.get(), 1, 5));
    m.apply(play_cmd(s.get(), 2, 0));
    m.apply(play_cmd(s.get(), 3, 0));
    // Full. Equal priority: the oldest of the weakest (2) goes.
    m.apply(play_cmd(s.get(), 4, 0));
    CHECK(handles(m) == std::vector<VoiceHandle>{1, 3, 4});
    // Lower than everything: dropped.
    m.apply(play_cmd(s.get(), 5, -1));
    CHECK(handles(m) == std::vector<VoiceHandle>{1, 3, 4});
    CHECK(m.dropped() == 1);
    // A voice fading out is the first victim, whatever its priority.
    m.apply(stop_cmd(1, 1.0f));
    m.apply(play_cmd(s.get(), 6, -1));
    CHECK(handles(m) == std::vector<VoiceHandle>{3, 4, 6});
    // Higher priority beats the lowest one present (6, priority -1).
    m.apply(play_cmd(s.get(), 7, 9));
    CHECK(handles(m) == std::vector<VoiceHandle>{3, 4, 7});
    CHECK(m.stolen() == 3);
}

TEST_CASE("audio: streams play continuously across refills and loop by rewinding") {
    SoundData sd;
    sd.frames = 3000;
    sd.open_stream = [] { return std::make_unique<CountingStream>(3000); };
    Mixer m;
    Command p = play_cmd(&sd, 1);
    auto* stream = new CountingStream(3000);
    p.stream = stream;
    p.loop = true;
    m.apply(p);
    auto out = mix(m, 7000);
    std::uint32_t mismatch = 7000;
    for (std::uint32_t i = 0; i < 7000 && mismatch == 7000; ++i) {
        if (out[i * 2] != static_cast<float>(i % 3000) * 0.0001f) mismatch = i;
    }
    CHECK_MESSAGE(mismatch == 7000, "first mismatch at frame " << mismatch << ": " << out[mismatch * 2]);
    CHECK(stream->rewinds == 2);
    CHECK(m.playing(1));

    // A one-shot stream ends; a play of a stream without a source is dropped.
    Mixer once;
    p.loop = false;
    p.stream = new CountingStream(3000);
    once.apply(p);
    mix(once, 2999);
    CHECK(once.playing(1));
    mix(once, 2);
    CHECK_FALSE(once.playing(1));
    p.stream = nullptr;
    once.apply(p);
    CHECK(once.dropped() == 1);
}

TEST_CASE("audio: output is clamped") {
    auto s = dc_sound(1000, 0.9f);
    Mixer m;
    for (VoiceHandle h = 1; h <= 4; ++h) m.apply(play_cmd(s.get(), h));
    for (float x : mix(m, 32)) CHECK(x == 1.0f);
}

TEST_CASE("audio: command queue publishes whole batches") {
    CommandQueue q(4);
    Command c[3];
    for (int i = 0; i < 3; ++i) c[i].voice = static_cast<VoiceHandle>(i + 1);
    CHECK(q.push_batch(c, 3));
    CHECK_FALSE(q.push_batch(c, 2)); // only one slot left: all or nothing
    std::vector<VoiceHandle> seen;
    q.drain([&](const Command& x) { seen.push_back(x.voice); });
    CHECK(seen == std::vector<VoiceHandle>{1, 2, 3});
    CHECK(q.push_batch(c, 3)); // wraps around
    seen.clear();
    q.drain([&](const Command& x) { seen.push_back(x.voice); });
    CHECK(seen == std::vector<VoiceHandle>{1, 2, 3});
}

TEST_CASE("audio: null backend renders one tick per end_tick, from triggers alone") {
    AudioOptions o;
    o.backend = Backend::Null;
    o.tick_rate = 144.0f; // 333.33 frames per tick, carried
    AudioSystem a(o);
    REQUIRE(a.backend() == Backend::Null);
    const SoundId id = a.add(Name("beep"), dc_sound(48000, 0.25f));
    CHECK(a.find(Name("beep")) == id);
    CHECK(a.add(Name("beep"), dc_sound(10, 0.0f)) == id); // first registration wins

    std::uint64_t frames = 0;
    float last = 0.0f;
    a.set_capture([&](const float* s, std::uint32_t n) {
        frames += n;
        if (n > 0) last = s[(n - 1) * 2];
    });
    a.set_recording(true);
    CHECK(a.play(Name("beep")) == 1);
    CHECK(a.play(Name("nope")) == 2); // unknown: still a fresh handle, no command
    a.end_tick(1);
    CHECK(last == 0.25f);
    for (std::uint64_t t = 2; t <= 144; ++t) a.end_tick(t);
    CHECK(frames == 48000 - 1); // floor(144 * 333.33): the carry keeps it exact over time
    a.end_tick(145);
    CHECK(frames == 48000 + 333);
    REQUIRE(a.recorded().size() == 1);
    CHECK(a.recorded()[0].tick == 1);
    CHECK(a.recorded()[0].voice == 1);
    CHECK(a.null_mixer()->active_voices() == 0); // the 1 s beep has ended

    // Bus settings read back the simulation's own values.
    a.set_bus_volume(Bus::Music, 0.3f, 1.0f);
    a.set_bus_muted(Bus::Sfx, true);
    CHECK(a.bus_volume(Bus::Music) == 0.3f);
    CHECK(a.bus_muted(Bus::Sfx));
    a.reset();
    CHECK(a.bus_volume(Bus::Music) == 1.0f);
    CHECK(a.play(Name("beep")) == 1); // handle numbering restarts
}

TEST_CASE("audio: music crossfades and ignores a replay of the same track") {
    AudioOptions o;
    o.backend = Backend::Null;
    AudioSystem a(o);
    const SoundId one = a.add(Name("one"), dc_sound(4800, 0.1f));
    const SoundId two = a.add(Name("two"), dc_sound(4800, 0.1f));
    a.set_recording(true);
    const VoiceHandle v1 = a.play_music(one, 0.5f);
    CHECK(a.play_music(one, 0.5f) == v1);
    a.end_tick(1);
    CHECK(a.recorded().size() == 1);
    CHECK(a.recorded()[0].loop);
    CHECK(a.recorded()[0].bus == Bus::Music);
    const VoiceHandle v2 = a.play_music(two, 2.0f);
    a.end_tick(2);
    REQUIRE(a.recorded().size() == 3);
    CHECK(a.recorded()[1].type == Command::Type::Stop);
    CHECK(a.recorded()[1].voice == v1);
    CHECK(a.recorded()[1].fade == 2.0f);
    CHECK(a.recorded()[2].voice == v2);
    CHECK(a.recorded()[2].fade == 2.0f);
    CHECK(a.music() == two);
    const auto voices = a.null_mixer()->voices();
    REQUIRE(voices.size() == 2); // both audible during the crossfade
    CHECK(voices[0].stopping);
    a.stop_music(0.0f);
    CHECK(a.music() == no_sound);
}

TEST_CASE("audio: decodes WAV and streams it through miniaudio") {
    const fs::path file = fs::path(CUERVO_EXAMPLES_DIR) / "shmup/assets/sfx/explosion.wav";
    std::string error;
    auto decoded = load_sound_file(file, {}, error);
    REQUIRE_MESSAGE(decoded, error);
    // 0.7 s at 44.1 kHz, resampled to 48 kHz.
    CHECK(decoded->frames == doctest::Approx(0.7 * sample_rate).epsilon(0.01));
    CHECK(decoded->pcm.size() == decoded->frames * channels);

    SoundConfig cfg;
    cfg.stream = true;
    auto streamed = load_sound_file(file, cfg, error);
    REQUIRE_MESSAGE(streamed, error);
    CHECK(streamed->pcm.empty());
    // The length is the decoder's estimate; reading gives the exact count.
    CHECK(streamed->frames == doctest::Approx(decoded->frames).epsilon(0.001));
    auto src = streamed->open_stream();
    REQUIRE(src);
    std::vector<float> buf(512 * channels);
    std::uint64_t total = 0;
    bool same = true;
    while (std::uint64_t n = src->read(buf.data(), 512)) {
        for (std::uint64_t i = 0; i < n * channels; ++i) same = same && buf[i] == decoded->pcm[total * channels + i];
        total += n;
    }
    CHECK(total == decoded->frames);
    CHECK(streamed->open_stream() != nullptr); // independent streams of one sound
    CHECK(same);
    CHECK(src->rewind());

    CHECK_FALSE(load_sound_file(fs::path(CUERVO_EXAMPLES_DIR) / "shmup/project.toml", {}, error));
    CHECK_FALSE(load_sound_file("no/such/file.wav", {}, error));
    CHECK(is_audio_file("a/B.OGG"));
    CHECK_FALSE(is_audio_file("a/b.txt"));
}

TEST_CASE("audio: decodes OGG Vorbis, FLAC and MP3") {
    // assets/test/audio: the shmup's hurt.wav (0.45 s) re-encoded with ffmpeg.
    for (const char* name : {"tone.ogg", "tone.flac", "tone.mp3"}) {
        CAPTURE(name);
        std::string error;
        auto s = load_sound_file(fs::path(CUERVO_ASSETS_DIR) / "test/audio" / name, {}, error);
        REQUIRE_MESSAGE(s, error);
        // MP3 adds encoder padding; all are about 0.45 s at 48 kHz.
        CHECK(s->seconds() == doctest::Approx(0.45).epsilon(0.08));
        float peak = 0.0f;
        for (float x : s->pcm) peak = std::max(peak, std::abs(x));
        CHECK(peak > 0.3f);
        SoundConfig cfg;
        cfg.stream = true;
        auto st = load_sound_file(fs::path(CUERVO_ASSETS_DIR) / "test/audio" / name, cfg, error);
        REQUIRE_MESSAGE(st, error);
        CHECK(st->open_stream() != nullptr);
    }
}

namespace {

struct ShmupRun {
    std::vector<Command> triggers;
    std::vector<std::string> names;
    std::uint64_t audio_hash = 0;
    std::uint64_t state_hash = 0;
    std::size_t sounds = 0;
};

// Runs the shmup headless (demo mode) with a null-backend audio system.
ShmupRun run_shmup(int ticks, bool load_sounds) {
    const fs::path dir = fs::path(CUERVO_EXAMPLES_DIR) / "shmup";
    std::string error;
    auto project = script::load_project(dir, error);
    REQUIRE_MESSAGE(project, error);
    InputMap map;
    REQUIRE(script::apply_input(*project, map, error));
    std::vector<pluma::SourceFile::Part> sources;
    REQUIRE(script::read_scripts(*project, sources, nullptr, error));

    AudioOptions ao;
    ao.backend = Backend::Null;
    ao.tick_rate = project->tick_rate;
    ao.root = dir;
    AudioSystem audio(ao);
    ShmupRun run;
    if (load_sounds) run.sounds = audio.load_directory(dir / "assets");
    audio.set_recording(true);
    StateHasher h;
    audio.set_capture([&](const float* s, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n * channels; ++i) h.value(s[i]);
    });

    script::HostOptions o;
    o.seed = project->seed;
    o.tick_rate = project->tick_rate;
    o.audio = &audio;
    o.project_dir = dir;
    script::ScriptHost host(o, map);
    REQUIRE_MESSAGE(host.load(sources, project->main_class), host.error());
    for (int i = 0; i < ticks; ++i) {
        host.tick(InputFrame{});
        REQUIRE_MESSAGE(host.running(), host.error());
    }
    run.triggers = audio.recorded();
    for (Command& c : run.triggers) {
        // Pointers differ between runs; compare by name.
        run.names.push_back(c.sound != nullptr ? c.sound->name : "");
        c.sound = nullptr;
    }
    run.audio_hash = h.digest();
    run.state_hash = host.state_hash();
    return run;
}

} // namespace

TEST_CASE("audio: the shmup's triggers and mix are deterministic and never touch the simulation") {
    const ShmupRun a = run_shmup(900, true);
    const ShmupRun b = run_shmup(900, true);
    CHECK(a.sounds == 7);
    // Shots, hits, explosions and the music all fired.
    std::size_t plays = 0;
    bool music = false;
    for (const Command& c : a.triggers) {
        if (c.type != Command::Type::Play) continue;
        ++plays;
        music = music || (c.bus == Bus::Music && c.loop);
    }
    CHECK(plays > 100);
    CHECK(music);
    // Explicit priorities from scripts arrive (big_explosion/hurt use 10).
    CHECK(std::any_of(a.triggers.begin(), a.triggers.end(), [](const Command& c) { return c.priority == 10; }));

    CHECK(a.triggers == b.triggers);
    CHECK(a.names == b.names);
    CHECK(a.audio_hash == b.audio_hash);
    CHECK(a.state_hash == b.state_hash);

    // Without any sound files the game makes the same decisions: audio is
    // output only (DESIGN.md 4.1).
    const ShmupRun silent = run_shmup(900, false);
    CHECK(std::none_of(silent.triggers.begin(), silent.triggers.end(),
                       [](const Command& c) { return c.type == Command::Type::Play; }));
    CHECK(silent.state_hash == a.state_hash);
}

TEST_CASE("audio: script bindings trigger, configure and read back simulation-side state") {
    AudioOptions ao;
    ao.backend = Backend::Null;
    ao.root = fs::path(CUERVO_EXAMPLES_DIR) / "shmup";
    AudioSystem audio(ao);
    audio.set_recording(true);
    script::HostOptions o;
    o.audio = &audio;
    InputMap map;
    script::ScriptHost host(o, map);
    REQUIRE_MESSAGE(host.load({{"main.plm", R"(
class Main extends Node {
    var a = 0
    var b = 0
    var c = 0
    var loaded = #none
    var music_volume = 0.0
    var sfx_muted = false
    fn ready() {
        loaded = Audio.load("assets/sfx/hit.wav", id = #zap, limit = 2, priority = 3)
        a = Audio.play(#zap, volume = 0.5, pitch = 1.5, pan = -0.5)
        b = Audio.play(#missing)
        c = Audio.play_file("assets/sfx/shoot.wav", bus = #ui, looping = true, priority = 7)
        Audio.stop(c, fade = 0.25)
        Audio.set_volume(#music, 0.4, fade = 1.0)
        Audio.set_muted(#sfx, true)
        music_volume = Audio.volume(#music)
        sfx_muted = Audio.muted(#sfx)
    }
}
)"}},
                              "Main"),
                    host.error());
    host.tick(InputFrame{});
    REQUIRE_MESSAGE(host.running(), host.error());

    pluma::Value root = host.session()->global("Scene.root");
    auto* inst = static_cast<pluma::InstanceObj*>(root.o);
    const std::size_t base = inst->fields.size() - 6;
    CHECK(inst->fields[base].i == 1);
    CHECK(inst->fields[base + 1].i == 2); // unknown sound: still a fresh id
    CHECK(inst->fields[base + 2].i == 3);
    CHECK(host.session()->vm().name_text(static_cast<std::uint32_t>(inst->fields[base + 3].i)) == "zap");
    CHECK(inst->fields[base + 4].f == 0.4f);
    CHECK(inst->fields[base + 5].b);

    const auto& t = audio.recorded();
    REQUIRE(t.size() == 5);
    CHECK(t[0].type == Command::Type::Play);
    CHECK(t[0].sound->name == "zap");
    CHECK(t[0].sound->config.max_instances == 2);
    CHECK(t[0].priority == 3);
    CHECK(t[0].volume == 0.5f);
    CHECK(t[0].pitch == 1.5f);
    CHECK(t[0].pan == -0.5f);
    CHECK(t[0].bus == Bus::Sfx);
    CHECK(t[1].voice == 3);
    CHECK(t[1].bus == Bus::Ui);
    CHECK(t[1].loop);
    CHECK(t[1].priority == 7);
    CHECK(t[2].type == Command::Type::Stop);
    CHECK(t[2].fade == 0.25f);
    CHECK(t[3].type == Command::Type::SetBus);
    CHECK(t[4].muted);
    for (const Command& c : t) CHECK(c.tick == 1);

    // An unknown bus is a script error.
    script::ScriptHost bad(o, map);
    CHECK(bad.load({{"main.plm", "class Main extends Node { fn ready() { Audio.set_volume(#loud, 1.0) } }"}}, "Main"));
    CHECK(bad.panicked());
    CHECK(bad.error().find("unknown bus") != std::string::npos);
}
