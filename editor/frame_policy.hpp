#pragma once
// When the RmlUi editor loop blocks and when it draws. Pure so the idle rules are testable:
// an idle editor sleeps on the event queue and draws only on the slow tick; after input it
// keeps drawing for `settle_s` so hover transitions and a 3-image swapchain both end on the
// final state (a single frame after a change can leave the previous image on screen).
namespace cuervo::editor {

struct FrameState {
    bool continuous = false;   // --continuous or a capture run
    bool busy = false;         // play, animation, scripted UI events, a pending debounce
    double since_input_s = 1e9;
    bool tick_due = false;     // the 4 Hz housekeeping tick
    unsigned frames = 0;       // frames drawn so far (the first few always draw)
};

inline constexpr double settle_s = 0.4;
inline constexpr unsigned warmup_frames = 8;

// True when the loop may sleep on the event queue instead of polling it.
constexpr bool may_block_for_events(const FrameState& s) {
    return !s.continuous && !s.busy && s.since_input_s >= settle_s && s.frames > warmup_frames;
}

// True when this iteration draws a frame.
constexpr bool should_draw(const FrameState& s) {
    return s.continuous || s.busy || s.tick_due || s.since_input_s <= settle_s || s.frames <= warmup_frames;
}

} // namespace cuervo::editor
