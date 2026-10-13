# Feature Specification: Shake-Triggered Command Daemon

**Feature Branch**: `001-shake-trigger-framework`

**Created**: 2026-10-09

**Status**: Draft

**Input**: User description: "this is the first version of the app. it must be a lightweight daemon, configured with toml at XDG_CONFIG_HOME/shaker/config.toml. this config should include sensitivity of shake, duration of shake, cooldown and a command to execute on shake."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Configure and run the daemon (Priority: P1)

A user places a supported input device on their desk, writes a small TOML
configuration describing how a "shake" should be recognized (how strongly and
how long the device must be moved) and which command to run when one happens,
then starts the daemon. From that point on, the daemon quietly watches the
device: whenever the user picks the device up and shakes it, the configured
command runs automatically. The user never has to touch the daemon again until
they want to change its behavior.

**Why this priority**: This is the core purpose of the product. Running the
daemon with a working configuration that translates a physical shake into an
automated action is the entire value proposition of v1; without it nothing
else matters.

**Independent Test**: Can be fully tested by writing a valid configuration
file, starting the daemon, physically shaking a connected device, and
observing that the configured command executes – and that it does not execute
when the device is merely moved normally.

**Acceptance Scenarios**:

1. **Given** a device-producing shake-strength events below the configured
   sensitivity is being monitored, **When** the user moves the device gently,
   **Then** no command is executed.
2. **Given** the daemon is monitoring a device, **When** the user performs a
   motion that meets both the configured sensitivity and the configured shake
   duration, **Then** the configured command is executed exactly once.
3. **Given** the configured command fails (non-zero result), **When** the
   shake is detected, **Then** the daemon stays running and executes the
   command again on the next qualifying shake after the cooldown.

### User Story 2 - Customize detection through the configuration file (Priority: P2)

A user finds that real shakes are being missed or that ordinary movements are
being misread as shakes. They open the TOML file at the configuration
location, adjust how strong a movement must be (sensitivity), how long the
strong movement must last (duration), and how long to ignore further shakes
after one is detected (cooldown), then restart the daemon. The changed
behavior matches their edit: weaker movement after lowering sensitivity now
triggers, and normal desk motion after raising it no longer does.

**Why this priority**: Tunability is what makes the detector usable across
different devices and users. A fixed threshold would either miss real shakes
or flood the user with false triggers, so this is the first thing users will
adjust, but it depends on US1's working detection loop.

**Independent Test**: Can be tested by changing one setting at a time (for
example halving sensitivity) and confirming the trigger behavior changes in
the expected direction, without any other part of the daemon being modified.

**Acceptance Scenarios**:

1. **Given** two configuration files identical except for sensitivity,
   **When** the same motion is performed against each, **Then** the motion
   that qualifies under the more permissive file does not necessarily qualify
   under the stricter one.
2. **Given** a configuration with a known cooldown, **When** shakes are
   performed more frequently than the cooldown, **Then** only shakes outside
   cooldown windows execute the command.
3. **Given** a configuration with a known duration requirement, **When** a
   motion is strong but shorter than the configured duration, **Then** no
   command is executed.

### User Story 3 - Safe defaults and clear startup behavior (Priority: P3)

A new user starts the daemon for the first time without having created a
configuration file, or with a typo in it. Instead of silently doing nothing or
crashing cryptically, the daemon tells them exactly what is wrong (file
missing, unreadable, or which setting line is invalid) and stops. When a
correct file exists but omits some settings, the daemon fills in documented
defaults and still runs.

**Why this priority**: First-run experience matters, but it only becomes
relevant once the daemon works (US1) and is tunable (US2). It protects new
users from silent misbehavior, but a user who already has a working setup
never encounters these paths.

**Independent Test**: Can be tested by launching the daemon with (a) no
config file, (b) a malformed file, and (c) a partial file, and verifying
error messages, default substitutions, and a clean exit in the failing cases.

**Acceptance Scenarios**:

1. **Given** no configuration file exists, **When** the daemon starts,
   **Then** it exits with a clear message naming the expected path and does
   not monitor any device.
2. **Given** the configuration file contains a structural or value error,
   **When** the daemon starts, **Then** it exits with a message identifying
   the offending item and does not monitor any device.
3. **Given** a valid configuration omitting some settings, **When** the
   daemon starts, **Then** it runs using documented default values for the
   omitted settings.
4. **Given** no configuration file exists, **When** the daemon starts,
   **Then** it writes a documented default configuration to the expected
   location and keeps running (US1 flows from that point); if the file
   cannot be created (e.g. unwritable location), **Then** it exits with a
   message naming the expected path and does not monitor any device.

---

### Edge Cases

- What happens when the device the daemon is watching disappears (unplugged)
  while the daemon runs? The daemon must keep running and resume detection
  when a suitable device reappears.
- What happens when the daemon is already executing the command and another
  qualifying shake occurs?
  During the cooldown that follows a trigger, qualifying shakes are ignored,
  so at most one command run is in flight per cooldown window.
- What happens when the user shakes the device continuously for longer than
  the cooldown? Continuous shaking does not queue up multiple runs; the same
  single-run-per-window rule applies.
- What happens when no device matching the daemon's needs is present at
  startup? The daemon must not exit; it waits and starts detecting as soon as
  a suitable device appears.
- What happens when multiple input devices are present? The daemon monitors
  all devices that can report the motion data the detector needs, rather than
  requiring the user to name one specific device.
- What happens when the configured command is slow-running? The daemon keeps
  watching for input during execution and honors cooldown for subsequent
  triggers; it does not freeze detection while the command runs.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The daemon MUST run in the background as a lightweight daemon,
  watching device motion without any interactive user interface.
- **FR-002**: The daemon MUST read its configuration from a TOML file located
  at the shaker configuration directory under the system's configuration home
  (i.e., the path formed by appending `shaker/config.toml` to the user's
  configuration home directory).
- **FR-003**: The configuration MUST include a sensitivity setting defining
  how intense a motion must be to count toward a shake.
- **FR-004**: The configuration MUST include a duration setting defining how
  long the qualifying motion must be sustained to count as a shake.
- **FR-005**: The configuration MUST include a cooldown setting defining how
  long after a detected shake further shakes are ignored.
- **FR-006**: The configuration MUST include a command setting defining the
  command to execute when a shake is detected.
- **FR-007**: The daemon MUST execute the configured command when a motion
  that meets both the sensitivity and duration settings is detected.
- **FR-008**: The daemon MUST ignore qualifying motions that occur within the
  cooldown period following a previous trigger.
- **FR-009**: The daemon MUST treat a trigger plus its cooldown window as
  allowing at most one command execution; overlapping or repeated qualifying
  motions during that window MUST NOT result in extra executions.
- **FR-010**: On invalid configuration structure or values, the daemon MUST
  report the problem clearly and exit without monitoring devices.
- **FR-011**: On a configuration file missing at the expected location, the
  daemon MUST write a documented default configuration there (creating the
  needed parent directories) and continue with startup; if it cannot be
  written, the daemon MUST report the expected location clearly and exit
  without monitoring devices. An existing file that cannot be *read* MUST
  still be reported as an error, and a file that exists but omits the
  required `command` key remains an error (no safe command default).
- **FR-012**: The daemon MUST supply documented default values for any
  configuration settings that are omitted from an otherwise valid file.
- **FR-013**: The daemon MUST keep running when the monitored device is
  removed, and MUST resume detection when a suitable device becomes
  available again.
- **FR-014**: The daemon MUST NOT exit when no suitable device exists at
  startup; it MUST wait for one to appear.
- **FR-015**: The daemon MUST monitor every connected device capable of
  reporting the motion data the shake detector needs, rather than a single
  user-designated device.
  *Amendment (v0.2)*: an optional `device` setting (name/sysname substring)
  MAY narrow monitoring to a chosen subset; when unset the daemon MUST keep
  the FR-015 "monitor every capable device" behavior.
- **FR-016**: The daemon MUST continue watching for motion while the
  configured command is executing.
- **FR-017**: The daemon MUST respond to a user request to terminate (e.g.,
  the standard termination signal) by shutting down promptly and cleanly.
- **FR-018**: The installed footprint of the daemon MUST remain small
  ("lightweight"), consistent with a single small binary and minimal runtime
  dependencies.

### Key Entities *(include if feature involves data)*

- **Configuration**: A single file containing four settings — shake
  sensitivity, shake duration, shake cooldown, and the command to execute.
  Sensitivity, duration, and cooldown are numeric values; the command is free
  text to be run through the system's command facilities. All but the command
  have documented defaults; the command has no safe default, so a file
  without it is treated as incomplete (see FR-010/FR-011/FR-012).
- **Detected Shake**: A stretch of motion above the sensitivity threshold that
  lasts at least the configured duration; consuming it sets the cooldown
  window during which further qualifying motions are ignored.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A qualified shake leads to the configured command beginning
  execution within 250 ms of the moment the shake completes (lasts at least
  the configured duration).
- **SC-002**: Gentle motions that never exceed the configured sensitivity
  produce zero command executions in a 10-minute monitoring session.
- **SC-003**: With default settings omitted from a valid configuration file,
  the daemon starts successfully and a shake still triggers the command,
  confirming defaults are applied.
- **SC-004**: Device unplug/replug events collected over a test session cause
  zero missed triggers on reconnection after a device is re-attached (detection
  resumes within 2 seconds of a suitable device appearing).
- **SC-005**: The daemon process stays resident with constant memory usage
  during 1 hour of continuous monitoring (no growth in resident memory over
  the hour).
- **SC-006**: 100% of qualifying shakes occurring inside an active cooldown
  window are ignored (zero extra command executions).
- **SC-007**: New users can go from having no configuration to a working
  trigger setup in under 5 minutes, using only documentation and the error
  messages the daemon produces.

## Assumptions

- The primary input device is a motion-capable device (e.g., a phone-like
  accelerometer device or a pointing device that reports its own motion);
  the daemon relies on the device's own motion reporting rather than
  reconstructing motion from unrelated event streams.
- "Shake" means sustained vigorous back-and-forth motion; a single abrupt
  bump or tap is NOT a shake and should not trigger.
- Sensitivity is expressed relative to the device's own reported motion
  strength (not in absolute physical units), so it works across devices
  without per-device calibration.
- The configured command is run in a detached execution context so that a
  hanging command cannot stall detection (see SC-005 / FR-016).
- Environment: single user per machine; the daemon runs with the privileges
  needed to read motion events from input devices.
- Security: because the command string comes from a user-owned config file,
  it runs with the daemon's privileges by design; this is trusted user
  configuration, not untrusted input. This is v1, so no authentication,
  network, or remote-configuration features are in scope.
- The configuration directory under the config home may not exist yet; the
  daemon creates the needed directories when auto-initializing a missing
  config (v1 amendment), and clear error reporting guides the user when
  creation is not possible.
- Documentation will state the concrete config path for the target platform
  (typically `~/.config/shaker/config.toml`) and that a different config home
  environment setting changes the location accordingly.

## Dependencies

- Requires read access to system input device events (typically root or
  membership in the `input` group on the target platform).
- Requires the platform command facilities to launch the configured command.
