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

Flat TOML document with five recognized top-level scalar keys (four in the
original v0.1 contract; `device` is an additive v0.2 amendment):

| Key | TOML type | Unit | Default (when omitted) | Constraint |
|-----|-----------|------|------------------------|------------|
| `sensitivity` | float (int accepted) | shake factor (path / trail diagonal) | `4.0` | > 0.0 (warning above 20) |
| `duration`    | float (int accepted) | seconds | `1.0` | > 0.0 |
| `cooldown`    | float (int accepted) | seconds | `2.0` | ≥ 0.0 |
| `device`      | string  | device name / sysname substring | `""` (empty = every pointer device on seat0) | matched case-insensitively as a substring of the libinput device **name** or **sysname** |
| `command`     | string  | shell command | **none — required** | non-empty after parse; executed via `/bin/sh -c` |

- Unknown top-level keys are **ignored** with a stderr warning line (does not
  fail the start).
- Key order is irrelevant. No `[section]` tables are part of the v1 contract:
  **parser scope** — the flat `key = value` form plus TOML strings:
  single-line `"…"` (escapes processed) and `'…'` (verbatim), multi-line
  `"""…"""` (escapes + EOL-backslash continuation) and `'''…'''`
  (verbatim, first newline trimmed) — numbers with `_` separators, `#`
  comments incl. mid-line; every unsupported TOML construct (tables,
  arrays, dotted keys) fails fast with a parse error naming the line
  (research R-01).
- Comments and TOML quoting/escaping follow the TOML 1.0 specification
  (enforced by the parser, not this document).

### Complete example

```toml
# Shake factor: how many times longer the pointer's path must be than
# the area it covered within `duration` (KDE Plasma default: 4).
sensitivity = 4.0

# How far back (seconds) the pointer's path is considered.
duration = 1.0

# How long (seconds) to ignore further shakes after one triggers.
cooldown = 2.0

# Optional: only watch devices whose name or sysname contains this string
# (case-insensitive). Omit for all pointer devices on the seat.
# device = "Logitech"

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
numeric keys and the optional string key `device` have documented defaults
and never fail by omission. This matches the spec: FR-012 (defaults for
omitted settings) applies to sensitivity/duration/cooldown; the spec's Key
Entities section explicitly gives `command` no safe default, so its omission
is treated as an invalid configuration (FR-010 path).

## 4. Semantics of each key (contract meaning)

- **sensitivity**: the shake factor (v3, KDE Plasma / PowerToys
  semantics). A shake triggers when the pointer path inside the `duration`
  window is more than `sensitivity` times the diagonal of its bounding
  box. Straight or curved pointing scores ≈ 1–2, one fast circle ≈ 2.2;
  each extra back-and-forth leg adds ≈ 1. Raise it if ordinary movement
  triggers, lower it (never below ~2.5) if shakes are missed. Internally,
  trails whose box is under 100 pointer units (tremor) never trigger, and
  motion with a pointer button held is ignored. Values above 20 load with
  a warning (likely a stale v2 pointer-unit setting).
- **duration**: how far back (seconds) the pointer path is considered.
  Longer windows accept slower shaking but also let more ordinary
  movement accumulate.
- **cooldown**: after a shake triggers, further shakes are ignored for
  this many seconds. `0.0` disables suppression. It applies in device
  event time.
- **device** (optional, additive v0.2 amendment): restricts detection to
  pointer devices whose libinput **name** or **sysname** contains the given
  string, compared case-insensitively. Matching is a plain substring test,
  so `"Surface"` matches `Microsoft Surface 045E:09AF Touchpad` and
  `"event19"` matches the sysname. Omitted or empty means *every*
  pointer-capable device on seat0 is watched (FR-015 default behavior). Use
  `shaker --list-devices` to see the exact names/sysnames. The filter only
  gates which devices feed the detector; button state is still tracked
  seat-wide, so dragging on any device still suppresses detection.
- **command**: a **full shell script** executed by `/bin/sh -c` with cwd
  and environment inherited from the daemon (otter-launcher-style
  `exec_cmd = "sh -c"` semantics: pipes, subshells, `;`-sequences,
  variable expansion, multiple lines via `'''…'''` all work), every time
  a shake triggers — at most once per cooldown window. A failing script
  does not stop the daemon (exit status of the child is not tracked in
  v1; spawn failure and script output surface on the daemon's inherited
  stderr/stdout). Quoting: wrap the command in a TOML literal string
  (`'…'` / `'''…'''`) so shell-level `"` and `$()` reach the shell
  untouched; selects like `grim -g "$(slurp)"` need the double quotes at
  the *shell* level.
