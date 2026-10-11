# Contract: Runtime Behavior

**Feature**: `001-shake-trigger-framework`
**Applies to**: shaker v0.1.x (single binary `shaker`)
**Stability**: v1 contract — output line formats are text humans (and shell
scripts) may consume; changes require a spec.

This document defines the program's command-line surface, output channels,
exit codes, device handling, command execution semantics, and
resource-behavior guarantees.

---

## 1. Command-line interface

```console
$ shaker [FLAGS]

Flags:
  --debug      verbose detection trace on stderr (calibration mode)
  --help       print usage (stdout) and exit 0
  --version    print version (stdout) and exit 0
```

- Unknown flags: usage-error — `shaker: unrecognized flag '<flag>'` on
  stderr, usage line follows, **exit 2**.
- With no flags the daemon starts normally (configuration rules from
  `config-schema.md`).

## 2. Output channels (constitution VI)

### 2.1 stdout — events

Human-readable one-line events, flushed immediately:

| Event | Line format |
|-------|-------------|
| start | `shaker <version>: watching seat0 — pid <pid>` |
| device added | `[+] device added: <name> (<sysname>)` |
| device removed | `[-] device removed: <name>` |
| shake triggered | `shake! speed=<speed> units/s → running: <command>` (speed = instantaneous pointer speed; multi-line scripts show their first line + `…`) |
| shake while previous command still running | `shake! speed=<speed> units/s → skipped: previous command still running` |

### 2.2 stderr — diagnostics (errors + `--debug` trace)

Diagnostics never go to stdout. Non-fatal diagnostics keep the daemon
running; fatal ones precede exit 1 (config errors) or exit 2 (argument
errors).

| Condition | Line |
|-----------|------|
| config failure (see config-schema.md table) | `shaker: cannot read '<path>': ...` / `shaker: config parse error '<path>': ...` / `shaker: config '<path>': key "..." ...` |
| unknown config key (non-fatal warning) | `shaker: config '<path>': ignoring unknown key "<key>"` |
| libinput dispatch failure (fatal) | `shaker: libinput_dispatch failed: <strerror>` , exit 1 |
| event-timestamp regression (non-fatal) | `shaker: non-monotonic timestamps from device; sample dropped` |
| command spawn failure (non-fatal) | `shaker: spawn failed: <errno reason>` |
| child exec failure inside spawned shell | inherited appearance on the daemon's stderr: `sh: <command>: not found` (exit 127 path; status not tracked) |
| --debug per-sample trace (non-fatal, verbose) | `debug: t=+<elapsed>s speed=<speed> segments=<n> factor=<path/diagonal> state=<IDLE\|SUSTAIN\|COOLDOWN>` (+ `window=+<elapsed>s` while SUSTAIN) |
| --debug trigger trace | `debug: TRIGGER at t=+<elapsed>s peak=<peak>` |

`--debug` never changes *decisions* — only adds visibility (otherwise a
"debug triggers differently" defect).

## 3. Exit codes

| Code | When |
|------|------|
| 0 | clean shutdown on SIGINT / SIGTERM (FR-017) — one last `shaker: exiting on signal` info line on stderr |
| 1 | configuration unreadable / unparseable / invalid value; or fatal libinput error |
| 2 | command-line usage error |

## 4. Device handling (FR-013, FR-014, FR-015)

- **FR-015**: every pointer-capable device libinput exposes on seat0 is
  monitored automatically; no device naming appears in the config. Device
  add/remove produce stdout lines. libinput **gesture** or keyboard-only
  devices are ignored for detection.
- **FR-013**: on device removal the daemon keeps running (stdout line only);
  the detection state machine is *not* reset — sustained windows only
  exist while samples flow, so a removed device cannot hold a stale window.
- **FR-014**: with zero pointer devices at startup, the daemon waits (udev
  monitors the seat); it prints the start line and its first trigger occurs
  whenever devices appear and start moving.

## 5. Command execution semantics (FR-007, FR-008, FR-009, FR-016)

- Triggered exactly when the detection core emits a Trigger (bindings per
  data-model.md).
- Executed as `fork()` + `execvp("/bin/sh", ["sh", "-c", <command>])`,
  detached: the daemon never waits for the child and never queues commands
  (SIGCHLD is ignored → children auto-reaped, no zombie accumulation; SC-005).
- cwd, environment, stdin are inherited; stdout **inherited** (a command
  writing to the terminal where the daemon runs mixes its output —
  documented behavior; redirection belongs in the configured command
  string).
- Detection continues immediately after fork (SC-001): spawn cost ~1 ms,
  well under the 250 ms budget.
- Trigger rate cap = one execution per cooldown window (core-enforced;
  SC-006).
- Single instance: a trigger that fires while the previously spawned
  command (the `/bin/sh` process) is still alive is skipped, not queued.
  Liveness is observed through a pidfd (`pidfd_open`, Linux ≥ 5.3) polled
  non-blockingly at trigger time; reaping stays automatic. Commands that
  background their work (`cmd &`) count as finished once the shell exits.
- Spawn failure (fork shortage) is a stderr diagnostic; daemon continues.

## 6. Resource behavior (SC-005, FR-018)

- Idle CPU: blocking `poll` (libinput fd + signalfd) — no busy loop.
- Memory: no per-sample allocation; core state is a fixed struct; spawned
  children keep no bookkeeping in the parent.
- Signals: SIGINT/SIGTERM → clean exit (dump-free); other signals keep
  default disposition; SIGCHLD ignored as above; SIGHUP reserved for
  future reload (ignored in v1). On exit, libinput/udev releases all device
  handles (RAII teardown).

## 7. What is NOT in the v1 contract

- No reload-on-file-change (restart to change anything).
- No `--config PATH` flag (env override only; see R-03).
- No per-device sensitivity overrides, no device naming.
- No JSON/machine-readable output mode (text is the contract; structured
  modes are future specs).
- No built-in daemonization / `--daemonize` (service managers own that).
