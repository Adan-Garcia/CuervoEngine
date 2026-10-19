#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cuervo/audio/audio_types.hpp"
#include "cuervo/audio/command_queue.hpp"
#include "cuervo/audio/mixer.hpp"
#include "cuervo/audio/sound.hpp"
#include "cuervo/core/name.hpp"

// The simulation-facing audio API (DESIGN.md 4.9).
//
// Determinism (DESIGN.md 4.1): sounds are *triggered* by the simulation and
// *played* by the audio thread, and nothing flows back.
//   - play()/stop()/set_bus_volume()... only record commands. Their results
//     (voice handles, the bus settings read back by bus_volume()) are computed
//     here from the trigger sequence alone, never from what the audio thread
//     is doing, so a replay makes the same calls and gets the same answers
//     with or without a sound device, a slow device or a missing file.
//   - end_tick(tick) closes the tick: its commands go to the audio thread as
//     one batch, so a tick's sounds always start together.
//
// Backends:
//   - Device: a miniaudio playback device whose callback drains the command
//     queue and runs the Mixer.
//   - Null: no device and no thread. end_tick() applies the batch and renders
//     exactly one tick of audio (sample_rate / tick_rate frames, carried
//     fractionally) on the calling thread, into an optional capture callback.
//     Used by tests, headless runs and when no device can be opened; the
//     output is a pure function of the trigger sequence.
namespace cuervo::assets {
class Vfs;
}

namespace cuervo::audio {

enum class Backend { Device, Null };

struct AudioOptions {
    Backend backend = Backend::Device;
    std::uint32_t max_voices = 32;
    float tick_rate = 60.0f; // Null backend: frames rendered per end_tick()
    // Relative sound paths resolve against this (the project directory)...
    std::filesystem::path root;
    // ...or, when set, through the game's VFS (docs/EXPORT.md "VFS rules"):
    // the project directory in development, the pack in release.
    std::shared_ptr<const assets::Vfs> vfs;
};

class AudioSystem {
public:
    explicit AudioSystem(AudioOptions options = {});
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // The backend actually running (Device falls back to Null on failure).
    Backend backend() const { return backend_; }

    // ---- sounds (main thread; registering is not simulation state)

    // Registers `path` under `name`; registering a name again returns the
    // existing sound (a different path is an error, logged). no_sound on failure.
    SoundId load(Name name, const std::filesystem::path& path, const SoundConfig& config = {});
    // Registers an already decoded sound.
    SoundId add(Name name, std::unique_ptr<SoundData> sound);
    // Registers every audio file under `dir` (sorted, recursive) by file stem:
    // assets/sfx/hit.wav is #hit. Files under a `music` directory stream, loop
    // and go to the music bus. Returns how many loaded.
    std::size_t load_directory(const std::filesystem::path& dir);
    // The same for a VFS directory ("assets"): the game's sounds in
    // development and release alike, in the same order.
    std::size_t load_directory(const assets::Vfs& vfs, std::string_view dir);
    std::optional<SoundId> find(Name name) const;
    const SoundData* sound(SoundId id) const;
    std::size_t sound_count() const { return sounds_.size(); }

    // ---- triggers (simulation tick)

    // Always returns a fresh handle, even for an unknown sound (warned once).
    VoiceHandle play(SoundId id, const PlayParams& params = {});
    VoiceHandle play(Name name, const PlayParams& params = {});
    void stop(VoiceHandle voice, float fade = 0.0f);
    void set_voice(VoiceHandle voice, float volume, float pitch, float pan, float fade = 0.0f);
    void stop_all(float fade = 0.0f);

    // Music: one looping, streamed track on the music bus. Starting the track
    // that is already playing does nothing; another track fades the current
    // one out over `fade` while the new one fades in (a crossfade).
    VoiceHandle play_music(SoundId id, float fade = 0.0f, float volume = 1.0f);
    void stop_music(float fade = 1.0f);
    SoundId music() const { return music_sound_; }

    void set_bus_volume(Bus bus, float volume, float fade = 0.0f);
    void set_bus_muted(Bus bus, bool muted);
    float bus_volume(Bus bus) const { return bus_settings_[static_cast<std::size_t>(bus)].volume; }
    bool bus_muted(Bus bus) const { return bus_settings_[static_cast<std::size_t>(bus)].muted; }

    // Closes simulation tick `tick`: hands its triggers to the audio thread.
    void end_tick(std::uint64_t tick);

    // Back to a fresh game: stops everything, forgets the music, restarts
    // handle numbering and resets the buses. Sounds stay loaded.
    void reset();

    // ---- inspection

    // Every command of every closed tick, in order (when recording is on).
    void set_recording(bool on) { recording_ = on; }
    const std::vector<Command>& recorded() const { return recorded_; }
    // Null backend: receives each rendered tick (interleaved stereo).
    void set_capture(std::function<void(const float* samples, std::uint32_t frames)> f) { capture_ = std::move(f); }
    // Null backend only (with a device the mixer belongs to the audio thread).
    const Mixer* null_mixer() const { return backend_ == Backend::Null ? &mixer_ : nullptr; }
    std::uint64_t ticks() const { return ticks_; }

private:
    struct DeviceState;
    void push(Command c);
    void push_bus(Bus bus, float fade);
    bool open_device();
    static void device_callback(void* user, float* out, std::uint32_t frames);

    AudioOptions options_;
    Backend backend_ = Backend::Null;
    std::unique_ptr<DeviceState> device_;
    Mixer mixer_;
    CommandQueue queue_;

    std::vector<std::unique_ptr<SoundData>> sounds_;
    std::unordered_map<Name, SoundId> by_name_; // lookups only, never iterated
    std::unordered_set<std::uint32_t> warned_;  // unknown names already reported

    // Simulation-side state (deterministic).
    struct BusSetting {
        float volume = 1.0f;
        bool muted = false;
    };
    std::array<BusSetting, bus_count> bus_settings_{};
    VoiceHandle next_handle_ = 1;
    VoiceHandle music_voice_ = 0;
    SoundId music_sound_ = no_sound;
    std::uint64_t tick_ = 0;
    std::uint64_t ticks_ = 0;

    std::vector<Command> pending_; // this tick's commands (+ any the queue had no room for)
    std::size_t prepared_ = 0;     // pending_ entries already recorded and given streams
    bool recording_ = false;
    std::vector<Command> recorded_;

    // Null backend rendering.
    double frame_carry_ = 0.0;
    std::vector<float> scratch_;
    std::function<void(const float*, std::uint32_t)> capture_;
};

} // namespace cuervo::audio
