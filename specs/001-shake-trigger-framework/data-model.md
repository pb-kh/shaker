# Data Model: Shake-Triggered Command Daemon

**Feature**: `001-shake-trigger-framework` | **Date**: 2026-10-09
**Source**: [spec.md](spec.md) Key Entities + FR-001…FR-018; implementation
carriers defined in [plan.md](plan.md).

There is no persistent storage: all entities live in memory for the life of
the process. The entities below are the pure, hardware-agnostic data used by
the detection core and the config loader (constitution V: the core consumes
plain data only — no libinput/udev types appear here).

---

## 1. Sample

A single device-motion observation consumed by the detection core. One sample
is derived from each libinput pointer-motion event; the daemon owns the
conversion from libinput to this plain type.

| Field | Type (domain) | Meaning | Validation |
|-------|---------------|---------|------------|
| `t_usec` | `int64` micro-seconds, CLOCK_MONOTONIC, strictly non-decreasing across samples | Event timestamp (device event time, not wall clock) | must not go backwards; violates → sample dropped + stderr diagnostic |
| `dx` | `double` libinput pointer delta units | Horizontal motion delta since previous motion event from the same device (as reported) | none (any real number) |
| `dy` | `double` libinput pointer delta units | Vertical motion delta | none |

The core derives everything else: it integrates displacement per axis and
counts direction changes (swings) on each axis with hysteresis; the
instantaneous swing speed (units/s) at the triggering sample is reported in
the result for diagnostics.

## 2. DetectionParams

The tuned/validated parameter set extracted from the Configuration entity
after parsing, converted to the core's microsecond event-time domain. This is
the "parameterizable" part required by constitution V.

| Field | Type | Source key | Unit | Default | Valid range | Invalid handling |
|-------|------|-----------|------|---------|-------------|------------------|
| `sensitivity` | `double` | `sensitivity` | pointer units per swing leg | `40.0` | > 0.0 | error + exit (FR-010) |
| `duration_usec` | `int64` | `duration` | seconds (config) | 0.25 s | > 0.0 | error + exit (FR-010) |
| `cooldown_usec` | `int64` | `cooldown` | seconds (config) | 2.0 s | ≥ 0.0 (0 = no suppression) | error + exit (FR-010) |

Relationships: ctor parameter of `DetectionCore`; immutable for process life
(config is read once).

## 3. DetectionCore (state machine)

The pure detection transformer (v2, research R-04): swings = direction
changes with real displacement. Owns all shake-decision state; contains
**no** I/O.

| Field | Type | Meaning |
|-------|------|---------|
| `params` | `DetectionParams` | immutable |
| `state` | enum `Idle`, `Sustaining`, `Cooldown` | current machine phase |
| `position[2]` | `double` per axis | running displacement along X and Y (integrates dx/dy) |
| `extreme[2]`, `phase[2]` | per axis | hysteresis run: current extreme of position and its phase (+rising/−falling/none) |
| `swings` | `int` | counted direction changes inside the open window |
| `first_flip_t_usec`, `window_open` | window anchor | when the current swing run began (event time) |
| `cooldown_until_usec` | `int64` | end of current cooldown (valid in Cooldown) |

Behavior (per sample; both axes run independently, either counts):

```text
                sample s (dx, dy, t)
                ───────────────────►
Position      : position[axis] += d[axis]
Phase none    : |position| ≥ sensitivity → open phase (+rising if position > 0, else −falling),
                                       extreme = position, leg starts here
Rising        : position > extreme       → extreme = position   (still moving out)
              : extreme − position ≥ sensitivity
                                         → candidate flip (see gates), swing++ ↺
Falling       : symmetric

Gates (internal constants, R-04 v3 — user-validated: slow big-amplitude
sweeps must stay silent): a candidate flip is REJECTED — poisoning the
window — when its leg is slower or travels farther than a shake can:
  leg duration (since phase open) > 350 ms      → slow sweep, reject
  leg travel |extreme − leg_open_pos| > 2000 u  → cursor travel, reject
A rejected leg keeps the hunt alive (opposite phase re-anchored) with
swings reset to 0.

Window        : opens at the first counted swing (first_flip = t)
SUSTAIN       : reported from the 2nd counted swing on (a lone candidate
                swing keeps state Idle)
              : (t − first_flip) > duration    → window abandoned (state → Idle, hunt restarted)
TRIGGER       : swings ≥ 3 (internal constant) within the window
                                           → emit Trigger{at = t, speed = instant swing speed}
                                             Cooldown{until = t + cooldown}, full reset
Long idle gap (> 0.5 s) → full reset
COOLDOWN      : t < until → silent (never retrigger, FR-008)
              : t ≥ until → Idle + full reset (evaluate fresh from next sample)
```

State summary: `Idle` = no open window; `Sustaining` = open swing window
with ≥ 2 swings; `Cooldown` = suppressing window.

Guarantees (each maps to a test requirement):
- G-1: never emits two Triggers closer than `cooldown` apart (FR-008/FR-009,
  SC-006).
- G-2: swings that cannot complete inside `duration` never emit (US2-3).
- G-3: samples at any motion during Cooldown never emit until it elapses
  (continuous-shaking edge case: no queued/executing storms).
- G-4: state has fixed size; no allocation per sample (SC-005).
- G-5: behavior depends only on (params, sample history) — deterministic,
  replayable.
- G-6: motion without direction changes (straight fast drags) never emits.

## 4. DetectionResult

The core's output event.

| Field | Type | Meaning |
|-------|------|---------|
| `triggered` | `bool` | true exactly when G-1..G-6 allowed an emission on this sample |
| `at_usec` | `int64` | event timestamp of the triggering sample (used for trigger logging) |
| `peak_speed` | `double` | instantaneous swing speed (units/s) at the triggering sample (diagnostics) |

Relationships: emitted by the core; the host layer prints the stdout trigger
line (when `triggered`) and then spawns the command.

## 5. Configuration (parsed)

The in-memory image of the TOML file (contracts/config-schema.md). Read once
at startup; converted to `DetectionParams` + `command` afterwards.

| Field | Type | TOML key | Constraint | Default on omission |
|-------|------|----------|------------|---------------------|
| `sensitivity` | `double` | `sensitivity` | > 0 | 8000.0 |
| `duration` | `double` | `duration` | > 0 | 0.25 |
| `cooldown` | `double` | `cooldown` | ≥ 0 | 2.0 |
| `command` | `string` | `command` | non-empty; free text executed by the shell | **none — missing → error + exit** |

Key relationships / invariants:
- Exactly one config file per invocation; path resolution in R-03.
- The whole file must parse as TOML; unknown keys are **ignored with a
  stderr warning** (forward compatibility; doesn't fail US3 scenario 2,
  which covers malformed structure/values).
- Failure ladder (FR-010 / FR-011 / command-missing):
  1. missing file → `cannot read <resolved path>: <reason>` + exit 1
  2. syntax error → `config parse error <path>: <line> <msg>` + exit 1
  3. missing/empty `command` → `config <path>: "command" is required` + exit 1
  4. invalid range → `config <path>: "sensitivity" must be > 0 (got -3)` +
     exit 1
- All four keys may appear with any relative order; no other top-level
  structure (flat file, no `[tables]` semantics).

## 6. CommandInvocation (runtime-only)

Not a data-class entity — the single outcome of a Trigger handled by
`exec_command.cpp`: a detached `/bin/sh -c <command>` child (see research
R-06). Spawn failure (fork returns < 0) degrades to a stderr diagnostic; the
daemon never exits because of it (US1 acceptance scenario 3).

## Sequencing overview (runtime)

```text
config path ──► parse/validate ──► DetectionParams + command
                                              │
libinput pointer motion ──► Sample ──► DetectionCore ──► (Trigger) ──► spawn
```
