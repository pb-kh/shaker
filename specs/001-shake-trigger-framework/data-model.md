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

The core derives everything else: it integrates displacement into a
pointer trail and compares the trail's length to its bounding-box
diagonal; the instantaneous pointer speed (units/s) at the triggering
sample is reported in the result for diagnostics.

## 2. DetectionParams

The tuned/validated parameter set extracted from the Configuration entity
after parsing, converted to the core's microsecond event-time domain. This is
the "parameterizable" part required by constitution V.

| Field | Type | Source key | Unit | Default | Valid range | Invalid handling |
|-------|------|-----------|------|---------|-------------|------------------|
| `sensitivity` | `double` | `sensitivity` | shake factor (path / trail diagonal) | `4.0` | > 0.0 | error + exit (FR-010) |
| `duration_usec` | `int64` | `duration` | seconds (config), trail window | 1.0 s | > 0.0 | error + exit (FR-010) |
| `cooldown_usec` | `int64` | `cooldown` | seconds (config) | 2.0 s | ≥ 0.0 (0 = no suppression) | error + exit (FR-010) |

Relationships: ctor parameter of `DetectionCore`; immutable for process life
(config is read once).

## 3. DetectionCore (state machine)

The pure detection transformer (v3, research R-04 v4 — KDE Plasma /
PowerToys shake detector). Owns all shake-decision state; contains **no**
I/O and no allocations.

| Field | Type | Meaning |
|-------|------|---------|
| `params` | `DetectionParams` | immutable |
| `state` | enum `Idle`, `Sustaining`, `Cooldown` | current machine phase |
| `x`, `y` | `double` | integrated pointer position (sum of dx/dy) |
| `trail` | fixed ring of 128 `{x, y, t}` vertices | trail vertices inside the window (oldest dropped on overflow) |
| `cooldown_until_usec` | `int64` | end of current cooldown (valid in Cooldown) |

Behavior (per sample):

```text
Position      : x += dx, y += dy
COOLDOWN      : t < until → silent (never retrigger, FR-008)
              : t ≥ until → Idle, trail = [current position]
Prune         : drop vertices with t − vertex.t ≥ duration
Merge         : ≥ 2 vertices and the new motion keeps the last segment's
                sign on both axes (±1 unit tolerance) → last vertex moves
                to the current position; done (no evaluation)
Push          : append current position as a new vertex (direction change)
Evaluate      : path = Σ segment lengths, diag = bbox diagonal
                diag < 100        → Idle (tremor-sized trail)
                path/diag > sensitivity
                                  → emit Trigger{at = t, speed = instant speed}
                                    Cooldown{until = t + cooldown}, trail cleared
                path/diag ≥ sensitivity/2 → Sustaining, else Idle
reset()       : trail cleared (host calls it while a pointer button is held)
```

State summary: `Idle` = no shake forming; `Sustaining` = factor past half
the threshold; `Cooldown` = suppressing window.

Guarantees (each maps to a test requirement):
- G-1: never emits two Triggers closer than `cooldown` apart (FR-008/FR-009,
  SC-006).
- G-2: oscillation too slow to pile up `sensitivity`× path inside `duration` never emits (US2-3).
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
| `peak_speed` | `double` | instantaneous pointer speed (units/s) at the triggering sample (diagnostics) |

Relationships: emitted by the core; the host layer prints the stdout trigger
line (when `triggered`) and then spawns the command.

## 5. Configuration (parsed)

The in-memory image of the TOML file (contracts/config-schema.md). Read once
at startup; converted to `DetectionParams` + `command` afterwards.

| Field | Type | TOML key | Constraint | Default on omission |
|-------|------|----------|------------|---------------------|
| `sensitivity` | `double` | `sensitivity` | > 0 | 8000.0 |
| `duration` | `double` | `duration` | > 0 | 1.0 |
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
