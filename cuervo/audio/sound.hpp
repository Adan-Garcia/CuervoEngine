#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cuervo/audio/audio_types.hpp"

// Sound assets (DESIGN.md 4.9, 7.2 "Audio formats"): WAV, OGG Vorbis, FLAC
// and MP3 through miniaudio's decoders.
//
// - Decoded sounds (SFX) are converted to the mixer format (stereo f32,
//   48 kHz) when they load, so playing one never decodes.
// - Streamed sounds (music) keep the *encoded* file in memory and decode it
//   incrementally on the audio thread while playing: no file I/O ever happens
//   in the audio callback, and a long track costs its compressed size.
namespace cuervo::audio {

// An incremental decoder. Created on the main thread, then owned and used by
// the audio thread only.
class StreamSource {
public:
    virtual ~StreamSource() = default;
    // Reads up to `frames` stereo frames into `out`; returns how many were
    // read (0 at the end).
    virtual std::uint64_t read(float* out, std::uint64_t frames) = 0;
    // Back to the first frame (looping). False if the source cannot seek.
    virtual bool rewind() = 0;
};

struct SoundData {
    std::string name; // registry name (diagnostics)
    std::string path;
    SoundConfig config;
    // Decoded sounds: interleaved stereo at `sample_rate`.
    std::vector<float> pcm;
    // Streamed sounds: the encoded file, read by `open_stream`.
    std::vector<std::uint8_t> encoded;
    std::function<std::unique_ptr<StreamSource>()> open_stream;
    // Length in frames at `sample_rate` (0 if unknown for a stream).
    std::uint64_t frames = 0;

    bool streamed() const { return static_cast<bool>(open_stream); }
    double seconds() const { return static_cast<double>(frames) / sample_rate; }
};

// Decodes (or, with config.stream, prepares to stream) an audio file. On
// failure returns null and sets `error`.
std::unique_ptr<SoundData> load_sound_file(const std::filesystem::path& path, const SoundConfig& config,
                                           std::string& error);
// Same, from an encoded file already in memory.
std::unique_ptr<SoundData> load_sound_memory(std::vector<std::uint8_t> bytes, const SoundConfig& config,
                                             std::string& error);

// A decoded sound from samples already in the mixer format (tests, procedural
// sounds). `stereo` is interleaved L, R.
std::unique_ptr<SoundData> make_sound(std::vector<float> stereo, const SoundConfig& config = {});

// True for the file extensions the decoders handle (.wav .ogg .flac .mp3).
bool is_audio_file(const std::filesystem::path& path);

} // namespace cuervo::audio
