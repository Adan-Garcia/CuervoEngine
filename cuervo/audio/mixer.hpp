#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "cuervo/audio/audio_types.hpp"
#include "cuervo/audio/sound.hpp"

// The software mixer (DESIGN.md 4.9). It owns the voices and the buses and
// renders interleaved stereo. It has no threads and no clock: whoever owns it
// applies Commands and calls mix() (the device callback, or the null backend
// on the main thread), so the same command sequence and the same mix() calls
// always produce the same samples.
//
// Gain of a voice = fade x voice volume x sound volume x pan x bus x master.
// Pan is constant-power, normalized to unity at the center:
//   left = sqrt2 * cos((pan + 1) * pi/4), right = sqrt2 * sin((pan + 1) * pi/4).
// Pitch resamples by linear interpolation (pitch 1 reproduces samples exactly).
// The output is clamped to [-1, 1].
//
// Voice limiting:
//   1. a sound with max_instances > 0 that is already at its limit steals its
//      own oldest voice;
//   2. otherwise a free voice is used;
//   3. otherwise the "weakest" voice is stolen: voices already fading out to
//      stop first, then the lowest priority, then the oldest. If even the
//      weakest outranks the new sound, the new sound is dropped.
// Stolen voices stop at once (their slot is needed now).
namespace cuervo::audio {

struct Command {
    enum class Type : std::uint8_t { Play, Stop, SetVoice, SetBus, StopAll };
    Type type = Type::Play;
    Bus bus = Bus::Sfx;          // Play: target bus; SetBus: the bus
    bool loop = false;           // Play
    bool muted = false;          // SetBus
    VoiceHandle voice = 0;       // Play, Stop, SetVoice
    std::int32_t priority = 0;   // Play
    float volume = 1.0f;         // Play, SetVoice, SetBus
    float pitch = 1.0f;          // Play, SetVoice
    float pan = 0.0f;            // Play, SetVoice
    float fade = 0.0f;           // seconds: Play fade-in, Stop fade-out, SetVoice/SetBus ramp
    const SoundData* sound = nullptr; // Play
    StreamSource* stream = nullptr;   // Play of a streamed sound: ownership passes to the mixer
    std::uint64_t tick = 0;      // the simulation tick that triggered it

    friend bool operator==(const Command&, const Command&) = default;
};

// A linear gain ramp, advanced per frame.
struct Ramp {
    float value = 1.0f;
    float target = 1.0f;
    float step = 0.0f;
    std::uint32_t remaining = 0;

    void jump(float v) {
        value = target = v;
        step = 0.0f;
        remaining = 0;
    }
    void to(float t, std::uint32_t frames) {
        target = t;
        if (frames == 0) {
            jump(t);
            return;
        }
        remaining = frames;
        step = (t - value) / static_cast<float>(frames);
    }
    float next() {
        if (remaining == 0) return value;
        if (--remaining == 0) {
            value = target;
        } else {
            value += step;
        }
        return value;
    }
    bool done() const { return remaining == 0; }
};

// Constant-power pan gains (see above).
void pan_gains(float pan, float& left, float& right);

struct MixerConfig {
    std::uint32_t max_voices = 32;
};

class Mixer {
public:
    explicit Mixer(MixerConfig config = {});
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    void apply(const Command& c);
    // Renders `frames` interleaved stereo frames into `out` (overwrites).
    void mix(float* out, std::uint32_t frames);

    // ---- inspection (same thread as mix(); tests and the null backend)
    struct VoiceInfo {
        VoiceHandle handle = 0;
        const SoundData* sound = nullptr;
        Bus bus = Bus::Sfx;
        std::int32_t priority = 0;
        std::uint64_t order = 0; // start sequence: lower = older
        double position = 0.0;   // frames into the sound
        float gain = 1.0f;       // current fade gain
        bool stopping = false;
    };
    // Active voices, oldest first.
    std::vector<VoiceInfo> voices() const;
    bool playing(VoiceHandle h) const;
    std::size_t active_voices() const;
    float bus_gain(Bus b) const { return buses_[static_cast<std::size_t>(b)].value; }
    std::uint64_t stolen() const { return stolen_; }
    std::uint64_t dropped() const { return dropped_; }
    std::uint32_t max_voices() const { return static_cast<std::uint32_t>(voices_.size()); }

private:
    struct Voice;
    Voice* find(VoiceHandle h);
    Voice* slot_for(const Command& c);
    void stop_now(Voice& v);
    // Next frame of a voice into l, r; false when it has ended.
    bool sample(Voice& v, float& l, float& r);
    bool stream_frame(Voice& v, std::uint64_t index, float& l, float& r);

    std::vector<std::unique_ptr<Voice>> voices_;
    std::array<Ramp, bus_count> buses_;
    std::uint64_t next_order_ = 1;
    std::uint64_t stolen_ = 0;
    std::uint64_t dropped_ = 0;
    std::vector<float> bus_block_; // per-bus gains for the current block
};

// Fade length in frames for `seconds` (at least `min_frames`).
std::uint32_t fade_frames(float seconds, std::uint32_t min_frames = 0);

} // namespace cuervo::audio
