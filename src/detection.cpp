#include "shaker/detection.hpp"

#include <cmath>

namespace shaker {

namespace {
// A shake needs at least this many direction changes (kReversalsRequired)
// inside the `duration` window to count as a shake instead of one-off
// gestures, doodles, or plain pointer wandering (research R-04 v2). Three
// is the minimum that separates "there and back" from continued
// oscillation.
constexpr int kReversalsRequired = 3;
// Longest tolerated idle gap before the swing run resets (a device at
// rest must not carry stale swing state into the next fetch).
constexpr int64_t kIdleResetGapUsec = 500'000;
} // namespace

void DetectionCore::reset_window()
{
    swings_ = 0;
    window_open_ = false;
    first_flip_t_usec_ = 0;
    for (auto& axis_phase : phase_) {
        axis_phase = 0;
    }
    for (auto& axis_extreme : extreme_) {
        axis_extreme = 0.0;
    }
}

void DetectionCore::reset_all()
{
    reset_window();
    for (auto& axis_position : position_) {
        axis_position = 0.0;
    }
}

DetectionResult DetectionCore::feed(const Sample& sample)
{
    const DetectionResult none{};

    // No anchor yet: the very first sample cannot express displacement
    // intervals, only spawn the timeline.
    if (last_t_usec_ < 0) {
        last_t_usec_ = sample.t_usec;
        return none;
    }

    // Regressing timeline: drop the sample, keep the state and anchor.
    if (sample.t_usec < last_t_usec_) {
        return none;
    }
    const int64_t dt = sample.t_usec - last_t_usec_;
    last_t_usec_ = sample.t_usec;
    if (dt < 1) {
        // Non-advancing timestamp (poll-rate duplicate): not a movement
        // observation; the core state must not be corrupted by it.
        return none;
    }

    const double swing_speed = std::hypot(sample.dx, sample.dy)
                               / (static_cast<double>(dt) / 1'000'000.0);
    last_speed_ = swing_speed;

    // An idle gap restarts the whole run (device was at rest).
    if (dt > kIdleResetGapUsec) {
        reset_all();
    }

    // Integrate displacement along both axes.
    position_[kAxisX] += sample.dx;
    position_[kAxisY] += sample.dy;

    switch (state_) {
    case DetectionState::Cooldown:
        if (sample.t_usec < cooldown_until_usec_) {
            return none;
        }
        state_ = DetectionState::Idle;
        reset_all();
        return none;

    case DetectionState::Idle:
    case DetectionState::Sustaining: {
        for (int axis = 0; axis < kAxisCount; ++axis) {
            double& pos = position_[axis];
            const double threshold_excurse = params_.sensitivity;
            int& phase = phase_[axis];
            double& extreme = extreme_[axis];

            if (phase == 0) {
                // Any axis reaching meaningful displacement opens rising or
                // falling phase for that axis.
                if (pos > threshold_excurse) {
                    phase = +1;
                    extreme = pos;
                } else if (pos < -threshold_excurse) {
                    phase = -1;
                    extreme = pos;
                }
                continue;
            }
            if (phase == +1) {
                if (pos > extreme) {
                    extreme = pos; // still moving out
                } else if (extreme - pos >= threshold_excurse) {
                    // Moved back by a full swing leg: count a direction
                    // change and start the opposite phase.
                    ++swings_;
                    if (!window_open_) {
                        window_open_ = true;
                        first_flip_t_usec_ = sample.t_usec;
                    }
                    phase = -1;
                    extreme = pos;
                }
            } else { // phase == -1
                if (pos < extreme) {
                    extreme = pos;
                } else if (pos - extreme >= threshold_excurse) {
                    ++swings_;
                    if (!window_open_) {
                        window_open_ = true;
                        first_flip_t_usec_ = sample.t_usec;
                    }
                    phase = +1;
                    extreme = pos;
                }
            }
        }

        if (window_open_) {
            // The window stayed open: the state machine reports SUSTAIN
            // from the first counted swing until the window settles.
            state_ = DetectionState::Sustaining;

            const int64_t window_elapsed = sample.t_usec - first_flip_t_usec_;
            if (window_elapsed > params_.duration_usec) {
                // The swing run did not finish fast enough: reopen the hunt
                // fresh (positions keep drifting; extremes re-anchor). The
                // window closes NOW — no next sample is needed for the
                // reported state to settle.
                reset_window();
                state_ = DetectionState::Idle;
                return none;
            }
            if (swings_ >= kReversalsRequired) {
                // Shake confirmed within the window: consume it, open the
                // cooldown window at event time.
                state_ = DetectionState::Cooldown;
                cooldown_until_usec_ = sample.t_usec + params_.cooldown_usec;
                const DetectionResult hit{
                    .triggered = true,
                    .at_usec = sample.t_usec,
                    .peak_speed = swing_speed,
                };
                reset_all();
                return hit;
            }
        }
        if (state_ == DetectionState::Sustaining && !window_open_) {
            state_ = DetectionState::Idle;
        }
        return none;
    }
    }
    return none; // unreachable
}

} // namespace shaker
