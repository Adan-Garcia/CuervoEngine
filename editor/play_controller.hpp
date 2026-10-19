#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cuervo/core/fixed_timestep.hpp"
#include "cuervo/input/replay.hpp"
#include "cuervo/script/game_session.hpp"

// Play-in-editor (DESIGN.md 4.13, "Clarified (Phase 6B)"): the game runs on
// a script::GameSession -- the same code `cuervo --game` runs -- in its own
// ScriptHost and VM, so playing never touches the edited document or the
// scene view's preview, and Stop just drops the session. The controller is
// headless (no window): the app feeds it real time and devices, command
// scripts and tests drive it tick by tick.
//
// Two modes:
//   Live    input comes from the devices (and `hold`); optionally recorded
//   Replay  input comes from a Replay; every tick's state hash is compared
//           with the recorded one and the first mismatch is kept. Seeking
//           re-simulates from the start (the VM has no snapshots), forward
//           seeks continue from the current tick.
// A panic pauses the game (DESIGN.md 5.8); panic() has its frames and
// locals. Saving a script while playing hot-reloads it (5.13); a reload
// that fixes the panic lets the game resume.
namespace cuervo::editor {

enum class PlayState { Stopped, Playing, Paused };
enum class PlayMode { Live, Replay };

class PlayController {
public:
    PlayController();
    ~PlayController();
    PlayController(const PlayController&) = delete;
    PlayController& operator=(const PlayController&) = delete;

    // Starts a live session (paused if `paused`). `options` are the
    // session's: project dir, font, audio, jobs, and the scene to play
    // (empty = the project's main, exactly like `cuervo --game`).
    bool start(const script::SessionOptions& options, std::string& error, bool paused = false);
    // Starts playing `replay` from its first tick (paused).
    bool start_replay(const script::SessionOptions& options, Replay replay, std::string& error);
    void stop();
    void pause();
    void resume();

    PlayState state() const { return state_; }
    PlayMode mode() const { return mode_; }
    bool active() const { return state_ != PlayState::Stopped; }
    std::string_view state_name() const;

    // Real time passed: runs the ticks due at the fixed rate (times
    // `speed`, replays only), polls for script changes twice a second.
    // Live input is sampled from `devices` with the pointer in virtual
    // pixels. Does nothing while stopped or paused.
    void update(double seconds, DeviceState& devices, vec2 pointer);
    // Exactly `count` ticks now, whatever the state (Step while paused,
    // command scripts). Stops early at a panic or the end of a replay.
    void step(DeviceState& devices, vec2 pointer, std::uint64_t count = 1);

    // Actions held on every live tick (command scripts' `hold`), by index
    // bit, on top of the devices.
    void hold(std::size_t action, bool down);
    std::uint64_t held() const { return held_; }

    // Hot reload: `force` reloads now, otherwise only when a script changed.
    script::GameSession::Reload reload(bool force);

    // ---- recording (live mode)
    void start_recording();
    void stop_recording();
    bool recording() const;
    // The recording so far (frames + hashes); empty header when none.
    const Replay& recorded() const;

    // ---- replay mode
    const Replay& replay() const { return replay_; }
    std::uint64_t replay_length() const { return replay_.ticks(); }
    // Moves to `tick` (0 = before the first tick): forward by simulating,
    // backward by restarting the session and re-simulating.
    bool seek(std::uint64_t tick, std::string& error);
    float speed = 1.0f;
    // Hashes compared so far, and the first tick whose hash differed.
    std::uint64_t verified() const { return verified_; }
    std::optional<std::uint64_t> first_mismatch() const { return first_mismatch_; }
    // Recorded vs actual hash at the first mismatch.
    std::uint64_t mismatch_expected() const { return mismatch_expected_; }
    std::uint64_t mismatch_actual() const { return mismatch_actual_; }

    // ---- state
    script::GameSession* session() { return session_.get(); }
    const script::GameSession* session() const { return session_.get(); }
    std::uint64_t tick() const;
    std::uint64_t state_hash() const;
    // The panic that paused the game, if any.
    const std::optional<pluma::PanicInfo>& panic() const;
    // Simulation milliseconds per tick: the last tick and a running average.
    double last_tick_ms() const { return last_tick_ms_; }
    double average_tick_ms() const { return average_tick_ms_; }

private:
    bool open(std::string& error);
    void run_tick(const InputFrame& frame);
    InputFrame live_frame(DeviceState& devices, vec2 pointer);

    script::SessionOptions options_;
    std::unique_ptr<script::GameSession> session_;
    std::optional<FixedTimestep> timestep_;
    PlayState state_ = PlayState::Stopped;
    PlayMode mode_ = PlayMode::Live;
    std::uint64_t held_ = 0;
    double since_watch_ = 0.0;
    double last_tick_ms_ = 0.0;
    double average_tick_ms_ = 0.0;
    Replay replay_;
    Replay empty_;
    std::uint64_t verified_ = 0;
    std::optional<std::uint64_t> first_mismatch_;
    std::uint64_t mismatch_expected_ = 0;
    std::uint64_t mismatch_actual_ = 0;
};

} // namespace cuervo::editor
