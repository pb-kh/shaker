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
| `speed` | `double` derived, device units / second | speed estimate fed to the state machine: instantaneous `hypot(dx, dy) / dt` smoothed by a fixed internal EMA (τ = 20 ms, research R-04 refinement) so direction-reversal dips don't shatter the sustained window; `dt` = δt between this and previous sample, guarded to ≥ 1 µs | derived, never persisted |

Relationships: consumed by `DetectionCore::feed()`.

## 2. DetectionParams

The tuned/validated parameter set extracted from the Configuration entity
after parsing, converted to the core's microsecond event-time domain. This is
the "parameterizable" part required by constitution V.

| Field | Type | Source key | Unit | Default | Valid range | Invalid handling |
|-------|------|-----------|------|---------|-------------|------------------|
| `sensitivity` | `double` | `sensitivity` | device units / second | `8000.0` (calibration starting point, documented) | > 0.0 | error + exit (FR-010) |
| `duration_usec` | `int64` | `duration` | seconds (config) | 0.25 s | > 0.0 | error + exit (FR-010) |
| `cooldown_usec` | `int64` | `cooldown` | seconds (config) | 2.0 s | ≥ 0.0 (0 = no suppression) | error + exit (FR-010) |

Relationships: ctor parameter of `DetectionCore`; immutable for process life
(config is read once).

## 3. DetectionCore (state machine)

The pure detection transformer. Owns all shake-decision state; contains **no**
I/O.

| Field | Type | Meaning |
|-------|------|---------|
| `params` | `DetectionParams` | immutable |
| `state` | enum `Idle`, `Sustaining`, `Cooldown` | current machine phase |
| `window_start_usec` | `int64` | timestamp at which the current above-threshold stretch began (valid in Sustaining) |
| `cooldown_until_usec` | `int64` | end of current cooldown (valid in Cooldown) |

State transitions (straight-line translation of FR-007, FR-008, FR-009):

```text
                sample s (speed, t)
                ───────────────────►
Idle          : speed ≥ sensitivity        → Sustaining{window_start = s.t}
Idle          : otherwise                  → Idle
Sustaining    : speed < sensitivity        → Idle          (window broken)
Sustaining    : (s.t − window_start) ≥ duration
                                           → emit Trigger{at = s.t}
                                             Cooldown{until = s.t + cooldown}
Sustaining    : otherwise                  → Sustaining
Cooldown      : s.t ≥ until                → Idle  (evaluate next sample normally)
Cooldown      : s.t < until                → Cooldown   (FR-008: never retrigger)
```

Guarantees (each maps to a test requirement):
- G-1: never emits two Triggers closer than `cooldown` apart (FR-008/FR-009,
  SC-006).
- G-2: bypass-free — a stretch shorter than `duration` cannot emit (US2
  scenario 3).
- G-3: samples at any speed during Cooldown never emit until it elapses
  (continuous-shaking edge case: no queued/executing storms).
- G-4: state has fixed size; no allocation per sample (SC-005).
- G-5: behavior depends only on (params, sample history) — deterministic,
  replayable.

## 4. DetectionResult

The core's output event.

| Field | Type | Meaning |
|-------|------|---------|
| `triggered` | `bool` | true exactly when G-1..G-3 allowed an emission on this sample |
| `at_usec` | `int64` | event timestamp of the triggering sample (used for trigger logging) |
| `peak_speed` | `double` | max speed observed inside the closing window (calibration diagnostics only) |

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
