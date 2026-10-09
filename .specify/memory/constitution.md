<!--
=== SYNC IMPACT REPORT ===
Version change: N/A → 1.0.0 (initial ratification; template placeholders replaced)
Modified principles: none (first version)
Added sections:
  - Core Principles I–VII (C++23-Only Standard; Meson as the Sole Build System;
    Zero-Warning Builds; RAII & Resource Safety; Testable Detection Core;
    Observable by Default; Simplicity / YAGNI)
  - Additional Constraints & Domain Requirements
  - Development Workflow & Quality Gates
  - Governance
Removed sections: none (template comment scaffolding dropped)
Templates requiring updates:
  - .specify/templates/plan-template.md ✅ (generic "Constitution Check" gate placeholder
    remains valid; gates are resolved from this constitution at plan time)
  - .specify/templates/spec-template.md ✅ (no constitution-specific sections affected)
  - .specify/templates/tasks-template.md ✅ (generic; principle-driven task types covered: C++23, Meson tests, warning discipline)
  - .specify/templates/commands/* ✅ (no command directory present)
Deferred / follow-up TODOs:
  - README.md still describes the placeholder behavior ("prints input events");
    update when the shake-detection MVP lands (manual follow-up, not a constitution change)
=== END SYNC IMPACT REPORT ===
-->

# shaker Constitution

## Core Principles

### I. C++23-Only Standard (NON-NEGOTIABLE)

All source code compiles under C++23 (`__cplusplus >= 202302L`). The
compile-time fuse in `meson.build` MUST remain in place and MUST fail the build
when the selected compiler cannot prove C++23 support. Language and standard
library features from C++23 and earlier only; compiler support floor is GCC 13+
or Clang 18+. Downgrading the standard to make the build pass is prohibited; any
workaround MUST be justified and documented in the implementation plan.

**Rationale**: The project deliberately targets C++23 idioms (`std::expected`,
`std::print`, `std::ranges`) as part of its charter; silently falling back to an
earlier standard would violate the project's reason for existence.

### II. Meson as the Sole Build System

Every build, dependency resolution, and test run MUST be orchestrated through
Meson with the Ninja backend. Dependencies MUST be declared via `dependency()`
(pkg-config) in `meson.build`, never via hand-written include/link flags in
scripts or IDE files. Ad-hoc filesystem paths or shell-side compile commands are
not accepted as project artifacts.

**Rationale**: A single declarative build description keeps the dependency set
(libinput ≥ 1.24, libudev) discoverable, reproducible, and portable.

### III. Zero-Warning Builds

`warning_level=2` MUST remain set. A build of changed code MUST complete with
zero compiler warnings. Compiler warnings MUST NOT be suppressed
(`-Wno-...`) or hidden behind local `#pragma` directives; the underlying issue
MUST be fixed instead. Any exception requires a documented justification in the
implementation plan and a tracked task to remove it.

**Rationale**: For a small, dependency-light binary, warnings are the cheapest
available defect detector; allowing them to accumulate erodes that signal.

### IV. RAII & Resource Safety

All ownership of C resources — the `udev` context, the libinput interface, the
libinput context, device handles, and event references — MUST be expressed
through RAII types (destructors/cleanup via types such as `std::unique_ptr`
with custom deleters, `std::expected` returns, or equivalent owning wrappers).
Manual `fclose`/`free`-style paired cleanup is not allowed; cleanup belongs in
destructors. A libinput event reference MUST NOT outlive the enclosing
read/dispatch call
that produced it.

**Rationale**: The program touches udev and raw device-node plumbing; leaks or
dangling handles turn into hard-to-reproduce runtime faults that violate the
"run continuously and observe" contract.

### V. Testable Detection Core

The purpose of shaker is to detect cursor shaking and to trigger configurable
actions in response. Detection MUST be implemented as a pure, hardware-agnostic
component: a stateful transform that consumes timing and pointer-motion samples
(plain data: timestamps, dx/dy) and emits detection results. The detection
signature and dispatch MUST be parameterizable (thresholds/frequency/decision
window) and MUST be unit-testable without libinput, udev, or device access.

The current `src/main.cpp`, which streams raw input events to stdout, is an
intentional placeholder that establishes the event source; it MUST be replaced
by the detection pipeline through feature specifications, not grown by accretion.

**Rationale**: Hardware access in tests makes the algorithm unverifiable in CI
and untestable in edge cases; only a pure core can be tuned and proven.

### VI. Observable by Default

The program MUST remain a text-in/text-out Unix citizen: human-readable events
and detections on `stdout`, diagnostics and errors on `stderr`, and non-zero
exit codes on failure. A structured/verbose diagnostic mode MUST be available
so detection behavior can be observed during calibration and debugging. Silent
operation without any observation channel constitutes a defect.

**Rationale**: An input-reaction daemon whose decisions are invisible cannot be
tuned or trusted; plain text output keeps it debuggable, scriptable, and
composable.

### VII. Simplicity (YAGNI)

Start minimal: a single executable, a small dependency surface
(libinput + libudev only), and the `src/` + `include/shaker/` layout. New
dependencies, abstractions, or extra targets MUST be justified in the feature
plan by a concrete need, with the simpler alternative explicitly rejected.
Premature generalization (pluggable frameworks, IPC servers, configuration
systems) is prohibited until a specification demands it.

**Rationale**: This is currently a one-binary utility; complexity buys
maintenance burden without adding to its utility.

## Additional Constraints & Domain Requirements

- **Platform**: GNU/Linux with libinput ≥ 1.24 (pkg-config module
  `libinput`) and `libudev`. No cross-platform abstractions are required; a
  non-Linux port MUST go through a new specification.
- **Event source**: The udev backend attached to `seat0` is the standard event
  source. Opening `/dev/input/*` typically requires root or membership in the
  `input` group; this prerequisite MUST stay documented in `README.md`.
- **License**: MIT, as declared in `meson.build`; all contributed files MUST be
  compatible with that.
- **Unknowns are explicit**: Anything unresolved about the shake signature,
  thresholds, or available action set MUST appear as a `[NEEDS CLARIFICATION]`
  item in the feature spec rather than as an implicit assumption in code.

## Development Workflow & Quality Gates

Features MUST pass these gates before completion:

1. **Build gate**: `meson compile -C build` succeeds with zero warnings and the
   C++23 fuse passes (Principles I–III).
2. **Test gate**: Once a test suite exists, `meson test -C build` MUST pass;
   detection-core changes MUST be covered by unit tests (Principle V).
3. **Smoke gate**: Input-dependent features MUST be validated manually with
   `sudo ./build/shaker` (or equivalent) and the observation recorded
   (Principle VI).
4. **Commit discipline**: Commits are small and atomic; each task or logical
   group lands as its own commit.

The workflow follows the specification-to-implementation pipeline
(`/speckit.specify` → `/speckit.plan` → `/speckit.tasks` → `/speckit.implement`);
the implementation plan's Constitution Check documents gate compliance.

## Governance

This constitution supersedes all other project practices and ad-hoc
conventions. Where any document conflicts with this constitution, the
constitution governs.

- **Amendment procedure**: Amendments MUST be proposed in a change document,
  reviewed, and committed with an updated version line and date. Each
  amendment states which sections were affected.
- **Versioning policy**: Semantic versioning applies. MAJOR: a principle is
  removed, redefined incompatibly, or governance becomes incompatible.
  MINOR: a new principle is added materially, or existing guidance is expanded
  in a compatible way. PATCH: wording, clarifications, typo and non-semantic
  refinements. A fresh constitution (initial ratification) starts at 1.0.0.
- **Compliance review**: Every pull request/review MUST verify compliance with
  the gates above; complexity or standard exceptions MUST be justified in the
  implementation plan's Complexity Tracking table.

**Version**: 1.0.0 | **Ratified**: 2026-10-09 | **Last Amended**: 2026-10-09
