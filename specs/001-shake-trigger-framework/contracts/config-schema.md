# Contract: Configuration File

**Feature**: `001-shake-trigger-framework`
**Applies to**: shaker v0.1.x
**Stability**: v1 contract — additive changes only in future versions;
renames/removals require a new specification.

This document defines the complete user-facing contract of the shaker TOML
configuration file. The daemon reads exactly this file once at startup.

---

## 1. Location

The configuration file path is resolved at startup as:

```
$XDG_CONFIG_HOME/shaker/config.toml
```

Resolution rules (implementation of the freedesktop basedir convention):

1. Let `$XDG_CONFIG_HOME` be the environment variable value.
2. If `$XDG_CONFIG_HOME` is unset, **empty**, or not an absolute path, use
   `$HOME/.config` instead.
3. Append `shaker/config.toml`.
4. The resolved absolute path is displayed verbatim in every configuration
   error.

**Auto-initialization**: when the file does not exist, shaker writes a
documented default configuration (the §2 example values) — creating missing
parent directories — prints an informational stderr line, and keeps
running (edit the generated `command` once, restart afterwards; there is no
hot reload). If the file or its parent directory cannot be created, shaker
exits with a `cannot write default config '<path>': …` error instead. An
*existing* file that cannot be read (e.g. permissions) keeps failing with
`cannot read '<path>': …` and auto-initialization never overwrites an
existing file.

Examples:
- default: `~/.config/shaker/config.toml`
- overriden for tests/portability:
  `XDG_CONFIG_HOME=/tmp/opencode/shaker-test ./build/shaker`
  → reads `/tmp/opencode/shaker-test/shaker/config.toml`

## 2. File format and keys

Flat TOML document with four recognized top-level scalar keys:

| Key | TOML type | Unit | Default (when omitted) | Constraint |
|-----|-----------|------|------------------------|------------|
| `sensitivity` | float (int accepted) | pointer units per swing leg | `40.0` | > 0.0 |
| `duration`    | float (int accepted) | seconds | `0.25` | > 0.0 |
| `cooldown`    | float (int accepted) | seconds | `2.0` | ≥ 0.0 |
| `command`     | string  | shell command | **none — required** | non-empty after parse; executed via `/bin/sh -c` |

- Unknown top-level keys are **ignored** with a stderr warning line (does not
  fail the start).
- Key order is irrelevant. No `[section]` tables are part of the v1 contract:
  **parser scope** — only the flat `key = value` form above is accepted
  (documented literal keys, numbers with `_` separators, quoted strings with
  the common `\`-escapes, `#` comments incl. mid-line); every unsupported
  TOML construct (tables, arrays, multi-line strings, dotted keys) fails
  fast with a parse error naming the line (research R-01).
- Comments and TOML quoting/escaping follow the TOML 1.0 specification
  (enforced by the parser, not this document).

### Complete example

```toml
# How far each swing leg must travel, in pointer units, to count as a
# direction change.
sensitivity = 40.0

# How long (seconds) the swing sequence may take to complete a shake.
duration = 0.25

# How long (seconds) to ignore further shakes after one triggers.
cooldown = 2.0

# Run through /bin/sh -c when a shake triggers.
command = "notify-send 'Shake detected'"
```

### Minimal example (defaults in play)

```toml
command = "notify-send 'Shake detected'"
```

## 3. Startup behavior on invalid input

Every configuration failure prints a single clear line on **stderr**, naming
the resolved file path, and the daemon exits **without monitoring devices**
(exit code 1). Categories, first match wins:

- *A file that simply does not exist never reaches this ladder* — it is
  auto-initialized with defaults (§1) and startup continues.
- Unknown top-level keys print a non-fatal warning line
  `shaker: config '<path>': ignoring unknown key "<key>"` (§2) and startup
  continues.

| # | Condition | stderr shape |
|---|-----------|--------------|
| 1 | existing file unreadable (permissions etc.) | `shaker: cannot read '<path>': <errno reason>` |
| 1b | missing file that cannot be auto-created | `shaker: cannot write default config '<path>': <errno reason>` |
| 2 | not valid TOML / syntax error | `shaker: config parse error '<path>': <parser message>` |
| 3 | `command` missing or empty after parse | `shaker: config '<path>': key "command" is required` |
| 4 | `sensitivity` ≤ 0 | `shaker: config '<path>': key "sensitivity" must be > 0 (got <value>)` |
| 5 | `duration` ≤ 0 | `shaker: config '<path>': key "duration" must be > 0 (got <value>)` |
| 6 | `cooldown` < 0 | `shaker: config '<path>': key "cooldown" must be >= 0 (got <value>)` |
| 7 | wrong TOML type for a known key (e.g. `duracion = "fast"`) | `shaker: config '<path>': key "<key>" has wrong type: expected <type>, got <name>` |

Rule 3 makes `command` the only key whose omission is fatal — the three
numeric keys have documented defaults and never fail by omission. This
matches the spec: FR-012 (defaults for omitted settings) applies to
sensitivity/duration/cooldown; the spec's Key Entities section explicitly
gives `command` no safe default, so its omission is treated as an invalid
configuration (FR-010 path).

## 4. Semantics of each key (contract meaning)

- **sensitivity**: minimum displacement of each swing leg (pointer units)
  that counts as a direction change. A shake = 3 swings (internal constant)
  inside the duration window, so this gates *how far the device must move
  back and forth* — raise it for bigger, more deliberate shakes; lower it
  if gentle shakes are missed. Ordinary cursor wandering rarely reverses
  direction 3× inside the window, so it stays quiet.
- **duration**: how much time the swing sequence may take (from the first
  swing) to complete before the run is abandoned. Raise for slower,
  lazier shaking sessions; keep short for snappy response.
- **cooldown**: after a shake triggers, further shakes are ignored for
  this many seconds. `0.0` disables suppression. It applies in device
  event time.
- **command**: executed by `/bin/sh -c` with cwd and environment inherited
  from the daemon, every time a shake triggers — at most once per cooldown
  window. A failing command does not stop the daemon (exit status of the
  child is not tracked in v1; spawn failure is reported on stderr).
