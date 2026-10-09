// Pure, hardware-agnostic shake detection core (constitution V).
//
// The core consumes plain motion samples (event timestamps + pointer
// deltas, see data-model.md §1) and emits detection results. It contains
// no I/O, no clocks, and no allocations: behavior is a deterministic
// function of (DetectionParams, sample history), so it is fully replayable
// in unit tests without device access. Numbers desk anchor on event time
// (monotonic microseconds), never the wall clock.
//
// Transition table: data-model.md §3. Speed is smoothed by a fixed
// internal EMA (research R-04 refinement) so the near-zero instantaneous
// speed at direction reversals doesn't shatter a sustained window.
#pragma once

#include <cstdint>
#include <string_view>

namespace shaker {

struct DetectionParams {
    double sensitivity = 8000.0; // device units / second
    int64_t duration_usec = 250'000;
    int64_t cooldown_usec = 2'000'000;
};

struct Sample {
    int64_t t_usec = 0; // device event time (monotonic µs), non-decreasing
    double dx = 0.0;    // pointer delta since the previous motion event
    double dy = 0.0;
};

enum class DetectionState { Idle, Sustaining, Cooldown };

// Human-readable state tag for diagnostics, matching the `--debug` trace
// contract (IDLE / SUSTAIN / COOLDOWN).
[[nodiscard]] constexpr std::string_view state_name(DetectionState state)
{
    switch (state) {
    case DetectionState::Idle: return "IDLE";
    case DetectionState::Sustaining: return "SUSTAIN";
    case DetectionState::Cooldown: return "COOLDOWN";
    }
    return "UNKNOWN";
}

struct DetectionResult {
    bool triggered = false;
    int64_t at_usec = 0;  // timestamp of the triggering sample (if triggered)
    double peak_speed = 0.0; // highest smoothed speed within the window
};

class DetectionCore {
public:
    explicit DetectionCore(DetectionParams params) : params_(params) {}

    // Process one sample; returns the detection outcome for this sample.
    // Samples with regressing or duplicate timestamps are dropped
    // (returned result is a no-trigger) without corrupting state.
    DetectionResult feed(const Sample& sample);

    [[nodiscard]] DetectionState state() const { return state_; }
    [[nodiscard]] double ema_speed() const { return ema_; } // smoothed speed (units/s)

    // Elapsed sustained-window time in µs (only meaningful while
    // Sustaining; hosts use it for the --debug trace).
    [[nodiscard]] int64_t window_elapsed_usec() const
    {
        return state_ == DetectionState::Sustaining
                   ? last_t_usec_ - window_start_usec_
                   : 0;
    }

private:
    DetectionParams params_;

    int64_t last_t_usec_ = -1; // -1 = no anchor yet
    DetectionState state_ = DetectionState::Idle;
    int64_t window_start_usec_ = 0;
    int64_t cooldown_until_usec_ = 0;
    double ema_ = 0.0;   // smoothed speed estimate (units/s)
    double peak_ = 0.0;  // max smoothed speed inside the current window
};

} // namespace shaker
