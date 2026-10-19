#include "editor/play_controller.hpp"

#include <chrono>

#include "cuervo/core/log.hpp"

namespace cuervo::editor {

namespace {

const std::optional<pluma::PanicInfo> no_panic;

} // namespace

PlayController::PlayController() = default;

PlayController::~PlayController() = default;

std::string_view PlayController::state_name() const {
    switch (state_) {
        case PlayState::Stopped: return "stopped";
        case PlayState::Playing: return "playing";
        case PlayState::Paused: return "paused";
    }
    return "?";
}

bool PlayController::open(std::string& error) {
    auto session = std::make_unique<script::GameSession>();
    if (!session->init(options_, error)) return false;
    session_ = std::move(session);
    timestep_.emplace(session_->project().tick_rate);
    since_watch_ = 0.0;
    last_tick_ms_ = 0.0;
    average_tick_ms_ = 0.0;
    return true;
}

bool PlayController::start(const script::SessionOptions& options, std::string& error, bool paused) {
    stop();
    options_ = options;
    mode_ = PlayMode::Live;
    replay_ = Replay{};
    if (!open(error)) return false;
    state_ = paused ? PlayState::Paused : PlayState::Playing;
    if (session_->host().panicked()) state_ = PlayState::Paused; // panicked while starting (ready())
    LOG_INFO("Play: '{}' from {}", session_->project().name,
             options.scene ? "the scene '" + options.scene_label + "'" : std::string("the project's main"));
    return true;
}

bool PlayController::start_replay(const script::SessionOptions& options, Replay replay, std::string& error) {
    stop();
    options_ = options;
    if (!script::session_options_for(replay, options_, error)) return false;
    mode_ = PlayMode::Replay;
    replay_ = std::move(replay);
    verified_ = 0;
    first_mismatch_.reset();
    if (!open(error)) return false;
    if (session_->input_map().action_names() != replay_.actions || session_->input_map().axis_names() != replay_.axes) {
        error = "the project's input actions or axes changed since the replay was recorded";
        stop();
        return false;
    }
    state_ = PlayState::Paused;
    LOG_INFO("Replay: {} ticks of '{}' (seed {})", replay_.ticks(), replay_.project, replay_.seed);
    return true;
}

void PlayController::stop() {
    if (session_ && session_->recording()) stop_recording();
    session_.reset();
    timestep_.reset();
    if (state_ != PlayState::Stopped) LOG_INFO("Play stopped");
    state_ = PlayState::Stopped;
    held_ = 0;
}

void PlayController::pause() {
    if (state_ == PlayState::Playing) state_ = PlayState::Paused;
}

void PlayController::resume() {
    if (state_ != PlayState::Paused || !session_) return;
    if (session_->host().panicked()) {
        LOG_WARN("The game is stopped by a panic: fix the script and save (hot reload), or Stop");
        return;
    }
    if (mode_ == PlayMode::Replay && tick() >= replay_length()) return;
    state_ = PlayState::Playing;
    if (timestep_) timestep_.emplace(session_->project().tick_rate); // no catch-up burst
}

std::uint64_t PlayController::tick() const { return session_ ? session_->host().tick_count() : 0; }

std::uint64_t PlayController::state_hash() const { return session_ ? session_->host().state_hash() : 0; }

const std::optional<pluma::PanicInfo>& PlayController::panic() const {
    return session_ ? session_->host().last_panic() : no_panic;
}

void PlayController::hold(std::size_t action, bool down) {
    if (action >= InputFrame::max_actions) return;
    const std::uint64_t bit = std::uint64_t{1} << action;
    held_ = down ? (held_ | bit) : (held_ & ~bit);
}

InputFrame PlayController::live_frame(DeviceState& devices, vec2 pointer) {
    InputFrame f = session_->sample(devices, pointer);
    f.held |= held_;
    return f;
}

void PlayController::run_tick(const InputFrame& frame) {
    const auto t0 = std::chrono::steady_clock::now();
    session_->tick(frame);
    last_tick_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    average_tick_ms_ = average_tick_ms_ == 0.0 ? last_tick_ms_ : average_tick_ms_ * 0.95 + last_tick_ms_ * 0.05;
    const std::uint64_t t = tick();
    if (mode_ == PlayMode::Replay && t >= 1 && t <= replay_.hashes.size() && t > verified_) {
        verified_ = t;
        const std::uint64_t expected = replay_.hashes[t - 1];
        const std::uint64_t actual = session_->host().state_hash();
        if (expected != actual && !first_mismatch_) {
            first_mismatch_ = t;
            mismatch_expected_ = expected;
            mismatch_actual_ = actual;
            LOG_WARN("Replay: state hash mismatch at tick {} (recorded {:016x}, now {:016x})", t, expected, actual);
        }
    }
    if (session_->host().panicked()) {
        state_ = PlayState::Paused;
        LOG_WARN("The game panicked at tick {}: paused (see the Debugger panel)", t);
    }
}

void PlayController::step(DeviceState& devices, vec2 pointer, std::uint64_t count) {
    if (!session_) return;
    for (std::uint64_t i = 0; i < count; ++i) {
        if (session_->host().panicked()) break;
        if (mode_ == PlayMode::Replay) {
            const std::uint64_t t = tick();
            if (t >= replay_length()) {
                if (state_ == PlayState::Playing) state_ = PlayState::Paused;
                break;
            }
            run_tick(replay_.frames[t]);
        } else {
            run_tick(live_frame(devices, pointer));
        }
    }
}

void PlayController::update(double seconds, DeviceState& devices, vec2 pointer) {
    if (state_ != PlayState::Playing || !session_) return;
    since_watch_ += seconds;
    if (since_watch_ >= 0.5) {
        since_watch_ = 0.0;
        reload(false);
        if (state_ != PlayState::Playing) return;
    }
    const double scale = mode_ == PlayMode::Replay ? static_cast<double>(speed) : 1.0;
    const FixedTimestep::Step s = timestep_->advance(seconds * scale);
    step(devices, pointer, static_cast<std::uint64_t>(s.ticks));
}

script::GameSession::Reload PlayController::reload(bool force) {
    if (!session_) return script::GameSession::Reload::None;
    const bool panicked = session_->host().panicked();
    const auto r = session_->reload(!force, force, false);
    if (r == script::GameSession::Reload::Reloaded) {
        LOG_INFO("Hot reload #{} while playing", session_->host().reloads());
        if (panicked && !session_->host().panicked()) LOG_INFO("The panic is fixed: press Play to resume");
    }
    return r;
}

void PlayController::start_recording() {
    if (!session_ || mode_ != PlayMode::Live) return;
    session_->start_recording();
    LOG_INFO("Recording from tick {}", tick());
    if (tick() > 0) {
        LOG_WARN("Recording started at tick {}, not 0: the replay can't reproduce the ticks before it, so it will "
                 "only verify if recorded from the start (Play with recording on)",
                 tick());
    }
}

void PlayController::stop_recording() {
    if (!session_ || !session_->recording()) return;
    session_->stop_recording();
    replay_ = session_->recorded();
    LOG_INFO("Recorded {} ticks", replay_.ticks());
}

bool PlayController::recording() const { return session_ && session_->recording(); }

const Replay& PlayController::recorded() const {
    if (session_ && session_->recording()) return session_->recorded();
    return mode_ == PlayMode::Live ? replay_ : empty_;
}

bool PlayController::seek(std::uint64_t target, std::string& error) {
    if (mode_ != PlayMode::Replay || !session_) {
        error = "seek needs a loaded replay";
        return false;
    }
    if (target > replay_length()) target = replay_length();
    if (target < tick()) {
        // No snapshots: go back to the start and simulate forward again.
        const PlayState was = state_;
        session_.reset();
        if (!open(error)) {
            state_ = PlayState::Stopped;
            return false;
        }
        verified_ = 0;
        first_mismatch_.reset();
        state_ = was;
    }
    DeviceState none;
    step(none, {0.0f, 0.0f}, target - tick());
    return true;
}

} // namespace cuervo::editor
