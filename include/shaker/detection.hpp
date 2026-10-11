// Pure, hardware-agnostic shake detection core (constitution V).
//
// v3 semantics (research R-04 v4): the KDE Plasma / PowerToys shake
// detector. The core integrates pointer deltas into a trail and keeps the
// trail's vertices from the last `duration` seconds, merging consecutive
// motion in the same direction into one segment (a new vertex appears
// only on a direction change). A shake triggers when the trail is much
// longer than the diagonal of the box that contains it:
//
//     path_length / bbox_diagonal > sensitivity   (KWin default 4)
//
// Ordinary pointing — however fast, curved, or corrective — travels about
// as far as its extent (ratio ≈ 1–2). Only repeated back-and-forth over
// the same small area piles up path without growing the box. The ratio is
// scale-free, so device speed, acceleration and amplitude do not matter;
// an internal minimum extent (100 pointer units, as in KWin) keeps
// hand-tremor jitter out. Cooldown gating and output shape are unchanged
// from data-model.md §3/§4.
//
// The core contains no I/O, no clocks, and no allocations (fixed-size
// trail ring): behavior is a deterministic function of (DetectionParams,
// sample history), so it is fully replayable in unit tests without device
// access. Timestamps are monotonic microseconds (device event time).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace shaker {

struct DetectionParams {
    double sensitivity = 4.0; // shake factor: path length / trail diagonal
    int64_t duration_usec = 1'000'000; // trail history window
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
    double peak_speed = 0.0; // pointer speed (units/s) at the triggering sample
};

class DetectionCore {
public:
    explicit DetectionCore(DetectionParams params) : params_(params) {}

    // Process one sample; returns the detection outcome for this sample.
    // Samples with regressing or duplicate timestamps are dropped
    // (returned result is a no-trigger) without corrupting state.
    DetectionResult feed(const Sample& sample);

    // Forget the trail (cooldown is kept). Hosts call it while a pointer
    // button is held: dragging (text selection, window moves) is never a
    // shake (KWin does the same).
    void reset();

    [[nodiscard]] DetectionState state() const { return state_; }

    // Trail vertices currently inside the window — direction changes + 1
    // (`--debug` trace).
    [[nodiscard]] std::size_t segments() const { return count_; }

    // Shake factor (path / diagonal) of the current trail; 0 while the
    // trail is smaller than the minimum extent (`--debug` trace).
    [[nodiscard]] double shake_factor() const { return last_factor_; }

    // Most recent instantaneous pointer speed (units/s) — `--debug` trace.
    [[nodiscard]] double last_speed() const { return last_speed_; }

    // Time span of the trail inside the window (`--debug` trace).
    [[nodiscard]] int64_t window_elapsed_usec() const;

private:
    struct Vertex {
        double x = 0.0;
        double y = 0.0;
        int64_t t_usec = 0;
    };
    // Fixed ring capacity. A real shake needs ~5–10 direction changes per
    // window; when jitter overflows it, the oldest vertices are dropped,
    // which only shortens the effective window.
    static constexpr std::size_t kCapacity = 128;

    [[nodiscard]] Vertex& at(std::size_t i) { return trail_[(head_ + i) % kCapacity]; }
    [[nodiscard]] const Vertex& at(std::size_t i) const
    {
        return trail_[(head_ + i) % kCapacity];
    }
    void push(const Vertex& v);
    void pop_front();

    DetectionParams params_;

    int64_t last_t_usec_ = -1; // -1 = no anchor yet
    double x_ = 0.0;           // integrated pointer position
    double y_ = 0.0;

    std::array<Vertex, kCapacity> trail_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;

    double last_factor_ = 0.0;
    double last_speed_ = 0.0;

    DetectionState state_ = DetectionState::Idle;
    int64_t cooldown_until_usec_ = 0;
};

} // namespace shaker
