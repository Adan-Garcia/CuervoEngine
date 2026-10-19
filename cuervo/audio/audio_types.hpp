#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// Shared audio vocabulary (DESIGN.md 4.9): buses, sound settings and the
// parameters of one play trigger.
namespace cuervo::audio {

// The mixer's fixed output format: interleaved stereo float at 48 kHz. Sounds
// are converted to it when they load (or as they stream); the device converts
// to whatever the hardware wants.
inline constexpr std::uint32_t sample_rate = 48000;
inline constexpr std::uint32_t channels = 2;

// Every bus feeds Master. Volumes multiply: voice x sound x bus x master.
enum class Bus : std::uint8_t { Master, Music, Sfx, Ui, Voice, Count };
inline constexpr std::size_t bus_count = static_cast<std::size_t>(Bus::Count);

constexpr std::string_view bus_name(Bus b) {
    switch (b) {
        case Bus::Master: return "master";
        case Bus::Music: return "music";
        case Bus::Sfx: return "sfx";
        case Bus::Ui: return "ui";
        case Bus::Voice: return "voice";
        case Bus::Count: break;
    }
    return "?";
}

constexpr std::optional<Bus> bus_from_name(std::string_view s) {
    for (std::size_t i = 0; i < bus_count; ++i) {
        if (bus_name(static_cast<Bus>(i)) == s) return static_cast<Bus>(i);
    }
    return std::nullopt;
}

using SoundId = std::uint32_t;
inline constexpr SoundId no_sound = 0xffffffffu;

// A playing (or requested) voice. Handed out by the simulation side in
// trigger order, so the numbers are deterministic; 0 is "no voice".
using VoiceHandle = std::uint32_t;

// Per-sound settings, fixed when the sound is registered.
struct SoundConfig {
    Bus bus = Bus::Sfx;
    float volume = 1.0f;
    // At most this many voices of the sound at once; a new one steals the
    // oldest (DESIGN.md 4.9). 0 = only the global voice limit applies.
    std::uint32_t max_instances = 8;
    // Higher wins when the global voice limit is reached.
    std::int32_t priority = 0;
    // Decode incrementally while playing instead of fully at load (music).
    bool stream = false;
};

// One play trigger.
struct PlayParams {
    float volume = 1.0f;
    float pitch = 1.0f;     // playback rate; 2 = an octave up and twice as fast
    float pan = 0.0f;       // -1 left .. 1 right
    std::optional<Bus> bus; // default: the sound's bus
    bool loop = false;
    float fade_in = 0.0f;   // seconds
    std::optional<std::int32_t> priority; // default: the sound's priority
};

// Pitch is clamped to this range (also bounds the streaming read-ahead).
inline constexpr float min_pitch = 0.125f;
inline constexpr float max_pitch = 4.0f;

} // namespace cuervo::audio
