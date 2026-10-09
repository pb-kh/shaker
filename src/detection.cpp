#include "shaker/detection.hpp"

#include <cmath>

namespace shaker {

namespace {
// Sustained-window smoothing constant (research R-04): ~2-5 ms holes in
// real oscillation (deceleration through the reversal point) must not
// break the window, while a stopped device still decays below any
// threshold within ~5·τ ≈ 100 ms (single bumps cannot trigger).
constexpr double kSmoothTauUsec = 20'000.0;
// Guard: treat δt below this as a non-advancing/duplicate timestamp.
constexpr int64_t kMinDtUsec = 1;

[[nodiscard]] double dt_seconds(int64_t dt_usec)
{
    return static_cast<double>(dt_usec) / 1'000'000.0;
}
} // namespace

DetectionResult DetectionCore::feed(const Sample& sample)
{
    const DetectionResult none{};

    // No anchor yet: the very first sample cannot express speed.
    if (last_t_usec_ < 0) {
        last_t_usec_ = sample.t_usec;
        ema_ = 0.0;
        peak_ = 0.0;
        return none;
    }

    // Regressing timeline: drop the sample, keep the state and anchor.
    if (sample.t_usec < last_t_usec_) {
        return none;
    }
    const int64_t dt = sample.t_usec - last_t_usec_;
    last_t_usec_ = sample.t_usec;
    if (dt < kMinDtUsec) {
        // Non-advancing timestamp (poll-rate duplicate): not a speed
        // observation; core state must not be corrupted by it.
        return none;
    }

    // Instantaneous average speed over this segment, smoothed by an EMA
    // with τ = kSmoothTauUsec (event-time based, replayable).
    const double instant = std::hypot(sample.dx, sample.dy) / dt_seconds(dt);
    const double alpha = 1.0 - std::exp(-static_cast<double>(dt) / kSmoothTauUsec);
    ema_ += (instant - ema_) * alpha;

    switch (state_) {
    case DetectionState::Cooldown:
        if (sample.t_usec < cooldown_until_usec_) {
            return none;
        }
        // Cooldown elapsed: become Idle; sustained evaluation resumes with
        // the NEXT sample (data-model.md §3).
        state_ = DetectionState::Idle;
        peak_ = 0.0;
        return none;

    case DetectionState::Idle:
        if (ema_ < params_.sensitivity) {
            return none;
        }
        state_ = DetectionState::Sustaining;
        window_start_usec_ = sample.t_usec;
        peak_ = ema_;
        return none;

    case DetectionState::Sustaining: {
        if (ema_ < params_.sensitivity) {
            // Window broken by sub-threshold motion.
            state_ = DetectionState::Idle;
            peak_ = 0.0;
            return none;
        }
        peak_ = std::max(peak_, ema_);
        const int64_t elapsed = sample.t_usec - window_start_usec_;
        if (elapsed < params_.duration_usec) {
            return none;
        }
        // Trigger: consume this stretch and open the cooldown window.
        state_ = DetectionState::Cooldown;
        cooldown_until_usec_ = sample.t_usec + params_.cooldown_usec;
        const DetectionResult hit{
            .triggered = true,
            .at_usec = sample.t_usec,
            .peak_speed = peak_,
        };
        peak_ = 0.0;
        return hit;
    }
    }
    return none; // unreachable
}

} // namespace shaker
