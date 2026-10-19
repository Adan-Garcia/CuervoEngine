#include "cuervo/audio/mixer.hpp"

#include <algorithm>

#include "cuervo/math/dmath.hpp"

namespace cuervo::audio {

namespace {

constexpr std::uint32_t block_frames = 256;
// Streaming read-ahead per voice, in frames. Must exceed max_pitch + 1 so one
// output frame never needs more than one refill.
constexpr std::uint32_t stream_buffer_frames = 1024;
// Changes without an explicit fade still ramp over this long, so stops and
// volume jumps don't click (~5 ms at 48 kHz).
constexpr std::uint32_t declick_frames = 240;

constexpr float sqrt2 = 1.41421356237309504880f;

} // namespace

std::uint32_t fade_frames(float seconds, std::uint32_t min_frames) {
    float f = seconds > 0.0f ? seconds * static_cast<float>(sample_rate) : 0.0f;
    // Bounded: a fade of more than ~10 minutes is a script bug, not a fade.
    f = std::min(f, static_cast<float>(sample_rate) * 600.0f);
    return std::max(static_cast<std::uint32_t>(f), min_frames);
}

void pan_gains(float pan, float& left, float& right) {
    pan = std::clamp(pan, -1.0f, 1.0f);
    // dmath rather than libm: not simulation, but it keeps the mix identical
    // across platforms too (DESIGN.md 4.1).
    const float angle = (pan + 1.0f) * (dmath::pi * 0.25f);
    left = sqrt2 * dmath::cos(angle);
    right = sqrt2 * dmath::sin(angle);
    if (pan == 0.0f) left = right = 1.0f; // exact unity at the center
}

struct Mixer::Voice {
    bool active = false;
    VoiceHandle handle = 0;
    const SoundData* sound = nullptr;
    std::unique_ptr<StreamSource> stream;
    Bus bus = Bus::Sfx;
    std::int32_t priority = 0;
    std::uint64_t order = 0;
    double pos = 0.0;
    float pitch = 1.0f;
    float pan_l = 1.0f;
    float pan_r = 1.0f;
    Ramp gain;  // fades in/out
    Ramp level; // voice volume (SetVoice ramps it)
    bool stopping = false;
    bool loop = false;
    bool finished = false; // decoded: played past the end
    // Streams: frames [buf_start, buf_start + buf_count) of the (looped) source.
    std::vector<float> buf;
    std::uint64_t buf_start = 0;
    std::uint32_t buf_count = 0;
    bool ended = false;
};

Mixer::Mixer(MixerConfig config) {
    voices_.resize(std::max<std::uint32_t>(config.max_voices, 1));
    for (auto& v : voices_) {
        v = std::make_unique<Voice>();
        // Allocated up front: the audio thread never allocates for a voice.
        v->buf.resize(static_cast<std::size_t>(stream_buffer_frames) * channels);
    }
    bus_block_.resize(bus_count * block_frames);
}

Mixer::~Mixer() = default;

Mixer::Voice* Mixer::find(VoiceHandle h) {
    if (h == 0) return nullptr;
    for (auto& v : voices_) {
        if (v->active && v->handle == h) return v.get();
    }
    return nullptr;
}

bool Mixer::playing(VoiceHandle h) const {
    if (h == 0) return false;
    return std::any_of(voices_.begin(), voices_.end(), [h](const auto& v) { return v->active && v->handle == h; });
}

std::size_t Mixer::active_voices() const {
    return static_cast<std::size_t>(std::count_if(voices_.begin(), voices_.end(), [](const auto& v) { return v->active; }));
}

std::vector<Mixer::VoiceInfo> Mixer::voices() const {
    std::vector<VoiceInfo> out;
    for (const auto& v : voices_) {
        if (!v->active) continue;
        out.push_back({v->handle, v->sound, v->bus, v->priority, v->order, v->pos, v->gain.value, v->stopping});
    }
    std::sort(out.begin(), out.end(), [](const VoiceInfo& a, const VoiceInfo& b) { return a.order < b.order; });
    return out;
}

void Mixer::stop_now(Voice& v) {
    v.active = false;
    v.stream.reset();
    v.sound = nullptr;
    v.handle = 0;
}

Mixer::Voice* Mixer::slot_for(const Command& c) {
    // Weakest first: stopping, then lower priority, then older.
    auto weaker = [](const Voice* a, const Voice* b) {
        if (a->stopping != b->stopping) return a->stopping;
        if (a->priority != b->priority) return a->priority < b->priority;
        return a->order < b->order;
    };
    const std::uint32_t limit = c.sound->config.max_instances;
    if (limit > 0) {
        std::uint32_t count = 0;
        Voice* victim = nullptr;
        for (auto& v : voices_) {
            if (!v->active || v->sound != c.sound) continue;
            ++count;
            if (victim == nullptr || weaker(v.get(), victim)) victim = v.get();
        }
        if (count >= limit) return victim;
    }
    Voice* weakest = nullptr;
    for (auto& v : voices_) {
        if (!v->active) return v.get();
        if (weakest == nullptr || weaker(v.get(), weakest)) weakest = v.get();
    }
    if (weakest->stopping || weakest->priority <= c.priority) return weakest;
    return nullptr;
}

void Mixer::apply(const Command& c) {
    switch (c.type) {
        case Command::Type::Play: {
            std::unique_ptr<StreamSource> stream(c.stream);
            const bool playable = c.sound != nullptr && (c.sound->streamed() ? stream != nullptr : c.sound->frames > 0);
            Voice* v = playable ? slot_for(c) : nullptr;
            if (v == nullptr) {
                ++dropped_;
                return;
            }
            if (v->active) {
                ++stolen_;
                stop_now(*v);
            }
            v->active = true;
            v->handle = c.voice;
            v->sound = c.sound;
            v->stream = std::move(stream);
            v->bus = c.bus;
            v->priority = c.priority;
            v->order = next_order_++;
            v->pos = 0.0;
            v->pitch = std::clamp(c.pitch, min_pitch, max_pitch);
            pan_gains(c.pan, v->pan_l, v->pan_r);
            v->level.jump(std::max(c.volume, 0.0f));
            v->gain.jump(c.fade > 0.0f ? 0.0f : 1.0f);
            if (c.fade > 0.0f) v->gain.to(1.0f, fade_frames(c.fade));
            v->stopping = false;
            v->loop = c.loop;
            v->finished = false;
            v->buf_start = 0;
            v->buf_count = 0;
            v->ended = false;
            return;
        }
        case Command::Type::Stop: {
            if (Voice* v = find(c.voice)) {
                v->stopping = true;
                v->gain.to(0.0f, fade_frames(c.fade, declick_frames));
            }
            return;
        }
        case Command::Type::SetVoice: {
            if (Voice* v = find(c.voice)) {
                v->level.to(std::max(c.volume, 0.0f), fade_frames(c.fade, declick_frames));
                v->pitch = std::clamp(c.pitch, min_pitch, max_pitch);
                pan_gains(c.pan, v->pan_l, v->pan_r);
            }
            return;
        }
        case Command::Type::SetBus: {
            if (c.bus == Bus::Count) return;
            buses_[static_cast<std::size_t>(c.bus)].to(c.muted ? 0.0f : std::max(c.volume, 0.0f),
                                                       fade_frames(c.fade, declick_frames));
            return;
        }
        case Command::Type::StopAll: {
            for (auto& v : voices_) {
                if (!v->active) continue;
                v->stopping = true;
                v->gain.to(0.0f, fade_frames(c.fade, declick_frames));
            }
            return;
        }
    }
}

bool Mixer::stream_frame(Voice& v, std::uint64_t index, float& l, float& r) {
    while (index >= v.buf_start + v.buf_count) {
        if (v.ended) return false;
        // Keep the last frame: interpolation needs the frame before `index`.
        std::uint32_t keep = 0;
        if (v.buf_count > 0) {
            const std::size_t last = static_cast<std::size_t>(v.buf_count - 1) * channels;
            v.buf[0] = v.buf[last];
            v.buf[1] = v.buf[last + 1];
            v.buf_start += v.buf_count - 1;
            keep = 1;
        }
        const std::uint64_t want = stream_buffer_frames - keep;
        std::uint64_t got = v.stream->read(v.buf.data() + static_cast<std::size_t>(keep) * channels, want);
        // A looping stream is an endless one: rewind and keep filling.
        while (got < want && v.loop) {
            if (!v.stream->rewind()) break;
            const std::uint64_t n =
                v.stream->read(v.buf.data() + static_cast<std::size_t>(keep + got) * channels, want - got);
            if (n == 0) break; // an empty source
            got += n;
        }
        if (got < want) v.ended = true;
        v.buf_count = keep + static_cast<std::uint32_t>(got);
        if (got == 0) return false;
    }
    if (index < v.buf_start) return false;
    const std::size_t i = static_cast<std::size_t>(index - v.buf_start) * channels;
    l = v.buf[i];
    r = v.buf[i + 1];
    return true;
}

bool Mixer::sample(Voice& v, float& l, float& r) {
    if (v.finished) return false;
    const auto index = static_cast<std::uint64_t>(v.pos);
    const auto frac = static_cast<float>(v.pos - static_cast<double>(index));
    float l0 = 0.0f, r0 = 0.0f, l1 = 0.0f, r1 = 0.0f;
    if (v.stream) {
        if (!stream_frame(v, index, l0, r0)) return false;
        if (frac != 0.0f && !stream_frame(v, index + 1, l1, r1)) l1 = r1 = 0.0f;
        v.pos += v.pitch;
    } else {
        const std::uint64_t frames = v.sound->frames;
        const float* pcm = v.sound->pcm.data();
        if (index >= frames) return false;
        l0 = pcm[index * channels];
        r0 = pcm[index * channels + 1];
        if (frac != 0.0f) {
            std::uint64_t next = index + 1;
            if (next >= frames) next = v.loop ? 0 : frames;
            if (next < frames) {
                l1 = pcm[next * channels];
                r1 = pcm[next * channels + 1];
            }
        }
        v.pos += v.pitch;
        if (v.pos >= static_cast<double>(frames)) {
            if (v.loop) {
                while (v.pos >= static_cast<double>(frames)) v.pos -= static_cast<double>(frames);
            } else {
                v.finished = true;
            }
        }
    }
    l = l0 + (l1 - l0) * frac;
    r = r0 + (r1 - r0) * frac;
    return true;
}

void Mixer::mix(float* out, std::uint32_t frames) {
    std::fill(out, out + static_cast<std::size_t>(frames) * channels, 0.0f);
    constexpr std::size_t master = static_cast<std::size_t>(Bus::Master);
    for (std::uint32_t start = 0; start < frames; start += block_frames) {
        const std::uint32_t n = std::min(block_frames, frames - start);
        float* block = out + static_cast<std::size_t>(start) * channels;

        // Bus gains for this block, each already multiplied by master's.
        float* master_gain = &bus_block_[master * block_frames];
        for (std::uint32_t i = 0; i < n; ++i) master_gain[i] = buses_[master].next();
        for (std::size_t b = 0; b < bus_count; ++b) {
            if (b == master) continue;
            float* g = &bus_block_[b * block_frames];
            for (std::uint32_t i = 0; i < n; ++i) g[i] = buses_[b].next() * master_gain[i];
        }

        // Voices in slot order (a fixed order: the float sums are reproducible).
        for (auto& vp : voices_) {
            Voice& v = *vp;
            if (!v.active) continue;
            // A voice routed to Master gets master's gain once, not twice.
            const float* bus_gain = &bus_block_[static_cast<std::size_t>(v.bus) * block_frames];
            const float sound_volume = v.sound->config.volume;
            for (std::uint32_t i = 0; i < n; ++i) {
                float l = 0.0f, r = 0.0f;
                if (!sample(v, l, r)) {
                    stop_now(v);
                    break;
                }
                const float g = v.gain.next() * v.level.next() * sound_volume * bus_gain[i];
                block[i * 2] += l * g * v.pan_l;
                block[i * 2 + 1] += r * g * v.pan_r;
                if (v.finished || (v.stopping && v.gain.done())) {
                    stop_now(v);
                    break;
                }
            }
        }
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * channels; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

} // namespace cuervo::audio
