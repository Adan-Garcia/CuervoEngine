#include "cuervo/audio/sound.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <fstream>

#include "cuervo/platform/platform.hpp"

namespace cuervo::audio {

namespace {

ma_decoder_config decoder_config() {
    return ma_decoder_config_init(ma_format_f32, channels, sample_rate);
}

// An ma_decoder over a streamed sound's encoded bytes (owned by the
// SoundData, which outlives every voice: sounds are never unloaded while the
// audio system runs).
class DecoderStream final : public StreamSource {
public:
    bool open(const std::vector<std::uint8_t>& bytes) {
        ma_decoder_config cfg = decoder_config();
        ok_ = ma_decoder_init_memory(bytes.data(), bytes.size(), &cfg, &decoder_) == MA_SUCCESS;
        return ok_;
    }
    ~DecoderStream() override {
        if (ok_) ma_decoder_uninit(&decoder_);
    }
    DecoderStream() = default;
    DecoderStream(const DecoderStream&) = delete;
    DecoderStream& operator=(const DecoderStream&) = delete;

    std::uint64_t read(float* out, std::uint64_t frames) override {
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&decoder_, out, frames, &got);
        return got;
    }
    bool rewind() override { return ma_decoder_seek_to_pcm_frame(&decoder_, 0) == MA_SUCCESS; }

private:
    ma_decoder decoder_{};
    bool ok_ = false;
};

const char* result_text(ma_result r) { return ma_result_description(r); }

} // namespace

bool is_audio_file(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".wav" || ext == ".ogg" || ext == ".flac" || ext == ".mp3";
}

std::unique_ptr<SoundData> load_sound_memory(std::vector<std::uint8_t> bytes, const SoundConfig& config,
                                             std::string& error) {
    auto sound = std::make_unique<SoundData>();
    sound->config = config;
    ma_decoder_config cfg = decoder_config();

    if (config.stream) {
        // Validate now so a bad file fails at load, not on the audio thread.
        ma_decoder probe;
        ma_result r = ma_decoder_init_memory(bytes.data(), bytes.size(), &cfg, &probe);
        if (r != MA_SUCCESS) {
            error = std::format("cannot decode: {}", result_text(r));
            return nullptr;
        }
        ma_uint64 length = 0;
        if (ma_decoder_get_length_in_pcm_frames(&probe, &length) == MA_SUCCESS) sound->frames = length;
        ma_decoder_uninit(&probe);
        sound->encoded = std::move(bytes);
        SoundData* self = sound.get();
        sound->open_stream = [self]() -> std::unique_ptr<StreamSource> {
            auto s = std::make_unique<DecoderStream>();
            if (!s->open(self->encoded)) return nullptr;
            return s;
        };
        return sound;
    }

    ma_uint64 frames = 0;
    void* pcm = nullptr;
    ma_result r = ma_decode_memory(bytes.data(), bytes.size(), &cfg, &frames, &pcm);
    if (r != MA_SUCCESS) {
        error = std::format("cannot decode: {}", result_text(r));
        return nullptr;
    }
    sound->frames = frames;
    sound->pcm.resize(static_cast<std::size_t>(frames) * channels);
    if (frames > 0) std::memcpy(sound->pcm.data(), pcm, sound->pcm.size() * sizeof(float));
    ma_free(pcm, nullptr);
    return sound;
}

std::unique_ptr<SoundData> load_sound_file(const std::filesystem::path& path, const SoundConfig& config,
                                           std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = std::format("cannot open '{}'", platform::utf8(path));
        return nullptr;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto sound = load_sound_memory(std::move(bytes), config, error);
    if (!sound) {
        error = std::format("'{}': {}", platform::utf8(path), error);
        return nullptr;
    }
    sound->path = platform::utf8(path);
    return sound;
}

std::unique_ptr<SoundData> make_sound(std::vector<float> stereo, const SoundConfig& config) {
    auto sound = std::make_unique<SoundData>();
    sound->config = config;
    sound->frames = stereo.size() / channels;
    sound->pcm = std::move(stereo);
    return sound;
}

} // namespace cuervo::audio
