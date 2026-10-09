# Research: Shake-Triggered Command Daemon

**Feature**: `001-shake-trigger-framework` | **Date**: 2026-10-09
**Method**: Direct verification against the local project and toolchain
(`meson --version`, `g++ --version`, `pkg-config`, `meson wrap search/info`)
plus analysis of `src/main.cpp`, `meson.build`, the constitution, and libinput
API as used in the existing placeholder. No external open questions remain.

---

## R-01: TOML parsing (REVISED after verification)

**Decision (final)**: Hand-written minimal TOML-subset parser scoped to the
documented contract: flat `key = value` lines for the four keys, floats/
ints (with `_` separators), quoted strings with the common escapes, end of
line / mid-line comments, whitespace flexibility. Anything else (tables,
arrays, multi-line strings, dotted keys) produces a **parse error** with a
line number — the conservative, contract-aligned behavior. No new runtime or
build-time dependency.

**Rationale (replacing the original adoption)**: The originally chosen
toml++ 3.4.0 was verified to be unusable in this project's mandated
combination: with GCC 16.2.1 at C++23, `toml::parse_result` malfunctions
(`parsed` even reports type `toml::v3::table`; `operator bool` is
unreachable; vendored headers additionally emit
`-Wdeprecated-literal-operator` warnings at warning_level=2, a constitution
III issue). No fixed upstream release exists (3.4.0 is the latest tag), and
patching a tarball-vendored subproject is not sustainable (patches are not
carried by the vendored `.wrap` file). Fidelity risk of the ~150-line parser
is contained: the contract surface is exactly four flat scalar keys
(contracts/config-schema.md), the parser rejects unsupported constructs with
clear errors, and its behavior is fully covered by unit tests
(tests/test_config.cpp covers the whole error ladder + defaults + path
rules). This also strengthens constitution VII (dependency floor stays
libinput + libudev; the only WrapDB dependency left is test-only doctest).

**Alternatives considered (including the ones originally preferred)**:
- *toml++ 3.4.0 wrapdb*: rejected — verified broken with this toolchain at
  C++23 (above); adopting it would force either a fragile header warning
  suppression + patched vendored tree, or a build that only works on older
  GCC. (Upstream issue exists; upstream's last release predates the fix.)
- *toml11*: same class of risk; additionally no maintained WrapDB meson
  port. Rejected.
- *System package (pacman)*: not reproducible from the repo. Rejected.
- *Hand-written parser*: adopted — see Decision.

**Parser scope (contract-level note)**: v1 accepts only the documented flat
`key = value` form; unsupported TOML constructs fail fast with a parse
error. This is recorded in contracts/config-schema.md §2.

## R-02: Unit test framework

**Decision**: Vendor **doctest** (WrapDB, test-only) for the unit-test
executables; runtime binary unchanged.

**Rationale**: Constitution test gate requires detection-core unit coverage
with `meson test`. doctest is a single header, compiles fast, gives
readable assertion output (values on failure, named tests, filtering) that
matters for a state-machine core with many boundary cases. It links only into
test targets: the runtime dependency floor (libinput + libudev) is untouched,
so constitution VII holds.

**Alternatives considered**:
- *Raw `assert` executables*: simplest zero-dep option; rejected for weak
  diagnostics (assert text only, first failure stops the suite) and slower
  iteration when tuning the detector.
- *Catch2 v3*: better library-mocking, heavier compile cost and footprint —
  no need at this scale. Rejected.
- *No tests / manual only*: violates constitution V + test gate. Rejected.

**Notes**: Verified `meson wrap search doctest` → available.

## R-03: Config location resolution (XDG)

**Decision**: Resolve the config path as
```
$XDG_CONFIG_HOME/shaker/config.toml
```
with fallback `$HOME/.config/shaker/config.toml` when `XDG_CONFIG_HOME` is
unset **or empty or not absolute**. Exactly one file is read at startup;
relative-path or empty `XDG_CONFIG_HOME` values are ignored (per the XDG
basedir spec) and fall back to the default. The resolved absolute path is
included verbatim in all config error messages (US3).

**Rationale**: Matches the spec's stated location and the freedesktop
basedir convention used by existing Linux tools. Implementing the fallback in
one pure helper function (`config::config_path()`) keeps it testable
(override env in tests) without a `--config` CLI flag — YAGNI: the env
override already serves tests and portable setups (constitution VII
post-check).

**Alternatives considered**:
- *`--config PATH` CLI flag*: useful override, but adds surface not required
  by the spec; `XDG_CONFIG_HOME=/tmp/... ./build/shaker` covers test and
  multi-user cases. Rejected for v1 (revisit at a future spec if needed).

**Addendum (user-requested v1 amendment)**: a *missing* config file is now
auto-initialized. shaker writes the documented default template (the
contract §2 example values, including an example
`command = "notify-send 'Shake detected'"`) after creating missing parent
directories, logs one informational stderr note, and keeps running — a
zero-config first run now works end-to-end. Constraints preserved from the
original design: an existing unreadable file still errors, the
required-`command` key rule still applies to *existing* files
(no safe command default), and auto-initialization never overwrites an
existing file. Template write failures are fatal:
`cannot write default config '<path>'`. Implemented by
`create_default_config()` in `src/config.cpp`; first-run behavior covered
in `tests/test_config.cpp`.

**Addendum 2 (v2 migration)**: files that match the untouched v1
auto-generated template **verbatim** are migrated once to the current
template on load (`migrate_v1_default_config()`), with an informational
stderr note. This matters because the v1 template's
`sensitivity = 8000.0` was in units/second, which v2 reads as *pointer
units per swing leg* — an impossible threshold that silently
swallowed every real shake (user-validated regression). Hand-edited
files are never rewritten: only an exact template match proves the file
is an untouched generated file. Covered by two dedicated tests
(migrates + idempotent second load; hand-edited file untouched).

## R-04: Shake detection definition and algorithm

**Decision**: A shake is **motion sustained above a speed threshold for at
least a minimum duration**. The pure core consumes samples
(monotonic-microsecond timestamp, dx, dy per libinput pointer-motion event,
interleaved from all seat pointers), derives speed, and runs a fixed-size
state machine:

```
speed(sample) = hypot(dx, dy) / dt_seconds   (dt from consecutive sample
                                              timestamps, clamped to a
                                              small minimum to avoid
                                              huge values on rare
                                              clock jitter)
IDLE        : speed ≥ sensitivity  → SUSTAINING (window_start = t)
SUSTAINING  : speed < sensitivity  → IDLE      (window broken)
              window elapsed ≥ duration (t - window_start)
                                    → TRIGGER + COOLDOWN (until = t + cooldown)
COOLDOWN    : t ≥ cooldown_until    → back to IDLE evaluation on next sample
              t < cooldown_until, any speed → stay, never re-trigger
```

- Sensitivity = speed in **device units per second** (libinput deltas are in
  device units; keeping the raw unit complies with the spec's assumption that
  sensitivity is relative to the device's own reported motion, not physical
  units).
- Duration/cooldown are configured in **seconds** and converted to the core's
  microsecond event-time domain.
- The window is anchored to **event timestamps** only: no wall-clock calls
  inside the core, so the whole behavior is replayable in unit tests with
  synthetic sample sequences.
- Research R-04 v3 (user feedback): slow, big-amplitude side-to-side
  movement registered as a shake. Added internal travel/cadence gates on
  swing legs (≤ 350 ms and ≤ 2000 pointer units per leg); SUSTAIN now
  reports from the 2nd swing. Rejected alternative: exposing the caps as
  config keys — YAGNI for v1, the fixed defaults cover the reported case.

**Rationale**: FR-007 requires a motion meeting *both* sensitivity and
duration; a speed threshold plus sustained-elapsed requirement satisfies it
directly with O(1) state (window start + last sample + cooldown deadline).
The sustained-requirement inherently rejects single bumps/taps (spec edge
case: "a single abrupt bump or tap is NOT a shake") because their
above-threshold stretch is shorter than a typical `duration`. It also matches
US2's tuning story (raise sensitivity → false triggers stop; lower → weaker
shakes catch; shorten duration → snappier triggers).

**Alternatives considered**:
- *Direction-reversal counting* (count sign flips of the dominant axis in a
  window): better matches "back-and-forth" literally and rejects sustained
  one-way fast drags, but adds parameters (reversal count, lookback) not
  present in the four-key config contract, complicating tuning. Deferred:
  accepted limitation for v1 that a fast sustained one-way drag can trigger;
  documented in contracts/runtime-behavior.md and in quickstart calibration
  guidance. Rejected as not v1.
- *Event-count thresholds* (N events above X each sequence): brittle w.r.t.
  device polling rate (1000 Hz mouse vs 125 Hz trackpoint). Rejected —
  duration-in-time is rate-independent.
- *Magnitudes only (no speed)*: WOULD misfire on slow long drags; speed
  normalizes across durations. Rejected.

**Interleaving note (accepted limitation)**: all pointer devices feed one
shared core in event order.

**Refinement (v2, user-validated)**: v1's sustained-speed window failed on
real hardware — a reported Surface trace ("shaking as hard as I want")
peaked at ~3,400 units/s while the direction reversals pulled the smoothed
speed down to ~600–1,000 between swings; *any* threshold consistent with
"ordinary use never triggers" either breaks the window at every reversal
or fires on ordinary pointer use. Velocity magnitude fundamentally cannot
discriminate a shake from regular use: reversals can. v2 therefore
redefines the detector as **direction-reversal counting with hysteresis**:

- the core accumulates displacement per axis and counts a *swing* whenever
  the axis position moves back by at least `sensitivity` units from its
  phase extreme (per-axis hysteresis, so flips that converge without real
  displacement are ignored);
- a **shake = ≥ 3 swings (internal constant) within `duration` seconds**
  (window anchored to the first counted swing, event-time only);
- straight, sustained or wandering motion produces 1–2 swings per window
  at most → stays silent; oscillating shaking produces ≥ 3 → triggers;
- `sensitivity` is now **pointer units per swing leg** (units changed!) —
  default 40.0; `duration` and `cooldown` keep their seconds semantics;
- cooldown/state machine/output contracts, idle/gap resets, and the pure,
  replayable character of the core are unchanged.

**Alternatives considered (v2)**:
- *Lower the speed threshold (sensitivity ~1,000 units/s)*: your reversal
  troughs (~600–900) would stay above it only barely; any real pointer
  usage then blows past the window constantly. Rejected (false triggers
  during normal use).
- *Peak-counting of the smoothed speed*: depends on dip ratio (~25% here)
  which varies per device; still speed-based. Rejected.
- *Reversal counting with per-axis hysteresis* (chosen): matches the
  physical definition of a shake, robust to drift (extremes re-anchor per
  leg), simplest fixed-size implementation. Simultaneously moving two devices can distort
inter-event dt and thus speed for flapping windows; single-user single-seat
reality makes this negligible in v1. Per-device cores rejected: require a
device→state map and device identity in the pure sample type for a case the
spec does not demand (spec: "monitors all devices", nothing about
per-device isolation).

## R-05: Host event loop + signals

**Decision**: Replace the hot-spin loop in `main.cpp` with a `poll(2)`-based
loop over the libinput fd (`libinput_get_fd()`) plus a **signalfd** carrying
SIGINT/SIGTERM/SIGHUP; `poll` blocks, so the resident process idles at ~0%
CPU. A `--debug` flag enables a verbose per-sample trace (speed, effective
states) on **stderr** for calibration; normal run prints device add/remove
and trigger lines on **stdout** (constitution VI).

**Rationale**: FR-017 (prompt clean termination) and FR-016/FR-018 (detached
command execution never stalls detection; lightweight) are both properties of
the loop. `poll` on the already-available libinput fd plus signalfd is the
minimal-signals-safe pattern for a single-threaded daemon: signals arrive as
data on the poll set (no EINTR juggling, no async-handler side effects), so
fork/exec of children stays in the one program-visible thread.

**Alternatives considered**:
- *Keep busy loop*: wastes CPU in a resident process (SC-005 goal contradicts
  it). Rejected.
- *Thread-per-signal / dedicated watcher threads*: complicates RAII + fork
  (child-safety only matters pre-exec), zero benefit in v1. Rejected.

## R-06: Detached command execution

**Decision**: On trigger, **fork + exec `/bin/sh -c <command>`** in the main
loop thread and never `waitpid()` — `SIGCHLD` is pre-set to `SIG_IGN` at
startup so reaping is automatic (no zombies under POSIX). The child is built
with only `execvp` after fork; on exec failure the child does `_exit(127)`
(one write to the pipe-less stderr inherited lets the user see spawn
failures). Environment and cwd are inherited from the daemon, matching the
spec's "command runs with the daemon's privileges by design".

**Rationale**: FR-007 + FR-016 + FR-018: the command must not block
detection; execution must be neither buffered, queued, nor joined. `/bin/sh
-c` lets the portable free-text command use pipes, redirections and shell
conveniences (that's why the spec calls it "free text"). fork/exec in a
single-threaded process is trivially fork-safe; nothing else in v1 runs
between fork and exec.

**Alternatives considered**:
- *posix_spawn*: slightly safer than fork+exec under threads, but internally
  does the same here; more call surface to wrap; fork is fine at
  single-threaded design. Rejected for consistency with the loop's
  simplicity (not a hard technical rejection; both would be constitution-
  compliant, fork chosen for a smaller wrapper).
- *system(3)*: blocks the loop and does its own signal fiddling — directly
  violates FR-016/SC-005. Rejected.

## R-07: Process model ("daemon") for v1

**Decision**: shaker runs as a **foreground resident process** printing its
observation stream; backgrounding is left to the terminal/service manager.
No built-in double-fork daemonization; README and quickstart provide a
systemd user service unit (type=simple) plus the classic `shaker &` flow.

**Rationale**: Constitution VI requires the process to stay observable
(stdout events) — a classically daemonized process with closed stdio would
conflict with it, unless special logging plumbing is added (premature).
Modern Linux daemons under systemd are expected NOT to self-fork. The spec's
"daemon" is satisfied by the resident, UI-free, long-running nature; SC-007
setup time is shortest in this model.

**Alternatives considered**:
- *Built-in daemonize (double-fork, setsid, reopen fds)*: silent mode
  conflicts with observable-by-default and adds failure modes around
  logging; rejected for v1 as unjustified complexity (constitution VII).
- *libinput gesture swipe as alternate trigger*: covers touchpads only;
  pointer motion already covers mice/trackpoints. Out of scope (documented
  as future spec input).

## R-08: libinput event-time semantics for interleaved hotplug

**Decision**: keep the existing udev-seat event loop semantics and add:
- treat `DEVICE_ADDED` of pointer-capable devices as observation-only
  (FR-015: every device is already included automatically by the seat model)
- treat `DEVICE_REMOVED` as observation-only (FR-013 auto-satisfied:
  libinput unbinds the device; core keeps state, cooldown continues — no
  special handling needed)
- startup with zero pointer devices is fine: the udev backend keeps
  listening; the poll loop wakes on later add (FR-014). No special code path.

**Rationale**: FR-013/014/015 are already guaranteed by libinput's udev
monitor design; the plan is to explicitly *not* add device-tracking logic
the library already provides (simplicity). `libinput_dispatch()` errors are
exit-with nonzero as today.

**Alternatives considered**:
- *Manual device list tracking + per-device reconnect logic*: duplicates the
  library. Rejected.

## R-09: Version and CLI surface

**Decision**: Support `shaker --help` (usage text, exit 0) and
`shaker --version` (print version from `SHAKER_VERSION`, exit 0). No other
flags in v1 other than `--debug` (R-05, constitution VI). Keep
`meson.project_version()` = 0.1.0 (first-version app).

**Rationale**: --version/--help are the two flags any Unix utility supports
(`--help` also documents the config path). Everything else is config-file
driven, matching the spec's single configuration interface.

**Alternatives considered**:
- *No flags at all* ("config only"): hinders debugging per constitution VI
  (verbose mode is required) — --debug is that minimum. Rejected.

---

## Fully resolved NEEDS CLARIFICATION items

The Technical Context required no `NEEDS CLARIFICATION` markers to be opened;
every unknown was resolved by R-01…R-09, with units and defaults concretized
in contracts/config-schema.md (defaults are documented starting-points for
calibration, not hard requirements from the spec).
