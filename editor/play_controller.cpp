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

void PlayController::debug_tick() {
    if (!debug_) return;
    debug_->pump();
    debug_->debugger().poll(); // breakpoint changes, pause and disconnect requests, once per frame
    if (debug_->debugger().terminate_requested() || debug_->terminated()) deferred_stop_ = true;
}

bool PlayController::open(std::string& error) {
    debug_.reset();
    bool any = false;
    for (const auto& [file, lines] : debug_bps_) any = any || !lines.empty();
    if (mode_ == PlayMode::Live && any && !options_.dir.empty()) {
        auto dbg = std::make_unique<DebugSession>();
        if (dbg->start(options_.dir, debug_bps_)) {
            pluma::dap::Debugger* d = &dbg->debugger();
            options_.debug_hooks = d;
            options_.on_active_vm = [d, prev = options_.on_active_vm](pluma::VM& vm) { if (prev) prev(vm); d->attach(vm); };
            d->set_idle([this] {
                blocked_ = true;
                if (debug_) debug_->pump();
                if (debug_idle_) debug_idle_();
                blocked_ = false;
            });
            debug_ = std::move(dbg);
        }
    }
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
    if (blocked_) { error = "The game is stopped at a breakpoint: continue or stop it first."; return false; }
    stop();
    options_ = options;
    mode_ = PlayMode::Live;
    replay_ = Replay{};
    if (!open(error)) return false;
    if (deferred_stop_) { // Stop was pressed while the game sat at a breakpoint inside ready()
        deferred_stop_ = false;
        stop();
        return true;
    }
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
    drop_snapshots();
    if (!open(error)) return false;
    // Actions added, removed or reordered since the recording are mapped by their stable IDs.
    if (!adapt_replay_input(replay_, session_->input_map(), error)) {
        stop();
        return false;
    }
    state_ = PlayState::Paused;
    keep_snapshot(); // tick 0: backward seeks never re-run ready()
    LOG_INFO("Replay: {} ticks of '{}' (seed {})", replay_.ticks(), replay_.project, replay_.seed);
    return true;
}

void PlayController::drop_snapshots() {
    snapshots_.clear();
    interval_ = snapshot_interval;
    snapshots_failed_ = false;
}

// Saves the simulation if the tick is on the interval. Snapshots are only taken between ticks, so the
// state they hold is exactly the replay's state at that tick.
void PlayController::keep_snapshot() {
    if (mode_ != PlayMode::Replay || !session_ || snapshots_failed_ || interval_ == 0) return;
    const std::uint64_t t = tick();
    if (t % interval_ != 0 || snapshots_.contains(t)) return;
    script::SimSnapshot snap;
    std::string error;
    if (!session_->host().save(snap, error)) {
        LOG_WARN("Replay: snapshots are off ({}); backward seeks restart from tick 0", error);
        snapshots_failed_ = true;
        snapshots_.clear();
        return;
    }
    snapshots_.emplace(t, std::move(snap));
    if (snapshots_.size() > max_snapshots) {
        // Keep every other snapshot (and tick 0): the interval doubles.
        interval_ *= 2;
        for (auto it = snapshots_.begin(); it != snapshots_.end();) {
            it = it->first % interval_ != 0 ? snapshots_.erase(it) : std::next(it);
        }
    }
}

void PlayController::stop() {
    if (blocked_) { // the VM is inside the debugger: let it finish, then stop (see update())
        deferred_stop_ = true;
        if (debug_) debug_->disconnect();
        return;
    }
    drop_snapshots();
    if (session_ && session_->recording()) stop_recording();
    session_.reset();
    debug_.reset();
    deferred_stop_ = false;
    timestep_.reset();
    if (state_ != PlayState::Stopped) LOG_INFO("Play stopped");
    state_ = PlayState::Stopped;
    held_ = 0;
}

void PlayController::pause() {
    if (state_ == PlayState::Playing) state_ = PlayState::Paused;
}

void PlayController::resume() {
    if (blocked_ && debug_) { debug_->resume(); return; }
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

void PlayController::run_tick(const InputFrame& frame, std::uint64_t index) {
    const auto t0 = std::chrono::steady_clock::now();
    if (mode_ == PlayMode::Replay && !replay_.extra_players.empty()) {
        // A networked recording (replay format 4): every player's frame of the tick goes in together,
        // exactly as verify_replay does, or the match diverges. Snapshots hold the extra players' input
        // history (ScriptHost::save), so backward seeks keep them too.
        players_.assign(1, frame);
        for (const auto& extra : replay_.extra_players) players_.push_back(index < extra.size() ? extra[index] : InputFrame{});
        session_->tick_players(players_);
    } else {
        session_->tick(frame);
    }
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
    if (!session_->host().panicked()) keep_snapshot();
    if (session_->host().panicked()) {
        state_ = PlayState::Paused;
        LOG_WARN("The game panicked at tick {}: paused (see the Debugger panel)", t);
    }
}

void PlayController::step(DeviceState& devices, vec2 pointer, std::uint64_t count) {
    if (blocked_ && debug_) { debug_->step_over(); return; }
    if (!session_) return;
    for (std::uint64_t i = 0; i < count; ++i) {
        if (session_->host().panicked()) break;
        if (mode_ == PlayMode::Replay) {
            const std::uint64_t t = tick();
            if (t >= replay_length()) {
                if (state_ == PlayState::Playing) state_ = PlayState::Paused;
                break;
            }
            run_tick(replay_.frames[t], t);
        } else {
            run_tick(live_frame(devices, pointer), 0);
        }
        if (deferred_stop_) break;
    }
}

void PlayController::update(double seconds, DeviceState& devices, vec2 pointer) {
    if (blocked_) return;
    debug_tick();
    if (deferred_stop_) { stop(); return; }
    if (state_ != PlayState::Playing || !session_) return;
    since_watch_ += seconds;
    if (since_watch_ >= 0.5) {
        since_watch_ = 0.0;
        reload(false);
        if (state_ != PlayState::Playing) return;
    }
    const double scale = mode_ == PlayMode::Replay ? static_cast<double>(speed) : 1.0;
    const FixedTimestep::Step s = timestep_->advance(seconds * scale);
    const std::uint64_t stops = debug_ ? debug_->debugger().stops() : 0;
    step(devices, pointer, static_cast<std::uint64_t>(s.ticks));
    // Time spent stopped at a breakpoint is not caught up with.
    if (debug_ && debug_->debugger().stops() != stops && session_) timestep_.emplace(session_->project().tick_rate);
    if (deferred_stop_) stop();
}

script::GameSession::Reload PlayController::reload(bool force) {
    if (!session_ || blocked_) return script::GameSession::Reload::None;
    const bool panicked = session_->host().panicked();
    const auto r = session_->reload(!force, force, false);
    if (r == script::GameSession::Reload::Reloaded) {
        LOG_INFO("Hot reload #{} while playing", session_->host().reloads());
        drop_snapshots(); // they belong to the old scripts
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
    last_seek_ = {tick(), 0, false};
    if (target < tick()) {
        const PlayState was = state_;
        bool resumed = false;
        // Resume from the nearest snapshot at or before the target.
        auto it = snapshots_.upper_bound(target);
        if (it != snapshots_.begin()) {
            --it;
            if (session_->host().restore(it->second, error)) {
                resumed = true;
                last_seek_ = {it->first, 0, true};
                // What was compared up to the snapshot stays compared; later ticks are compared again.
                if (verified_ > it->first) verified_ = it->first;
                if (first_mismatch_ && *first_mismatch_ > it->first) first_mismatch_.reset();
            } else {
                LOG_WARN("Replay: restoring the snapshot at tick {} failed ({}); restarting from tick 0", it->first, error);
                snapshots_.clear();
                snapshots_failed_ = true;
            }
        }
        if (!resumed) {
            // No snapshot to use: go back to the start and simulate forward again.
            session_.reset();
            if (!open(error)) {
                state_ = PlayState::Stopped;
                return false;
            }
            verified_ = 0;
            first_mismatch_.reset();
            last_seek_ = {0, 0, false};
        }
        state_ = was;
    }
    DeviceState none;
    last_seek_.simulated = target - tick();
    step(none, {0.0f, 0.0f}, target - tick());
    return true;
}

} // namespace cuervo::editor
