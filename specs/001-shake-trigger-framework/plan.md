# Implementation Plan: Shake-Triggered Command Daemon

**Branch**: `001-shake-trigger-framework` | **Date**: 2026-10-09 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/001-shake-trigger-framework/spec.md`

## Summary

shaker watches device motion on the seat via libinput's udev backend and
executes a user-configured command when the device is shaken: a sustained
stretch of motion above a configured sensitivity that lasts at least the
configured duration, with a cooldown suppressing immediate repeats. The daemon
is a resident, foreground, text-out process configured entirely from a single
TOML file at `XDG_CONFIG_HOME/shaker/config.toml` (four settings: sensitivity,
duration, cooldown, command; the three tunables have defaults, the command
does not). Detection is implemented as a pure, hardware-agnostic core
consuming (timestamp, dx, dy) samples so it is unit-testable without devices;
the existing placeholder `src/main.cpp` event stream is replaced by the
detection pipeline. Command execution is detached (`/bin/sh -c`) so a slow or
hanging command cannot stall detection. New build-time-only dependencies
(toml++ for the config contract, doctest for unit tests) are vendored through
Meson subprojects; the runtime dependency surface stays libinput + libudev.

## Technical Context

**Language/Version**: C++23 (GCC 13+ or Clang 18+; verified locally with GCC
16.2.1). The C++23 compile fuse in `meson.build` stays in place.

**Primary Dependencies**: Runtime: libinput ≥ 1.24 (udev backend, seat0) +
libudev — unchanged. Build-time only: doctest (WrapDB subproject) for the
unit-test executables. The TOML configuration contract is served by a
hand-written, fully unit-tested minimal parser scoped to the documented
4-key flat file (research R-01 revised: toml++ 3.4.0 was verified broken
with the mandated GCC 16 / C++23 combination). No new runtime dependency.

**Storage**: None. The configuration file is read once at startup; no state is
persisted between runs (cooldown is anchored to device event timestamps, not
the wall clock).

**Testing**: Meson test targets built with Ninja:
`tests/test_detection.cpp` (pure detection-core state machine: no libinput,
no udev, no files) and `tests/test_config.cpp` (TOML string/file parsing,
defaults, and error reporting). doctest provides assertions; `meson test -C
build` runs both. Manual smoke validation via `sudo ./build/shaker` is the
third gate per the constitution.

**Target Platform**: GNU/Linux, udev backend attached to seat0. Reading
`/dev/input/*` requires root or membership in the `input` group (documented in
README.md, unchanged). No cross-platform layer.

**Project Type**: Single executable (daemon-style resident process). No
library targets are exposed; tests link a small internal static core so test
and runtime binary share one translation unit set.

**Performance Goals**: SC-001 — command starts ≤ 250 ms after a qualifying
shake completes. The poll-based event loop replaces the current hot spin
(`libinput_dispatch` + `get_event` in an infinite loop) with `poll(2)` on the
libinput fd plus a signalfd, so idle CPU usage is ~0%. Per-event work is O(1)
(timestamp + speed arithmetic). SC-005 — constant resident memory over 1 h
(no per-event allocations; the detection core is a fixed-size state machine
and spawning is an unbuffered exec, not a queue).

**Constraints**: Constitution gates: zero-warning builds at
`warning_level=2`, C++23 fuse, RAII ownership of all C resources, pure
testable detection core, observable text-out behavior. Lightweight footprint:
single binary, tiny codebase; runtime deps limited to libinput + libudev.
Detection state must be anchored to device event timestamps (pure core, no
clock dependency inside the core).

**Scale/Scope**: One user, one seat, all pointer-capable devices on the seat
(config does not name a device — FR-015). Four config settings, one command
per shake. v1 explicitly excludes: per-device servers, pluggable action
systems, network/remote configuration, any UI beyond text output
(constitution VII).

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. C++23-Only Standard | ✅ Pass | Fuse retained; `std::expected`, `std::print`, `std::ranges` used in new code. No standard downgrade. |
| II. Meson as the Sole Build System | ✅ Pass | New dependencies are declared through `dependency()` / subproject in `meson.build` (WrapDB provides meson build files). No hand-written flags outside `meson.build`. |
| III. Zero-Warning Builds | ✅ Pass (risk noted) | Project code must build warning-free at `warning_level=2`. Risk: third-party header-only subprojects may emit warnings; if so this is documented and handled at implementation time (subproject option override is NOT warning suppression of project code — see research.md R-05). |
| IV. RAII & Resource Safety | ✅ Pass | Existing RAII wrappers are kept and extended: unique fd wrapper for signalfd/poll fds; udev/libinput ownership stays RAII; child processes are reaped via `SIGCHLD = SIG_IGN` (no waitpid bookkeeping, no zombies). |
| V. Testable Detection Core | ✅ Pass | Detection is a pure state machine consuming (timestamp, dx, dy) samples and emitting results, parameterized by sensitivity/duration/cooldown, unit-testable without device access. Placeholder `main.cpp` is replaced by the pipeline (constitution explicitly requires this replacement via feature flow — this is that feature). |
| VI. Observable by Default | ✅ Pass | Text contract on `stdout` (device/trigger events), diagnostics on `stderr`, non-zero exit codes on failure, `--debug` verbose per-sample trace for calibration. Config errors print the file location and failing key. |
| VII. Simplicity (YAGNI) | ✅ Pass (2 justified deps) | Runtime dep surface unchanged (libinput + libudev). Build-time additions justified in research.md R-01/R-02 with simpler alternatives explicitly rejected. No daemonization subsystem, no plugin framework, no abstraction layers. |

Post-design re-check: see bottom of this file — **no violations introduced**.

## Project Structure

### Documentation (this feature)

```text
specs/001-shake-trigger-framework/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (/speckit.plan command)
├── data-model.md        # Phase 1 output (/speckit.plan command)
├── quickstart.md        # Phase 1 output (/speckit.plan command)
├── contracts/           # Phase 1 output (/speckit.plan command)
│   ├── config-schema.md     # User-facing TOML configuration contract
│   └── runtime-behavior.md  # CLI, output lines, exit codes, spawn semantics
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
include/shaker/
├── version.hpp          # existing project version macro (unchanged)
├── config.hpp           # config value types + load/validate API (std::expected)
└── detection.hpp        # pure detection core: Sample, Params, State, Result

src/
├── main.cpp             # REWRITTEN: config load → poll(event loop) → dispatch → detect → spawn
├── config.cpp           # TOML parsing (toml++), XDG_CONFIG_HOME resolution, validation
├── detection.cpp        # the pure detection state machine (device-agnostic)
└── exec_command.cpp     # detached child spawn via fork/exec, /bin/sh -c

tests/
├── test_detection.cpp   # doctest; pure core, no hardware
└── test_config.cpp      # doctest; parse from strings/files, defaults, errors

meson.build             # + shaker_core static lib, test suite, subprojects
meson.build (subprojects.tomlpp/doctest via `meson wrap`)
```

**Structure Decision**: Constitution VII fixes the `src/` + `include/shaker/`
single-binary layout, which is preserved. One internal static library target
(`shaker_core`) carries `detection.cpp` + `config.cpp` so the unit-test
executables share the exact translation units shipped in the binary (test
gate requires detection-core unit coverage; duplicating sources per test
target would drift). Tests live in `tests/` with Meson test registration. No
new top-level options, frameworks, or directories beyond the constitution's
floor.

## Complexity Tracking

No constitution violations require justification. The two new build-time-only
dependencies are the only additions to the constitution's dependency floor and
are justified in research.md (R-01, R-02) with rejected alternatives — they do
not constitute a YAGNI violation (both are demanded by the specification: the
TOML contract and the test gate), but they are recorded here for visibility
per Principle VII ("new dependencies MUST be justified in the feature plan"):

| Addition | Why Needed | Simpler Alternative Rejected Because |
|----------|------------|--------------------------------------|
| doctest (build-time, test-only, wrapdb) | Constitution test gate requires unit tests for the pure core; assertion diagnostics and test structure | Raw `assert`-based test executables: rejected — weak diagnostics (assert text only), compiles slower per case, no test naming/filtering; doctest is header-only and test-only so runtime footprint is unchanged |
| signalfd + poll event loop (code only) | Prompt SIGINT/SIGTERM shutdown + idle-time CPU usage; replaces hot-spin loop | Thread-per-signal / busy loop: rejected — thread forks complicate RAII + signal safety and busy loop burns CPU while resident |
| Hand-written minimal TOML parser (~150 lines, fully unit-tested) | Spec mandates the TOML contract (4 flat keys); toml++ 3.4.0 verified broken with GCC 16.2 at C++23 (research R-01 revised) | toml++ dependency: rejected after verification — incorrect parse_result behavior + C++23 header warnings at warning_level=2 with the mandated compiler; no fixed upstream release; patched vendored trees are not sustainable via .wrap. Parser scope is bounded to the contract surface and covered by the whole config test suite |

## Post-Design Constitution Re-Check (after Phase 1)

- **I**: design uses only C++23 features (`std::expected`, `std::print`,
  `std::ranges`, `chrono::duration`). ✅
- **II**: all new build pieces (subprojects, test executables, static lib) are
  meson-native `dependency()`/`test()` declarations. ✅
- **III**: project sources constrained; any third-party header warning is an
  implementation-time item with a documented resolution, never a `-Wno-` flag
  in project code. ✅
- **IV**: every new C resource (signalfd, poll fd, child process, udev nums)
  is owned by an RAII wrapper or auto-reap; no paired manual cleanup. ✅
- **V**: detection core remains pure — data-model.md defines Sample/Params/
  State/Result with event-time anchoring; no libinput types leak into
  `include/shaker/detection.hpp`. Cooldown lives inside the core so
  FR-008/FR-009 are unit-testable. ✅
- **VI**: contracts/runtime-behavior.md fixes the text presentation, exit
  codes, and `--debug` trace. ✅
- **VII**: runtime keeps exactly libinput + libudev; adds no component the
  spec does not demand; `--config` CLI override was rejected in favor of the
  spec-mandated XDG_CONFIG_HOME environment override (also usable for tests).
  ✅

**Gate status: PASS — proceed to Phase 0/1 artifacts (complete).**
