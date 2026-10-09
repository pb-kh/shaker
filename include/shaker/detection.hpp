// Pure, hardware-agnostic shake detection core (constitution V).
//
// v2 semantics (research R-04): a shake is an oscillation — repeated
// direction changes with real (but bounded) displacement at a fast
// cadence. The core accumulates displacement per axis and, with per-axis
// hysteresis, counts a *swing* whenever an axis position moves back by at
// least `sensitivity` pointer units from its phase extreme. A shake
// triggers when `kReversalsRequired` (internal constant, 3) swings occur
// within `duration` seconds (event-time anchored). Two internal gates
// keep non-shake movement silent: legs longer than 350 ms (slow deliberate
// sweeps) and legs covering more than 2000 pointer units (full-screen
// travel) reject the swing run. The downstream Idle/Sustaining/Cooldown
// state machine semantics, cooldown gating and output shape are unchanged
// from data-model.md §3/§4.
//
// The core contains no I/O, no clocks, and no allocations: behavior is a
// deterministic function of (DetectionParams, sample history), so it is
// fully replayable in unit tests without device access. Timestamps are
// monotonic microseconds (device event time), never the wall clock.
#pragma once

#include <cstdint>
#include <string_view>

namespace shaker {

struct DetectionParams {
    double sensitivity = 40.0; // pointer units per swing leg (hysteresis)
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
    int64_t at_usec = 0;     // timestamp of the triggering sample (if triggered)
    double peak_speed = 0.0; // swing speed (units/s) at the triggering sample
};

class DetectionCore {
public:
    explicit DetectionCore(DetectionParams params) : params_(params) {}

    // Process one sample; returns the detection outcome for this sample.
    // Samples with regressing or duplicate timestamps are dropped
    // (returned result is a no-trigger) without corrupting state.
    DetectionResult feed(const Sample& sample);

    [[nodiscard]] DetectionState state() const { return state_; }

    // Swing count within the currently open window (0 while no window is
    // open) — the host layer surfaces it in the `--debug` trace.
    [[nodiscard]] int swings() const { return swings_; }

    // Most recent instantaneous swing speed (units/s) — used by the host
    // layer's `--debug` trace ("speed=").
    [[nodiscard]] double last_speed() const { return last_speed_; }

    // Elapsed window time in µs — only meaningful while a window is open
    // (Sustaining); hosts use it for the `--debug` trace.
    [[nodiscard]] int64_t window_elapsed_usec() const
    {
        return state_ == DetectionState::Sustaining
                   ? last_t_usec_ - first_flip_t_usec_
                   : 0;
    }

private:
    enum Axis { kAxisX = 0, kAxisY = 1, kAxisCount = 2 };

    void reset_window();
    void reset_all();

    DetectionParams params_;

    int64_t last_t_usec_ = -1; // -1 = no anchor yet

    // Per-axis hysteresis run (one machine per axis; either axis can carry
    // the shake).
    double position_[kAxisCount] = {0.0, 0.0}; // running displacement
    double extreme_[kAxisCount] = {0.0, 0.0};  // phase extreme of position
    int phase_[kAxisCount] = {0, 0};           // 0 none, +1 rising, -1 falling
    int64_t leg_start_t_usec_[kAxisCount] = {0, 0};    // leg open time
    double leg_start_pos_[kAxisCount] = {0.0, 0.0};    // leg open position

    int swings_ = 0;                 // swings within the open window
    int64_t first_flip_t_usec_ = 0;  // window anchor (event time)
    bool window_open_ = false;
    double last_speed_ = 0.0;        // most recent instantaneous swing speed

    DetectionState state_ = DetectionState::Idle;
    int64_t cooldown_until_usec_ = 0;
};

} // namespace shaker
