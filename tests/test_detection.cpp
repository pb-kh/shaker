// Unit tests for the pure detection core (constitution V): synthetic
// (timestamp, dx, dy) sequences with no libinput, no udev, no files.
// Source of truth: data-model.md §3 transition table + guarantees G-1…G-5,
// with the R-04 refinement (EMA smoothing bridges reversal dips).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shaker/detection.hpp>

#include <string>
#include <vector>

namespace {

using shaker::DetectionCore;
using shaker::DetectionParams;
using shaker::Sample;
using shaker::DetectionResult;
using shaker::DetectionState;

constexpr int64_t kUsecPerSec = 1'000'000;

DetectionParams params_for(double sensitivity,
                           double duration_sec,
                           double cooldown_sec)
{
    return DetectionParams{
        .sensitivity = sensitivity,
        .duration_usec = static_cast<int64_t>(duration_sec * static_cast<double>(kUsecPerSec)),
        .cooldown_usec = static_cast<int64_t>(cooldown_sec * static_cast<double>(kUsecPerSec)),
    };
}

// Feed an alternating-direction movement (a shake flips direction) with a
// per-segment average speed, starting at t_start_sec, `count` samples every
// dt_sec. Returns the timestamps (µs) at which the core triggered.
[[nodiscard]] std::vector<int64_t> feed_shake(DetectionCore& core,
                                              double t_start_sec, int count,
                                              double speed, double dt_sec)
{
    const double step_usec = dt_sec * static_cast<double>(kUsecPerSec);
    int64_t t_usec = static_cast<int64_t>(t_start_sec * static_cast<double>(kUsecPerSec));
    std::vector<int64_t> triggers;
    for (int i = 0; i < count; ++i) {
        const double dx = (i % 2 == 0 ? 1.0 : -1.0) * speed * dt_sec;
        const int64_t next = t_usec + static_cast<int64_t>(step_usec);
        const DetectionResult r = core.feed(Sample{next, dx, 0.0});
        if (r.triggered) {
            triggers.push_back(r.at_usec);
        }
        t_usec = next;
    }
    return triggers;
}

} // namespace

TEST_CASE("US1-1: gentle motion below sensitivity never triggers")
{
    DetectionCore core(params_for(8000.0, 0.25, 2.0));
    // 2 s of gentle movement.
    CHECK(feed_shake(core, 1, 1000, 300.0, 0.002).empty());
    CHECK(core.state() == DetectionState::Idle);
}

TEST_CASE("US1-2: sustained shake above sensitivity triggers; cooldown then gate")
{
    DetectionCore core(params_for(8000.0, 0.25, 2.0));

    // One second of vigorous shaking.
    auto first = feed_shake(core, 10, 500, 40'000.0, 0.002);
    REQUIRE(first.size() == 1);

    // Keep shaking through the cooldown: no second trigger (G-1/G-3).
    auto during = feed_shake(core, 11, 500, 40'000.0, 0.002);
    CHECK(during.empty());

    // After the cooldown elapses, another sustained shake triggers once.
    auto after = feed_shake(core, 13.5, 500, 40'000.0, 0.002);
    CHECK(after.size() == 1);
}

TEST_CASE("US2-3 / G-2: strength without duration is not a shake")
{
    DetectionCore core(params_for(8000.0, 0.25, 2.0));

    // Vigorous but only 0.05 s of activity: nothing.
    CHECK(feed_shake(core, 0, 25, 60'000.0, 0.002).empty());

    // A below-threshold sample long after (idle gap) keeps it idle.
    CHECK(core.feed(Sample{1'000'000, 0.0, 0.0}).triggered == false);
    CHECK(core.state() == DetectionState::Idle);

    // Another short burst: still nothing.
    CHECK(feed_shake(core, 1.01, 25, 60'000.0, 0.002).empty());
}

TEST_CASE("G-1 / FR-009: triggers are cooldown-spaced, never denser")
{
    DetectionCore core(params_for(8000.0, 0.1, 5.0));

    // Continuous vigorous shaking for 6 s with a 5 s cooldown: the first
    // trigger lands early, the earliest possible second one is cooldown
    // away, and the third one is out of the timeline. No denser pattern
    // is acceptable.
    auto times = feed_shake(core, 0, 3000, 40'000.0, 0.002);
    REQUIRE(times.size() == 2);
    CHECK(times[1] - times[0] >= 4'990'000); // ≈ the 5 s cooldown minus ε
}

TEST_CASE("US2-2: cooldown timeline — inside suppressed, after allowed")
{
    DetectionCore core(params_for(8000.0, 0.1, 3.0));

    // Trigger inside the first 0.3 s.
    CHECK(feed_shake(core, 0, 150, 40'000.0, 0.002).size() == 1);
    // 1 s later: still inside a 3 s cooldown → nothing.
    CHECK(feed_shake(core, 1.05, 150, 40'000.0, 0.002).empty());
    // 3.5 s in: cooldown over → exactly one trigger accepted.
    CHECK(feed_shake(core, 3.55, 150, 40'000.0, 0.002).size() == 1);
    // Immediately afterwards: inside the fresh cooldown window.
    CHECK(feed_shake(core, 4.0, 150, 40'000.0, 0.002).empty());
}

TEST_CASE("cooldown = 0.0 disables suppression")
{
    DetectionCore core(params_for(8000.0, 0.1, 0.0));

    // Continuous vigorous shaking for 0.6 s with zero cooldown: several
    // triggers fire, one every `duration` (0.1 s), never denser.
    auto times = feed_shake(core, 0, 300, 40'000.0, 0.002);
    REQUIRE(times.size() >= 2);
    CHECK(times[1] - times[0] >= 95'000); // ≈ duration minus ε
}

TEST_CASE("US2-1: same motion, sensitivity decides the outcome")
{
    // Gentle alternating movement (~6000 units/s) under a strict threshold.
    DetectionCore strict(params_for(8000.0, 0.25, 2.0));
    CHECK(feed_shake(strict, 0, 400, 6000.0, 0.002).empty());

    // Identical motion under a permissive threshold triggers: the EMA
    // carries the alternating bursts above the lower threshold.
    DetectionCore permissive(params_for(2000.0, 0.25, 2.0));
    CHECK(feed_shake(permissive, 0, 400, 6000.0, 0.002).size() == 1);
}

TEST_CASE("non-monotonic timestamps are dropped without corrupting state")
{
    DetectionCore core(params_for(8000.0, 0.25, 2.0));

    // First: a cleanly triggered shake.
    auto before = feed_shake(core, 0, 200, 40'000.0, 0.002);
    REQUIRE(before.size() == 1);

    // Samples going backwards in time: ignored entirely, no side effect.
    const DetectionResult r = core.feed(Sample{0, 999'999.0, 0.0});
    CHECK(r.triggered == false);

    // State is preserved (this shake is inside the 2 s cooldown → silent):
    CHECK(feed_shake(core, 0.1, 200, 40'000.0, 0.002).empty());

    // After the cooldown from the pre-regression trigger elapses, a fresh
    // sustained shake triggers exactly once.
    CHECK(feed_shake(core, 2.6, 300, 40'000.0, 0.002).size() == 1);
}

TEST_CASE("trigger result carries timestamp, peak, and single-shot semantics")
{
    DetectionCore core(params_for(8000.0, 0.1, 2.0));
    int64_t trigger_time = -1;
    double peak = -1.0;
    for (int i = 0; i < 500; ++i) {
        const int64_t t = static_cast<int64_t>(
            (0.05 + static_cast<double>(i) * 0.002) * static_cast<double>(kUsecPerSec));
        const DetectionResult r = core.feed(Sample{t, 40.0, 0.0});
        if (r.triggered) {
            trigger_time = r.at_usec;
            peak = r.peak_speed;
            break;
        }
    }

    REQUIRE(trigger_time > 0);
    CHECK(peak > 0.0);
    // Exactly one trigger across the run (cooldown window is 2 s).
    int total = 0;
    for (int i = 0; i < 100; ++i) {
        const DetectionResult r =
            core.feed(Sample{trigger_time + static_cast<int64_t>((i + 1) * 2000), 40.0, 0.0});
        if (r.triggered) total += 1;
    }
    CHECK(total == 0);
}

TEST_CASE("trigger fires exactly when the sustained window crosses duration")
{
    DetectionCore core(params_for(8000.0, 0.1, 1.0));
    // Continuous vigorous movement from t=0; the first trigger may only
    // appear once elapsed ≥ duration (plus the small EMA warm-up), and
    // must not be a burst shorter than the duration crossing.
    auto times = feed_shake(core, 0, 300, 40'000.0, 0.002);
    REQUIRE(!times.empty());
    const int64_t since_start = times.front();
    CHECK(since_start >= 99'000);  // ≥ duration (0.1 s) minus ε
    CHECK(since_start <= 300'000); // well inside the 0.3 s timeline
}

TEST_CASE("duplicate timestamp sample is not fatal")
{
    DetectionCore core(params_for(8000.0, 0.1, 0.0));
    const Sample same_time{1'000'000, 50.0, 0.0};
    (void)core.feed(same_time);
    (void)core.feed(same_time); // dt would be 0 — must not divide by zero
    // Core remains functional afterwards.
    (void)feed_shake(core, 1.01, 150, 40'000.0, 0.002);
}
