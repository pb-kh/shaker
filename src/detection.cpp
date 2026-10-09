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
// Maximum flip interval (event µs) for a direction change to count as
// part of a shake: shaking is FAST oscillation. Slow deliberate sweeps
// (big amplitude, long legs) produce flips farther apart than this and
// abandoned windows instead of accumulating swings (user-validated:
// slow side-to-side movement with big amplitude must stay silent).
constexpr int64_t kMaxFlipIntervalUsec = 350'000;
// Maximum displacement (pointer units) a single swing leg may cover:
// real shakes sweep an arm's worth of a desk at most. Anything larger is
// cursor travel, not shaking (user-validated: full-screen sweeps must
// stay silent).
constexpr double kMaxLegDisplacement = 2000.0;
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
            const int64_t& leg_start_t = leg_start_t_usec_[axis];
            double& leg_start_pos = leg_start_pos_[axis];

            // A counted flip: bookkeeping shared by both phases.
            const auto count_flip = [&]() {
                ++swings_;
                if (!window_open_) {
                    window_open_ = true;
                    first_flip_t_usec_ = sample.t_usec;
                }
                phase = -phase;
                extreme = pos;
                leg_start_t_usec_[axis] = sample.t_usec;
                leg_start_pos = pos;
            };
            // A flip that fails the travel/cadence gates: poison the
            // window (reset the run) but re-anchor the fresh opposite
            // phase at this point so the hunt continues.
            const auto reject_leg = [&]() {
                reset_window();
                state_ = DetectionState::Idle;
                phase = -phase;
                extreme = pos;
                leg_start_t_usec_[axis] = sample.t_usec;
                leg_start_pos = pos;
            };

            if (phase == 0) {
                // Any axis reaching meaningful displacement opens rising or
                // falling phase for that axis (the leg starts here).
                if (pos > threshold_excurse) {
                    phase = +1;
                    extreme = pos;
                    leg_start_t_usec_[axis] = sample.t_usec;
                    leg_start_pos = pos;
                } else if (pos < -threshold_excurse) {
                    phase = -1;
                    extreme = pos;
                    leg_start_t_usec_[axis] = sample.t_usec;
                    leg_start_pos = pos;
                }
                continue;
            }
            if (phase == +1) {
                if (pos > extreme) {
                    extreme = pos; // still moving out
                } else if (extreme - pos >= threshold_excurse) {
                    // Moved back by a full swing leg: candidate direction
                    // change. Travel/cadence gates: shakes are FAST small
                    // oscillation — slow deliberate sweeps (long legs) and
                    // full-screen travel (oversized legs) are cursor
                    // movement, not shaking (user-validated: slow
                    // side-to-side movement with big amplitude must stay
                    // silent).
                    const int64_t leg_duration =
                        sample.t_usec - leg_start_t;
                    const double leg_travel =
                        std::fabs(extreme - leg_start_pos);
                    if (leg_duration > kMaxFlipIntervalUsec
                        || leg_travel > kMaxLegDisplacement) {
                        reject_leg();
                        continue;
                    }
                    count_flip();
                }
            } else { // phase == -1
                if (pos < extreme) {
                    extreme = pos;
                } else if (pos - extreme >= threshold_excurse) {
                    const int64_t leg_duration =
                        sample.t_usec - leg_start_t;
                    const double leg_travel =
                        std::fabs(extreme - leg_start_pos);
                    if (leg_duration > kMaxFlipIntervalUsec
                        || leg_travel > kMaxLegDisplacement) {
                        reject_leg();
                        continue;
                    }
                    count_flip();
                }
            }
        }

        if (window_open_ && swings_ >= 2) {
            // The state machine reports SUSTAIN once an oscillation is
            // actually forming (two swings); a lone candidate swing keeps
            // the reported state Idle so slow sweeps (whose cadence/travel
            // gates keep resetting runs at swing 1) never read as SUSTAIN.
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
        if (state_ == DetectionState::Sustaining
            && (!window_open_ || swings_ < 2)) {
            state_ = DetectionState::Idle;
        }
        return none;
    }
    }
    return none; // unreachable
}

} // namespace shaker
