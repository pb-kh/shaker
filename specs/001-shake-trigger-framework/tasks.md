# Tasks: Shake-Triggered Command Daemon

**Input**: Design documents from `/specs/001-shake-trigger-framework/`

**Prerequisites**: plan.md ✅, spec.md ✅, research.md ✅, data-model.md ✅,
contracts/ ✅ (config-schema.md, runtime-behavior.md), quickstart.md ✅

**Tests**: Included — the constitution (Principle V: Testable Detection Core,
test gate `meson test`) requires unit coverage of the pure detection core and
config loader. Test tasks are marked per story and are written before their
implementation.

**Organization**: Tasks grouped by user story from `spec.md`; every task can
be executed by an LLM without additional context beyond the referenced
design docs.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies on incomplete tasks)
- **[Story]**: Which user story this task belongs to (US1, US2, US3)
- Exact file paths included in every description

## Path Conventions

Single project at repository root: `src/`, `include/shaker/`, `tests/`,
`meson.build` (per plan.md Project Structure).

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Vendor build-time dependencies and prepare Meson targets; no
behavioral code yet.

- [x] T001 Vendor build-time dependencies from Meson WrapDB (`meson wrap install tomlplusplus doctest`) and add `dependency()` declarations for both to the root `meson.build` (toml++ linked into lib/executables, doctest only into test targets; WrapDB meson build files keep this Meson-native per constitution II)
- [x] T002 [P] Add `shaker_core` static library target to `meson.build` (`src/detection.cpp`, `src/config.cpp`, `src/exec_command.cpp` added incrementally by later tasks), keep `cpp_std=c++23` fuse + `warning_level=2` intact, keep `shaker` executable linking the core

**Checkpoint**: `meson setup build && meson compile -C build` succeeds with
subprojects vendored and zero warnings.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Host-process scaffolding every user story builds on: RAII fd
ownership, the idle-CPU poll event loop with clean signal shutdown, CLI
surface, and detached command spawning.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [x] T003 [P] Create RAII unique-fd wrapper in `include/shaker/fd.hpp` (owns open fd's via custom close deleter; non-copyable, movable; no manual `close()` pairs anywhere — constitution IV)
- [x] T004 [P] Create include/shaker/exec_command.hpp and src/exec_command.cpp with detached-command spawn: `fork()` + `execvp("/bin/sh", ["sh", "-c", command])`, `exec` failure → `perror` + `_exit(127)`, parent sets `SIGCHLD = SIG_IGN` at startup so children auto-reap (no zombies); spawn failure reported non-fatally via errno (contracts/runtime-behavior.md §5)
- [x] T005 Rewrite src/main.cpp as the poll-based host loop per contracts/runtime-behavior.md: parse/rest of CLI (`--help`, `--version` exit 0; unknown flag → usage-error stderr + exit 2), resolve config path (delegated to config loader in US1 — placeholder error until then), open libinput/udev context (keep existing RAII wrappers), `poll(2)` over `libinput_get_fd()` + signalfd (SIGINT/SIGTERM/SIGHUP-clean-exit-0 with `shaker: exiting on signal` stderr line, no busy loop), keep `DEVICE_ADDED`/`DEVICE_REMOVED` stdout lines and `shaker <version>: watching seat0 — pid <pid>` start line, dispatch-error path exit 1

**Checkpoint**: `sudo ./build/shaker --help` prints usage; SIGINT exits 0;
device add/remove lines stream; loop idles at ~0% CPU.

---

## Phase 3: User Story 1 — Configure and Run the Daemon (Priority: P1) 🎯 MVP

**Goal**: Start the daemon with a valid TOML config (4 settings), watch all
pointer devices, detect a sustained above-threshold shake, and execute the
configured command once per cooldown window.

**Independent Test**: Write `~/.config/shaker/config.toml` (see
contracts/config-schema.md), run `sudo ./build/shaker`, shake the device →
command executes exactly once per window; gentle motion never triggers
(quickstart.md §5 rows US1-1/US1-2/US1-3).

### Tests for User Story 1 ⚠️ (constitution test gate — write first, expect FAIL)

- [x] T006 [P] [US1] Create tests/test_detection.cpp (doctest, no device access) covering the pure core from data-model.md: speed = hypot(dx,dy)/dt derivation + dt clamping, threshold crossing Idle→Sustaining, window broken by below-threshold sample, trigger exactly when elapsed ≥ duration (US2-3/G-2), cooldown suppression never re-triggers until elapsed (FR-008/FR-009, G-1, G-3 continuous-shake case), zero-cooldown edge, non-monotonic timestamp input flagged; feed() returns Result{triggered, at_usec, peak_speed}
- [x] T007 [P] [US1] Create tests/test_config.cpp (doctest) covering happy-path load from in-memory TOML strings: all four keys parsed with documented types, defaults 8000.0/0.25/2.0 substitute for omitted numeric keys, required non-empty `command`, Config → DetectionParams conversion (seconds→µs)

### Implementation for User Story 1

- [x] T008 [P] [US1] Implement configuration module: include/shaker/config.hpp + src/config.cpp — `config_path()` XDG resolution per contracts/config-schema.md §1 (XDG_CONFIG_HOME set/absolute→use, else `$HOME/.config`; then + `shaker/config.toml`), load file, parse with toml++, fill defaults for omitted `sensitivity`/`duration`/`cooldown`, require `command`, return `std::expected<Config, ConfigError>` (FR-002…FR-006, FR-012; error value carries path + reason for US3 polish)
- [x] T009 [US1] Implement the pure detection core (depends on T006): include/shaker/detection.hpp + src/detection.cpp — Sample/Params/State/Result plain-data types and the fixed-size Idle/Sustaining/Cooldown state machine exactly per data-model.md §3 transition table; no I/O, no clocks, no allocation (FR-007, FR-009 groundwork for SC-001/SC-005)
- [x] T010 [US1] Wire the pipeline into the host loop in src/main.cpp (depends on T004, T005, T008, T009): load config first (fatal error → stderr + exit 1, no devices opened), derive DetectionParams, convert `LIBINPUT_EVENT_POINTER_MOTION` → Sample (event µs timestamp, dx, dy; drop + stderr-annotate non-monotonic timestamps), feed DetectionCore each dispatcher pass; on Trigger print `shake! speed=… units/s (peak …) → running: <command>` to stdout and spawn via exec_command without waiting (FR-001, FR-007, FR-015 via seat model, FR-016, US1 acceptance scenarios)

**Checkpoint**: US1 complete end-to-end — `sudo ./build/shaker` with valid
config triggers exactly once per cooldown window; `meson test -C build`
passes the two story suites; constitution smoke gate observed.

---

## Phase 4: User Story 2 — Customize Detection Through the Configuration File (Priority: P2)

**Goal**: Tuning sensitivity/duration/cooldown demonstrably changes trigger
behavior, and `--debug` makes calibration observable (constitution VI
diagnostic mode).

**Independent Test**: Change one setting, restart, observe direction-consistent
behavior change (e.g. halve sensitivity → milder shake now triggers);
cooldown windows honored (quickstart.md §5 rows US2-1/US2-2/US2-3).

### Tests for User Story 2 ⚠️

- [x] T011 [P] [US2] Extend tests/test_detection.cpp with the parameter-effect matrix (same file, different cases): same sample sequence under permissive vs strict sensitivity yields different outcomes (US2-1), duration-boundary cases (window shortened → triggers; shortened below elapsed → not), cooldown timing across synthetic timelines (`+1s no / +5.1s yes` pattern per quickstart US2-2), `cooldown = 0.0` re-trigger allowed, peak_speed reported on Trigger for calibration

### Implementation for User Story 2

- [x] T012 [US2] Add calibration observability to src/main.cpp: parse `--debug` flag (no-arg, config-independent) and emit per-sample stderr trace `debug: t=+<elapsed>s speed=<speed> state=<IDLE|SUSTAIN|COOLDOWN>` (+`window=+<elapsed>s` while SUSTAIN), `debug: TRIGGER at t=+<elapsed>s peak=<peak>` lines on trigger, plus the non-monotonic-timestamp diagnostic (see contracts/runtime-behavior.md §2.2); debug only adds visibility, never changes decisions

**Checkpoint**: US2 independently testable — tuning effects proven by unit
matrix; manual calibration possible with `--debug` alone
(quickstart.md §3 ritual).

---

## Phase 5: User Story 3 — Safe Defaults and Clear Startup Behavior (Priority: P3)

**Goal**: First-run and broken-config failures speak precisely: exact file
path, offending key/value, exit 1 without touching any device; omitted
numeric settings always fall back to documented defaults.

**Independent Test**: Run with (a) no config file, (b) each malformed
variant, (c) partial config — verify stderr shapes + exit codes + no
monitoring (quickstart.md §5 rows US3-1/US3-2/US3-3; SC-003).

### Tests for User Story 3 ⚠️

- [x] T013 [P] [US3] Extend tests/test_config.cpp with the full error ladder + resolution rows (same file, new cases): each contract §3 shape (#1 missing/unreadable, #2 syntax, #3 missing/empty command, #4/#5 non-positive sensitivity/duration, #6 negative cooldown, #7 wrong TOML type), unknown top-level key → warning + not fatal, `config_path()` rules (absolute XDG honored; unset/empty/relative → `$HOME/.config` fallback), path printed verbatim in every error value

### Implementation for User Story 3

- [x] T014 [US3] Harden src/config.cpp + src/main.cpp startup messaging to match contracts/config-schema.md §3 exactly (single stderr line `shaker: …` per category with resolved path verbatim, offending key + got-value), unknown-key stderr warnings, ensure validation completes and exits 1 strictly before any libinput/udev setup (no monitoring on failure, FR-010/FR-011/FR-012, US3 scenarios)

**Checkpoint**: All three user stories independently functional; config
failures can no longer produce confusing jobs at runtime.

---

## Phase N: Polish & Cross-Cutting Concerns

**Purpose**: Constitution gates + project self-description.

- [x] T015 [P] Update README.md: replace the placeholder print-events description with the daemon usage (config location incl. `XDG_CONFIG_HOME/shaker/config.toml`, four keys table, build/test/run commands, input-group prerequisite, quickstart reference) — the deferred TODO recorded in the constitution's `SYNC IMPACT REPORT` deferred TODO
- [x] T016 [P] Zero-warning gate sweep across all targets including subprojects (constitution III): fix any project-source warnings directly (no `-Wno-`/`#pragma` suppression anywhere); if a third-party header warns, handle subproject-side (e.g. wrap `override_options`: documented in plan.md Complexity Tracking) and record the outcome
- [x] T017 Execute quickstart.md end-to-end as the final validation (SC-001…SC-007 table) — meson test green, acceptance-row walk-through recorded in the PR/commit message, constitution commit discipline (small atomic commits per task, constitution gate 4)
- [x] T018 [US3] Auto-initialize a missing config file
- [x] T020 [US3] v1→v2 config migration: rewrite untouched v1-generated
  templates (`sensitivity = 8000.0` in units/second — unreachably large
  under v2's pointer-units-per-swing semantics, which swallowed all real
  shakes) to the current template on load, with an informational stderr
  note; hand-edited files are never rewritten (exact-match guard);
  tests: migrate-idempotent, hand-edited-untouched; research R-03
  Addendum 2
- [x] T019 [US1] Detection semantics v2 (user-validated): replace the
  sustained-speed window with direction-reversal counting (research R-04
  v2) — per-axis hysteresis over accumulated displacement (`sensitivity`
  = pointer units per swing leg, default 40.0), shake = 3 swings within
  `duration`; cooldown machinery unchanged; update core
  (`include/shaker/detection.hpp`, `src/detection.cpp`), full test suite
  incl. the user-reported Surface trace regression, config template +
  default in `include/shaker/config.hpp` + `src/config.cpp`, docs
  (contract §2/§4, data-model §1–§4, README, quickstart, plan summary)
  and the `--debug` trace fields (swings) in `src/main.cpp`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: no dependencies — start immediately
- **Foundational (Phase 2)**: depends on Phase 1 targets existing (T005 compiles against `shaker_core`) — BLOCKS all user stories
- **User Story 1 (Phase 3)**: depends on Foundational (needs fd wrapper T003, spawn T004, loop T005)
- **User Story 2 (Phase 4)**: depends on US1's detection core + connected pipeline (tests extend the same files)
- **User Story 3 (Phase 5)**: depends on US1's config module (tests extend tests/test_config.cpp, hardening edits src/config.cpp/src/main.cpp)
- **Polish (Phase N)**: after all desired user stories

### User Story Dependencies

- **US1 (P1)**: starts after Phase 2 — no other-story dependencies; alone is the MVP
- **US2 (P2)**: touches tests/test_detection.cpp (needs T009) + src/main.cpp trace (needs T010); independently testable via `meson test` + manual `--debug`
- **US3 (P3)**: touches tests/test_config.cpp (needs T008) + same src files; independently testable via `meson test` + `XDG_CONFIG_HOME` override

### Within Each User Story

- Tests (TDD constitution gate) precede their implementation:
  T006→T009, T007→T008, T011→(T009 tune matrix validates after), T013→T014
- Models/types before wiring: T008/T009 before T010
- Story complete (Checkpoint) before the next priority begins

### Parallel Opportunities

- Phase 1: T002 parallel (file-scoped edits after T001's wrap install)
- Phase 2: T003 ∥ T004 (different files); T005 last
- Phase 3: T006 ∥ T007; T008 ∥ T009 (config vs detection files) → T010
- Cross-story: US2 and US3 are disjoint file sets (except main.cpp messaging in T014) and could run in parallel under capacity, but each phase gate should land on its own commit
- Polish: T015 ∥ T016

---

## Parallel Example: User Story 1

```bash
# After Foundational: launch the two test suites together
Task: "tests/test_detection.cpp detection-core scenarios (T006)"
Task: "tests/test_config.cpp happy-path config cases (T007)"

# Then the two implementation modules together
Task: "include/shaker/config.hpp + src/config.cpp parser/defaults (T008)"
Task: "include/shaker/detection.hpp + src/detection.cpp state machine (T009)"

# Finally the single wiring task
Task: "src/main.cpp pipeline wiring (T010)"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Phase 1 Setup → vendored deps compile
2. Phase 2 Foundational → loop, CLI, spawn survive SIGINT
3. Phase 3 US1 → full detect-trigger-execute path, unit tests green
4. **STOP and VALIDATE**: quickstart.md §5 US1 rows on real hardware
   (`sudo ./build/shaker`)
5. This is a deployable v1 skeleton (spec P1 delivered)

### Incremental Delivery

1. US1 → MVP (works end-to-end with all four settings supplied)
2. US2 → tunable + calibratable (`--debug`, parameter matrix)
3. US3 → hardened startup UX (exact errors, defaults proven)
4. Polish → README truth + zero-warning sweep + full-gate validation

### Notes (constitution workflow)

- Tests are written first and MUST fail before the implementation lands;
  `meson test -C build` runs the whole suite per task
- Commit after each task or logical group (constitution gate 4)
- Smoke gate (constitution gate 3) applies at T010/T012 checkpoints —
  observation must be recorded
- Stop at any checkpoint to validate a story independently
