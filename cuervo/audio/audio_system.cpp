#include "cuervo/audio/audio_system.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

#include "cuervo/assets/vfs.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/platform/platform.hpp"

namespace cuervo::audio {

namespace fs = std::filesystem;

namespace {

// Music outranks every sound effect when voices run out.
constexpr std::int32_t music_priority = std::numeric_limits<std::int32_t>::max() / 2;

} // namespace

struct AudioSystem::DeviceState {
    ma_device device{};
    bool started = false;
    // Audio thread statistics, read after the device stops.
    std::uint64_t frames = 0;
    float peak = 0.0f;
};

AudioSystem::AudioSystem(AudioOptions options)
    : options_(std::move(options)), mixer_(MixerConfig{options_.max_voices}) {
    if (options_.backend == Backend::Device && open_device()) {
        backend_ = Backend::Device;
    } else {
        backend_ = Backend::Null;
        if (options_.backend == Backend::Device) LOG_WARN("Audio: no playback device, using the null backend");
    }
}

AudioSystem::~AudioSystem() {
    if (device_ && device_->started) {
        ma_device_uninit(&device_->device);
        // The audio thread is gone: the mixer can be read here.
        LOG_INFO("Audio: {:.1f} s played, peak {:.2f}, {} voice(s) stolen, {} dropped",
                 static_cast<double>(device_->frames) / sample_rate, device_->peak, mixer_.stolen(), mixer_.dropped());
    }
    device_.reset();
    // Streams still queued (never reached the mixer) are owned here.
    queue_.drain([](const Command& c) { delete c.stream; });
    for (Command& c : pending_) delete c.stream;
}

bool AudioSystem::open_device() {
    auto state = std::make_unique<DeviceState>();
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = channels;
    cfg.sampleRate = sample_rate;
    cfg.pUserData = this;
    cfg.dataCallback = [](ma_device* d, void* out, const void*, ma_uint32 frames) {
        device_callback(d->pUserData, static_cast<float*>(out), frames);
    };
    if (ma_device_init(nullptr, &cfg, &state->device) != MA_SUCCESS) return false;
    // In place before the first callback can run.
    device_ = std::move(state);
    if (ma_device_start(&device_->device) != MA_SUCCESS) {
        ma_device_uninit(&device_->device);
        device_.reset();
        return false;
    }
    device_->started = true;
    LOG_INFO("Audio: '{}' via {}, {} Hz stereo, {} voices", device_->device.playback.name,
             ma_get_backend_name(device_->device.pContext->backend), sample_rate, mixer_.max_voices());
    return true;
}

// The audio thread: whole tick batches, then the mix. Never allocates, locks
// or logs (streams are freed here when their voice ends: one free per track).
void AudioSystem::device_callback(void* user, float* out, std::uint32_t frames) {
    auto* self = static_cast<AudioSystem*>(user);
    self->queue_.drain([self](const Command& c) { self->mixer_.apply(c); });
    self->mixer_.mix(out, frames);
    DeviceState& d = *self->device_;
    d.frames += frames;
    for (std::uint32_t i = 0; i < frames * channels; ++i) d.peak = std::max(d.peak, std::abs(out[i]));
}

// ------------------------------------------------------------ sounds

SoundId AudioSystem::add(Name name, std::unique_ptr<SoundData> sound) {
    if (auto existing = find(name)) return *existing;
    sound->name = std::string(name.str());
    const auto id = static_cast<SoundId>(sounds_.size());
    sounds_.push_back(std::move(sound));
    by_name_.emplace(name, id);
    return id;
}

SoundId AudioSystem::load(Name name, const fs::path& path, const SoundConfig& config) {
    // Game paths go through the VFS (pack or project); absolute paths
    // (tools, tests) are real files.
    const bool through_vfs = options_.vfs && !path.is_absolute();
    const fs::path full = path.is_absolute() || options_.root.empty() || through_vfs ? path : options_.root / path;
    if (auto existing = find(name)) {
        if (platform::path_from_utf8(sounds_[*existing]->path) != full) {
            LOG_WARN("Audio: sound '{}' is already '{}', not loading '{}'", name.str(), sounds_[*existing]->path,
                     platform::utf8(full));
        }
        return *existing;
    }
    std::string error;
    std::unique_ptr<SoundData> sound;
    if (through_vfs) {
        const std::string game_path = platform::utf8(path);
        if (auto file = options_.vfs->open(game_path, error)) {
            const auto bytes = file->bytes();
            const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.data());
            sound = load_sound_memory(std::vector<std::uint8_t>(begin, begin + bytes.size()), config, error);
            if (sound) {
                sound->path = game_path;
            } else {
                error = std::format("'{}': {}", options_.vfs->describe(game_path), error);
            }
        }
    } else {
        sound = load_sound_file(full, config, error);
    }
    if (!sound) {
        LOG_ERROR("Audio: {}", error);
        return no_sound;
    }
    return add(name, std::move(sound));
}

std::size_t AudioSystem::load_directory(const fs::path& where) {
    std::error_code ec;
    // Absolute, so load() doesn't resolve the paths against `root` again.
    const fs::path dir = fs::absolute(where, ec);
    if (!fs::is_directory(dir, ec)) return 0;
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file() && is_audio_file(e.path())) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end()); // deterministic registration order
    std::size_t loaded = 0;
    for (const fs::path& f : files) {
        SoundConfig cfg;
        const fs::path rel = f.lexically_relative(dir);
        const bool music = std::any_of(rel.begin(), rel.end(), [](const fs::path& part) { return part == "music"; });
        if (music) {
            cfg.stream = true;
            cfg.bus = Bus::Music;
            cfg.max_instances = 0;
        }
        if (load(Name(platform::utf8(f.stem())), f, cfg) != no_sound) ++loaded;
    }
    return loaded;
}

std::size_t AudioSystem::load_directory(const assets::Vfs& vfs, std::string_view dir) {
    // Vfs::list sorts like the directory walk above: the same registration order.
    std::size_t loaded = 0;
    for (const std::string& path : vfs.list(dir)) {
        const fs::path p = platform::path_from_utf8(path);
        if (!is_audio_file(p)) continue;
        SoundConfig cfg;
        const fs::path rel = p.lexically_relative(platform::path_from_utf8(dir));
        const bool music = std::any_of(rel.begin(), rel.end(), [](const fs::path& part) { return part == "music"; });
        if (music) {
            cfg.stream = true;
            cfg.bus = Bus::Music;
            cfg.max_instances = 0;
        }
        const Name name(platform::utf8(p.stem()));
        if (auto existing = find(name)) { // same rule as load(): the first file with a name wins
            LOG_WARN("Audio: sound '{}' is already '{}', not loading '{}'", name.str(), sounds_[*existing]->path, path);
            continue;
        }
        std::string error;
        auto file = vfs.open(path, error);
        std::unique_ptr<SoundData> sound;
        if (file) {
            const auto bytes = file->bytes();
            const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.data());
            sound = load_sound_memory(std::vector<std::uint8_t>(begin, begin + bytes.size()), cfg, error);
        }
        if (!sound) {
            LOG_ERROR("Audio: '{}': {}", vfs.describe(path), error);
            continue;
        }
        sound->path = path;
        if (add(name, std::move(sound)) != no_sound) ++loaded;
    }
    return loaded;
}

std::optional<SoundId> AudioSystem::find(Name name) const {
    auto it = by_name_.find(name);
    if (it == by_name_.end()) return std::nullopt;
    return it->second;
}

const SoundData* AudioSystem::sound(SoundId id) const {
    return id < sounds_.size() ? sounds_[id].get() : nullptr;
}

// ------------------------------------------------------------ triggers

void AudioSystem::push(Command c) {
    c.tick = tick_ + 1; // the tick being simulated (end_tick closes it)
    pending_.push_back(c);
}

VoiceHandle AudioSystem::play(SoundId id, const PlayParams& p) {
    const VoiceHandle handle = next_handle_++;
    const SoundData* s = sound(id);
    if (s == nullptr) return handle;
    Command c;
    c.type = Command::Type::Play;
    c.voice = handle;
    c.sound = s;
    c.bus = p.bus.value_or(s->config.bus);
    c.priority = p.priority.value_or(s->config.priority);
    c.loop = p.loop;
    c.volume = p.volume;
    c.pitch = p.pitch;
    c.pan = p.pan;
    c.fade = p.fade_in;
    push(c);
    return handle;
}

VoiceHandle AudioSystem::play(Name name, const PlayParams& p) {
    if (auto id = find(name)) return play(*id, p);
    if (warned_.insert(name.id()).second) LOG_WARN("Audio: unknown sound '{}'", name.str());
    return play(no_sound, p);
}

void AudioSystem::stop(VoiceHandle voice, float fade) {
    if (voice == 0) return;
    Command c;
    c.type = Command::Type::Stop;
    c.voice = voice;
    c.fade = fade;
    push(c);
    if (voice == music_voice_) {
        music_voice_ = 0;
        music_sound_ = no_sound;
    }
}

void AudioSystem::set_voice(VoiceHandle voice, float volume, float pitch, float pan, float fade) {
    if (voice == 0) return;
    Command c;
    c.type = Command::Type::SetVoice;
    c.voice = voice;
    c.volume = volume;
    c.pitch = pitch;
    c.pan = pan;
    c.fade = fade;
    push(c);
}

void AudioSystem::stop_all(float fade) {
    Command c;
    c.type = Command::Type::StopAll;
    c.fade = fade;
    push(c);
    music_voice_ = 0;
    music_sound_ = no_sound;
}

VoiceHandle AudioSystem::play_music(SoundId id, float fade, float volume) {
    if (id == music_sound_ && music_voice_ != 0) return music_voice_;
    if (music_voice_ != 0) stop(music_voice_, fade);
    PlayParams p;
    p.loop = true;
    p.bus = Bus::Music;
    p.fade_in = fade;
    p.volume = volume;
    p.priority = music_priority;
    music_voice_ = play(id, p);
    music_sound_ = sound(id) != nullptr ? id : no_sound;
    return music_voice_;
}

void AudioSystem::stop_music(float fade) {
    if (music_voice_ != 0) stop(music_voice_, fade);
}

void AudioSystem::push_bus(Bus bus, float fade) {
    const BusSetting& s = bus_settings_[static_cast<std::size_t>(bus)];
    Command c;
    c.type = Command::Type::SetBus;
    c.bus = bus;
    c.volume = s.volume;
    c.muted = s.muted;
    c.fade = fade;
    push(c);
}

void AudioSystem::set_bus_volume(Bus bus, float volume, float fade) {
    if (bus == Bus::Count) return;
    bus_settings_[static_cast<std::size_t>(bus)].volume = std::max(volume, 0.0f);
    push_bus(bus, fade);
}

void AudioSystem::set_bus_muted(Bus bus, bool muted) {
    if (bus == Bus::Count) return;
    bus_settings_[static_cast<std::size_t>(bus)].muted = muted;
    push_bus(bus, 0.0f);
}

void AudioSystem::reset() {
    for (Command& c : pending_) delete c.stream;
    pending_.clear();
    prepared_ = 0;
    stop_all(0.0f);
    next_handle_ = 1;
    for (std::size_t b = 0; b < bus_count; ++b) {
        bus_settings_[b] = {};
        push_bus(static_cast<Bus>(b), 0.0f);
    }
}

void AudioSystem::end_tick(std::uint64_t tick) {
    tick_ = tick;
    ++ticks_;
    // pending_[0, prepared_) are leftovers the queue had no room for; the new
    // ones are recorded and get their streams now (made on the main thread).
    for (std::size_t i = prepared_; i < pending_.size(); ++i) {
        Command& c = pending_[i];
        if (recording_) recorded_.push_back(c);
        if (c.type == Command::Type::Play && c.sound != nullptr && c.sound->streamed()) {
            c.stream = c.sound->open_stream().release();
        }
    }

    if (backend_ == Backend::Device) {
        if (!pending_.empty()) {
            if (queue_.push_batch(pending_.data(), pending_.size())) {
                pending_.clear();
                prepared_ = 0;
            } else {
                prepared_ = pending_.size();
                LOG_WARN("Audio: command queue full, {} command(s) wait a tick", pending_.size());
            }
        }
        return;
    }

    // Null backend: apply and render one tick's worth of frames here.
    for (const Command& c : pending_) mixer_.apply(c);
    pending_.clear();
    frame_carry_ += static_cast<double>(sample_rate) / static_cast<double>(options_.tick_rate);
    const auto frames = static_cast<std::uint32_t>(frame_carry_);
    frame_carry_ -= frames;
    scratch_.resize(static_cast<std::size_t>(frames) * channels);
    mixer_.mix(scratch_.data(), frames);
    if (capture_) capture_(scratch_.data(), frames);
}

} // namespace cuervo::audio
