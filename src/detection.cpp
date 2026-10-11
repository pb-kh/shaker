#include "shaker/detection.hpp"

#include <algorithm>
#include <cmath>

namespace shaker {

namespace {
// Trails whose bounding box is smaller than this (pointer units, diagonal)
// are never a shake: hand tremor and sub-pixel jitter reverse direction
// constantly but stay inside a tiny box (KWin: 100 logical pixels;
// libinput's accelerated deltas are in the same unit).
constexpr double kMinDiagonal = 100.0;
// Per-axis motion below this (pointer units) counts as moving in either
// direction, so one-unit jitter does not split a straight stroke into
// many segments (KWin: 1).
constexpr double kDirectionTolerance = 1.0;

[[nodiscard]] bool same_direction(double a, double b)
{
    return (a >= -kDirectionTolerance && b >= -kDirectionTolerance)
           || (a <= kDirectionTolerance && b <= kDirectionTolerance);
}
} // namespace

void DetectionCore::push(const Vertex& v)
{
    if (count_ == kCapacity) {
        pop_front();
    }
    at(count_) = v;
    ++count_;
}

void DetectionCore::pop_front()
{
    head_ = (head_ + 1) % kCapacity;
    --count_;
}

void DetectionCore::reset()
{
    head_ = 0;
    count_ = 0;
    last_factor_ = 0.0;
    if (state_ == DetectionState::Sustaining) {
        state_ = DetectionState::Idle;
    }
}

int64_t DetectionCore::window_elapsed_usec() const
{
    return count_ < 2 ? 0 : at(count_ - 1).t_usec - at(0).t_usec;
}

DetectionResult DetectionCore::feed(const Sample& sample)
{
    const DetectionResult none{};

    // No anchor yet: the very first sample cannot express a speed, only
    // spawn the timeline (and the trail).
    if (last_t_usec_ < 0) {
        last_t_usec_ = sample.t_usec;
        x_ += sample.dx;
        y_ += sample.dy;
        push(Vertex{x_, y_, sample.t_usec});
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

    last_speed_ = std::hypot(sample.dx, sample.dy)
                  / (static_cast<double>(dt) / 1'000'000.0);
    x_ += sample.dx;
    y_ += sample.dy;
    const Vertex now{x_, y_, sample.t_usec};

    if (state_ == DetectionState::Cooldown) {
        if (sample.t_usec < cooldown_until_usec_) {
            return none;
        }
        // Cooldown over: hunt again from a fresh trail anchored here.
        state_ = DetectionState::Idle;
        reset();
        push(now);
        return none;
    }

    // Forget trail vertices older than the window.
    while (count_ > 0 && sample.t_usec - at(0).t_usec >= params_.duration_usec) {
        pop_front();
    }

    // Still moving the same way: extend the last segment instead of adding
    // a vertex. Only direction changes are evaluated (KWin, PowerToys).
    if (count_ >= 2) {
        Vertex& last = at(count_ - 1);
        const Vertex& prev = at(count_ - 2);
        if (same_direction(last.x - prev.x, now.x - last.x)
            && same_direction(last.y - prev.y, now.y - last.y)) {
            last = now;
            return none;
        }
    }
    push(now);

    double left = at(0).x;
    double right = left;
    double top = at(0).y;
    double bottom = top;
    double path = 0.0;
    for (std::size_t i = 1; i < count_; ++i) {
        const Vertex& a = at(i - 1);
        const Vertex& b = at(i);
        path += std::hypot(b.x - a.x, b.y - a.y);
        left = std::min(left, b.x);
        right = std::max(right, b.x);
        top = std::min(top, b.y);
        bottom = std::max(bottom, b.y);
    }

    const double diagonal = std::hypot(right - left, bottom - top);
    if (diagonal < kMinDiagonal) {
        last_factor_ = 0.0;
        state_ = DetectionState::Idle;
        return none;
    }
    last_factor_ = path / diagonal;

    if (last_factor_ > params_.sensitivity) {
        // Shake confirmed: consume the trail, open the cooldown window at
        // event time.
        state_ = DetectionState::Cooldown;
        cooldown_until_usec_ = sample.t_usec + params_.cooldown_usec;
        head_ = 0;
        count_ = 0;
        return DetectionResult{
            .triggered = true,
            .at_usec = sample.t_usec,
            .peak_speed = last_speed_,
        };
    }

    // SUSTAIN: the trail is past half the trigger factor — an oscillation
    // is forming (`--debug` visibility only).
    state_ = last_factor_ >= params_.sensitivity / 2.0 ? DetectionState::Sustaining
                                                       : DetectionState::Idle;
    return none;
}

} // namespace shaker
