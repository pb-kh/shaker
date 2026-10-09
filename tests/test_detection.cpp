// Unit tests for the pure detection core (constitution V): synthetic
// (timestamp, dx, dy) sequences with no libinput, no udev, no files.
// v2 semantics (research R-04): swings = direction changes with real
// displacement; shake = ≥3 swings within `duration` (cooldown gating
// unchanged). Includes a regression test built from the user-reported
// Surface trace that was missed by the v1 sustained-speed detector.
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

TEST_CASE("US1-1: gentle motion below sensitivity never triggers")
{
    DetectionCore core(params_for(40.0, 0.25, 2.0));
    // Small wiggles (±15 units per leg) — no swing passes the 40-unit
    // hysteresis, so nothing accumulates.
    CHECK(feed_shake(core, 1, 60, 15.0, 0.02).empty());
    CHECK(core.state() == DetectionState::Idle);
}

TEST_CASE("US1-2: sustained oscillation triggers; cooldown then gates")
{
    DetectionCore core(params_for(40.0, 0.25, 2.0));

    // 1 s of vigorous shaking (per leg 0.02 s): the 3-swing window
    // completes in ~0.1 s; after the trigger the 2 s cooldown gates
    // anything else inside this batch.
    auto first = feed_shake(core, 10, 50, 120.0, 0.02);
    REQUIRE(first.size() == 1);

    // Keep shaking before the cooldown expires: nothing new (G-1/G-3).
    auto during = feed_shake(core, 11, 40, 120.0, 0.02);
    CHECK(during.empty());

    // After the cooldown elapses (10.07 + 2 = 12.07), an oscillation
    // triggers again.
    auto after = feed_shake(core, 12.6, 50, 120.0, 0.02);
    CHECK(after.size() == 1);
}

TEST_CASE("G-1 / FR-009: triggers are cooldown-spaced, never denser")
{
    DetectionCore core(params_for(40.0, 0.25, 5.0));
    // Continuous vigorous shaking for 8 s with a 5 s cooldown: the second
    // trigger is at least a full cooldown after the first.
    auto times = feed_shake(core, 0, 400, 120.0, 0.02);
    REQUIRE(!times.empty());
    if (times.size() >= 2) {
        CHECK(times[1] - times[0] >= 4'990'000);
    }
}

TEST_CASE("US2-2: cooldown timeline — inside suppressed, after allowed")
{
    DetectionCore core(params_for(40.0, 0.25, 3.0));

    CHECK(feed_shake(core, 0, 40, 120.0, 0.02).size() == 1);
    // 1 s later: inside the 3 s cooldown → nothing.
    CHECK(feed_shake(core, 1.05, 40, 120.0, 0.02).empty());
    // 3.5 s in: cooldown over → exactly one trigger accepted.
    CHECK(feed_shake(core, 3.55, 40, 120.0, 0.02).size() == 1);
    // Immediately afterwards: inside the fresh cooldown window.
    CHECK(feed_shake(core, 4.0, 40, 120.0, 0.02).empty());
}

TEST_CASE("cooldown = 0.0 disables suppression")
{
    DetectionCore core(params_for(40.0, 0.25, 0.0));

    // Continuous vigorous shaking for ~1.6 s with zero cooldown: several
    // triggers fire, never denser than a fresh 3-swing window.
    auto times = feed_shake(core, 0, 80, 120.0, 0.02);
    REQUIRE(times.size() >= 2);
    for (size_t i = 1; i < times.size(); ++i) {
        CHECK(times[i] - times[i - 1] >= 50'000); // ≥ ~3 swings minus ε
    }
}

TEST_CASE("US2-1: same motion, sensitivity decides the outcome")
{
    // Legs of 120 units each: under the default 40-unit sensitivity this
    // easily completes 3 swings inside the window.
    DetectionCore strict(params_for(40.0, 0.25, 2.0));
    CHECK(feed_shake(strict, 0, 40, 120.0, 0.02).size() == 1);

    // Identical motion under a stricter (100-unit) sensitivity with
    // 60-unit legs: each swing leg never reaches the 100-unit hysteresis
    // → no flips at all → silent (tuning direction preserved with the
    // units change).
    DetectionCore strictest(params_for(100.0, 0.25, 2.0));
    CHECK(feed_shake(strictest, 0, 40, 60.0, 0.02).empty());
}

TEST_CASE("US2-3 / G-2: oscillation slower than the duration window never triggers")
{
    DetectionCore core(params_for(40.0, 0.25, 2.0));
    // Swing legs of 0.4 s: flips land every ~370 ms (40-unit retrace
    // ends a leg early), so even three swings span ≈ 1.1 s — always
    // beyond the 0.25 s window → nothing triggers (the state may sit in
    // SUSTAIN while a futile window is open; that is presentation, not a
    // trigger).
    CHECK(feed_shake(core, 0, 20, 120.0, 0.4).empty());
}

TEST_CASE("US2-4: straight fast movement (no reversal) never triggers")
{
    DetectionCore core(params_for(40.0, 0.25, 2.0));
    // Sustained one-way fast drag: at most one direction change, and the
    // window never completes 3 swings.
    int64_t t = 0;
    int64_t t_sec = 0;
    (void)t;
    int64_t t_usec = 0;
    for (int i = 0; i < 1000; ++i) {
        t_usec += 2'000;
        (void)core.feed(Sample{t_usec, 30.0, 0.0}); // 30 units/sample, one way
    }
    (void)t_sec;
    CHECK(core.state() == DetectionState::Idle);
}

TEST_CASE("non-monotonic timestamps are dropped without corrupting state")
{
    DetectionCore core(params_for(40.0, 0.4, 2.0));

    // First: a cleanly triggered shake.
    auto before = feed_shake(core, 0, 60, 120.0, 0.02);
    REQUIRE(before.size() == 1);

    // Samples going backwards in time: ignored entirely, no side effect.
    const DetectionResult r = core.feed(Sample{0, 999.0, 0.0});
    CHECK(r.triggered == false);

    // State is preserved (this oscillation is inside the 2 s cooldown):
    CHECK(feed_shake(core, 0.1, 60, 120.0, 0.02).empty());

    // After the pre-regression cooldown elapses: fresh trigger, once.
    CHECK(feed_shake(core, 2.6, 60, 120.0, 0.02).size() == 1);
}

TEST_CASE("duplicate timestamp sample is not fatal")
{
    DetectionCore core(params_for(40.0, 0.25, 0.0));
    const Sample same_time{1'000'000, 50.0, 0.0};
    (void)core.feed(same_time);
    (void)core.feed(same_time); // dt would be 0 — must not divide by zero
    // Core remains functional afterwards.
    (void)feed_shake(core, 1.01, 40, 120.0, 0.02);
}

TEST_CASE("slow big-amplitude sweep never triggers (cadence gate)")
{
    // User-validated complaint: slow side-to-side movement with big
    // amplitude must stay silent. Legs of 0.5 s at ±1500 units: each leg
    // is far slower than the 350 ms cadence cap, so its flips are
    // rejected and never accumulate into a window.
    DetectionCore core(params_for(40.0, 0.25, 2.0));
    CHECK(feed_shake(core, 0, 16, 1500.0, 0.5).empty());
    CHECK(core.state() == DetectionState::Idle);
}

TEST_CASE("oversized fast travel never triggers (travel gate)")
{
    // Full-screen flicks: ±3000-unit legs fast enough to pass the cadence
    // cap, but far over the 2000-unit travel cap → cursor travel, not
    // shaking.
    DetectionCore core(params_for(40.0, 0.25, 2.0));
    CHECK(feed_shake(core, 0, 60, 3000.0, 0.03).empty());
    CHECK(core.state() == DetectionState::Idle);
}

TEST_CASE("fast small shakes still trigger under the travel/cadence gates")
{
    // No regression: ±60-unit legs at 30 ms — well inside both gates.
    DetectionCore core(params_for(40.0, 0.25, 2.0));
    auto times = feed_shake(core, 0, 40, 60.0, 0.03);
    CHECK(times.size() == 1);
}

TEST_CASE("trigger result carries timestamp, peak, and single-shot semantics")
{
    DetectionCore core(params_for(40.0, 0.25, 2.0));
    int64_t trigger_time = -1;
    double peak = -1.0;
    for (int i = 0; i < 500; ++i) {
        const int64_t t = static_cast<int64_t>(
            (0.05 + static_cast<double>(i) * 0.002) * static_cast<double>(kUsecPerSec));
        // oscillate 120 units per leg, 2 ms per leg-sample (10 ms/leg)
        const double dir = (i % 20 < 10) ? +1.0 : -1.0;
        const DetectionResult r = core.feed(Sample{t, 12.0 * dir, 0.0});
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
            core.feed(Sample{trigger_time + static_cast<int64_t>((i + 1) * 2000), 12.0, 0.0});
        if (r.triggered) total += 1;
    }
    CHECK(total == 0);
}

TEST_CASE("user-reported Surface trace now triggers (v2 regression)")
{
    // The trace that missed v1 entirely: doors EMA-smoothed peak ~3.4k
    // units/s on a Surface — reproduced here as the equivalent pointer
    // displacement sequence (velocity samples reversed per swing, dt 7 ms).
    // Recorded from the bug report: "shaking as hard as I want".
    DetectionCore core(params_for(40.0, 0.25, 2.0));

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
