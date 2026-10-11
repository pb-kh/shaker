// Unit tests for the pure detection core (constitution V): synthetic
// (timestamp, dx, dy) sequences with no libinput, no udev, no files.
// v3 semantics (research R-04 v4, KDE Plasma / PowerToys): shake = the
// pointer trail inside `duration` is more than `sensitivity` times longer
// than the diagonal of its bounding box (≥ 100 units). Includes the
// user-reported Surface trace (missed by v1) and the ordinary-movement
// shapes that false-triggered the v2 swing counter.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shaker/detection.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace {

using shaker::DetectionCore;
using shaker::DetectionParams;
using shaker::Sample;
using shaker::DetectionResult;
using shaker::DetectionState;

constexpr int64_t kUsecPerSec = 1'000'000;

DetectionParams params_for(double sensitivity_units,
                           double duration_sec,
                           double cooldown_sec)
{
    return DetectionParams{
        .sensitivity = sensitivity_units,
        .duration_usec = static_cast<int64_t>(duration_sec * static_cast<double>(kUsecPerSec)),
        .cooldown_usec = static_cast<int64_t>(cooldown_sec * static_cast<double>(kUsecPerSec)),
    };
}

// Feed a straight stroke of `steps` samples, 2 ms apart; returns whether
// anything triggered.
[[nodiscard]] bool feed_stroke(DetectionCore& core, int64_t& t_usec, int steps,
                               double dx, double dy)
{
    bool fired = false;
    for (int i = 0; i < steps; ++i) {
        t_usec += 2'000;
        fired = core.feed(Sample{t_usec, dx, dy}).triggered || fired;
    }
    return fired;
}

// Feed `count` oscillation legs of `amplitude_units` pointer units each,
// `leg_sec` per leg, starting at t_start_sec, alternating in sign per leg
// (a physical shake). Returns the trigger timestamps (µs).
[[nodiscard]] std::vector<int64_t> feed_shake(DetectionCore& core,
                                              double t_start_sec, int legs,
                                              double amplitude_units,
                                              double leg_sec)
{
    int64_t t_usec = static_cast<int64_t>(t_start_sec * static_cast<double>(kUsecPerSec));
    std::vector<int64_t> triggers;
    for (int i = 0; i < legs; ++i) {
        const double dir = (i % 2 == 0) ? +1.0 : -1.0;
        const double per_sample =
            dir * amplitude_units / 10.0; // 10 samples per leg (~1 kHz)
        for (int s = 0; s < 10; ++s) {
            const int64_t next =
                t_usec + static_cast<int64_t>(leg_sec * static_cast<double>(kUsecPerSec) / 10.0);
            const DetectionResult r = core.feed(Sample{next, per_sample, 0.0});
            if (r.triggered) {
                triggers.push_back(r.at_usec);
            }
            t_usec = next;
        }
    }
    return triggers;
}

} // namespace

TEST_CASE("US1-1: jitter inside a tiny area never triggers")
{
    DetectionCore core(params_for(4.0, 1.0, 2.0));
    // ±30-unit wiggles for over a second: the factor is huge, but the
    // trail never leaves a box smaller than the 100-unit minimum extent.
    CHECK(feed_shake(core, 1, 60, 30.0, 0.02).empty());
    CHECK(core.state() == DetectionState::Idle);
}

TEST_CASE("US1-2: sustained oscillation triggers; cooldown then gates")
{
    DetectionCore core(params_for(4.0, 1.0, 2.0));

    // Vigorous shaking (±200 units, 40 ms per leg): ~5 legs push the
    // path past 4× the extent; then the 2 s cooldown gates the rest.
    auto first = feed_shake(core, 10, 25, 200.0, 0.04);
    REQUIRE(first.size() == 1);
    CHECK(first[0] - 10 * kUsecPerSec < 400'000);

    // Keep shaking before the cooldown expires: nothing new (G-1/G-3).
    CHECK(feed_shake(core, 11, 20, 200.0, 0.04).empty());

    // After the cooldown elapses, an oscillation triggers again.
    CHECK(feed_shake(core, 12.6, 25, 200.0, 0.04).size() == 1);
}

TEST_CASE("G-1 / FR-009: triggers are cooldown-spaced, never denser")
{
    DetectionCore core(params_for(4.0, 1.0, 5.0));
    auto times = feed_shake(core, 0, 200, 200.0, 0.04);
    REQUIRE(times.size() >= 2);
    for (size_t i = 1; i < times.size(); ++i) {
        CHECK(times[i] - times[i - 1] >= 4'990'000);
    }
}

TEST_CASE("US2-2: cooldown timeline — inside suppressed, after allowed")
{
    DetectionCore core(params_for(4.0, 1.0, 3.0));

    CHECK(feed_shake(core, 0, 20, 200.0, 0.04).size() == 1);
    CHECK(feed_shake(core, 1.05, 20, 200.0, 0.04).empty());
    CHECK(feed_shake(core, 3.55, 20, 200.0, 0.04).size() == 1);
    CHECK(feed_shake(core, 4.5, 20, 200.0, 0.04).empty());
}

TEST_CASE("cooldown = 0.0 disables suppression")
{
    DetectionCore core(params_for(4.0, 1.0, 0.0));
    // Each trigger consumes the trail, so the next needs a fresh ~5 legs.
    auto times = feed_shake(core, 0, 60, 200.0, 0.04);
    REQUIRE(times.size() >= 2);
    for (size_t i = 1; i < times.size(); ++i) {
        CHECK(times[i] - times[i - 1] >= 120'000);
    }
}

TEST_CASE("US2-1: same motion, sensitivity decides the outcome")
{
    // 25 legs of 40 ms = 1 s window holds at most ~25 legs → factor ≤ ~25.
    DetectionCore normal(params_for(4.0, 1.0, 2.0));
    CHECK(feed_shake(normal, 0, 25, 200.0, 0.04).size() == 1);

    DetectionCore strictest(params_for(40.0, 1.0, 2.0));
    CHECK(feed_shake(strictest, 0, 25, 200.0, 0.04).empty());
}

TEST_CASE("US2-3 / G-2: oscillation slower than the duration window never triggers")
{
    // 0.4 s legs: a 1 s window holds ~2.5 legs → factor ≤ ~2.5.
    DetectionCore core(params_for(4.0, 1.0, 2.0));
    CHECK(feed_shake(core, 0, 20, 300.0, 0.4).empty());
}

TEST_CASE("slow big-amplitude sweep never triggers")
{
    // User-validated complaint (v2): slow side-to-side movement with big
    // amplitude must stay silent.
    DetectionCore core(params_for(4.0, 1.0, 2.0));
    CHECK(feed_shake(core, 0, 16, 1500.0, 0.5).empty());
}

TEST_CASE("US2-4: straight fast movement (no reversal) never triggers")
{
    DetectionCore core(params_for(4.0, 1.0, 2.0));
    int64_t t_usec = 0;
    CHECK_FALSE(feed_stroke(core, t_usec, 1000, 30.0, 0.0));
    CHECK(core.state() == DetectionState::Idle);
}

TEST_CASE("false positive: overshoot-and-correct pointing never triggers")
{
    // Aim at a target, overshoot, correct, overshoot slightly, settle —
    // the v2 counter saw 3 swings here.
    DetectionCore core(params_for(4.0, 1.0, 2.0));
    int64_t t_usec = 0;
    bool fired = feed_stroke(core, t_usec, 40, 20.0, 3.0); // 800 right
    fired = feed_stroke(core, t_usec, 15, -6.0, 0.0) || fired; // back 90
    fired = feed_stroke(core, t_usec, 10, 5.0, 0.0) || fired;  // fwd 50
    fired = feed_stroke(core, t_usec, 10, -4.0, 0.0) || fired; // back 40
    CHECK_FALSE(fired);
}

TEST_CASE("false positive: zig-zag while travelling never triggers")
{
    // Fast vertical wiggle while moving right (scribbling, wavy travel):
    // per-axis swing counting fired on the Y axis; the trail's extent
    // grows with the travel, so the factor stays ≈ 1.4.
    DetectionCore core(params_for(4.0, 1.0, 2.0));
    int64_t t_usec = 0;
    bool fired = false;
    for (int leg = 0; leg < 30; ++leg) {
        const double dy = (leg % 2 == 0) ? 10.0 : -10.0;
        fired = feed_stroke(core, t_usec, 15, 10.0, dy) || fired;
    }
    CHECK_FALSE(fired);
}

TEST_CASE("false positive: one fast circle never triggers")
{
    // A circle's perimeter is ~2.2× its bounding diagonal.
    DetectionCore core(params_for(4.0, 1.0, 2.0));
    int64_t t_usec = 0;
    bool fired = false;
    constexpr int kSteps = 200;
    constexpr double kRadius = 200.0;
    constexpr double kPi = 3.14159265358979323846;
    for (int i = 0; i < kSteps; ++i) {
        const double a = 2.0 * kPi * i / kSteps;
        const double step = 2.0 * kPi * kRadius / kSteps;
        fired = feed_stroke(core, t_usec, 1, -std::sin(a) * step,
                            std::cos(a) * step)
                || fired;
    }
    CHECK_FALSE(fired);
}

TEST_CASE("reset() (button held / drag) discards the trail")
{
    DetectionCore core(params_for(4.0, 1.0, 2.0));
    // Four legs: just short of a shake.
    CHECK(feed_shake(core, 0, 4, 200.0, 0.04).empty());
    core.reset();
    CHECK(core.segments() == 0);
    // Two more legs would have completed it; after the reset they don't.
    CHECK(feed_shake(core, 0.17, 2, 200.0, 0.04).empty());
}

TEST_CASE("non-monotonic timestamps are dropped without corrupting state")
{
    DetectionCore core(params_for(4.0, 1.0, 2.0));

    auto before = feed_shake(core, 0, 20, 200.0, 0.04);
    REQUIRE(before.size() == 1);

    const DetectionResult r = core.feed(Sample{0, 999.0, 0.0});
    CHECK(r.triggered == false);

    // Still inside the 2 s cooldown.
    CHECK(feed_shake(core, 0.9, 20, 200.0, 0.04).empty());
    // After the cooldown: fresh trigger, once.
    CHECK(feed_shake(core, 2.6, 20, 200.0, 0.04).size() == 1);
}

TEST_CASE("duplicate timestamp sample is not fatal")
{
    DetectionCore core(params_for(4.0, 1.0, 0.0));
    const Sample same_time{1'000'000, 50.0, 0.0};
    (void)core.feed(same_time);
    (void)core.feed(same_time); // dt would be 0 — must not divide by zero
    CHECK(feed_shake(core, 1.01, 20, 200.0, 0.04).size() >= 1);
}

TEST_CASE("1 kHz sign jitter never overflows the fixed trail")
{
    // Every sample reverses: the ring (128 vertices) wraps many times.
    DetectionCore core(params_for(4.0, 1.0, 2.0));
    int64_t t_usec = 0;
    for (int i = 0; i < 5000; ++i) {
        t_usec += 1'000;
        (void)core.feed(Sample{t_usec, (i % 2 == 0) ? 5.0 : -5.0, 0.0});
    }
    CHECK(core.segments() <= 128);
}

TEST_CASE("trigger result carries timestamp, peak, and single-shot semantics")
{
    DetectionCore core(params_for(4.0, 1.0, 2.0));
    auto times = feed_shake(core, 0.05, 20, 200.0, 0.04);
    REQUIRE(times.size() == 1);
    CHECK(times[0] > 50'000);
    CHECK(core.state() == DetectionState::Cooldown);
}

TEST_CASE("user-reported Surface trace now triggers (v2 regression)")
{
    // The trace that missed v1 entirely: doors EMA-smoothed peak ~3.4k
    // units/s on a Surface — reproduced here as the equivalent pointer
    // displacement sequence (velocity samples reversed per swing, dt 7 ms).
    // Recorded from the bug report: "shaking as hard as I want".
    DetectionCore core(params_for(4.0, 1.0, 2.0));

    // Reconstruct: speeds reported, ~0.007 s per sample; sign alternates on
    // the local extremes of the reported sequence (offsets between peaks).
    constexpr double report[] = {
        2432, 2438, 2061, 1597, 1198, 868, 627, 659, 1010, 2264, 3272, 3418,
        3094, 2499, 1886, 1392, 1034, 930, 1170, 1953, 2805, 3035, 2867, 2344,
        1782,
    };
    constexpr int kSamples = static_cast<int>(sizeof(report) / sizeof(report[0]));
    // Repeat the trace ~8x to give the detector time to trigger. The
    // velocity magnitude's local minimum (dip) under way is where the
    // device reverses direction: flip the sign there.
    int64_t t_usec = 0;
    int fired = 0;
    double dir = +1.0;
    const int prev_baseline = kSamples - 1;
    for (int cycle = 0; cycle < 8; ++cycle) {
        for (int i = 0; i < kSamples; ++i) {
            t_usec += 7'000;
            // displacement dt * velocity (a swing covers ~40-90 pointer
            // units per leg of this trace)
            const double dx = (dir * static_cast<double>(report[i])) * 0.007;
            const DetectionResult r = core.feed(Sample{t_usec, dx, 0.0});
            if (r.triggered) {
                ++fired;
            }
            const int next = (i + 1 < kSamples) ? i + 1 : 0;
            const int prev = (i == 0) ? prev_baseline : i - 1;
            if (report[i] <= report[prev] && report[i] <= report[next]) {
                dir = -dir;
            }
        }
    }
    CHECK(fired >= 1);
}
