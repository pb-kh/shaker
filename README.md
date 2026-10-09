# shaker

A minimal C++23 [Meson](https://mesonbuild.com) daemon that watches pointer
devices on the seat via the [libinput](https://www.freedesktop.org/wiki/Library/libinput/)
udev backend, detects **shaking** (fast movement sustained for a configured
duration above a configured sensitivity), and runs a configurable command —
e.g. a notification — when it happens.

## Dependencies

- Meson ≥ 1.0 and Ninja
- A C++23-capable compiler (GCC 13+ or Clang 18+)
- [libinput](https://gitlab.freedesktop.org/libinput/libinput) ≥ 1.24
  (pkg-config module `libinput`, usually with `libudev`)
- For tests only: [doctest](https://github.com/doctest/doctest) — vendored
  automatically as a Meson WrapDB subproject

On Arch: `sudo pacman -S meson libinput systemd-libs` (udev is pulled in via
systemd).

## Building

```sh
meson setup build
meson compile -C build
meson test -C build
```

## Running

```sh
sudo ./build/shaker
```

The program uses the udev backend attached to `seat0`. Opening `/dev/input/*`
typically requires root or membership in the `input` group:

```sh
sudo gpasswd -a $USER input   # log out/in afterwards
```

Press Ctrl+C to quit (clean exit code 0).

## Configuration

The daemon reads a TOML file at `XDG_CONFIG_HOME/shaker/config.toml`
(defaulting to `~/.config/shaker/config.toml`). When the file does not
exist, shaker **creates it with defaults** (creating missing directories)
and runs — edit the generated `command` and restart to set the action.

```toml
# How far each swing leg must travel, in pointer units, to count as a
# direction change.
sensitivity = 40.0

# How long (seconds) the swing sequence may take to complete a shake.
duration = 0.25

# How long (seconds) to ignore shakes after one triggers.
cooldown = 2.0

# Executed through /bin/sh -c when a shake triggers (required key).
command = "notify-send 'Shake detected'"
```

A shake is recognized as **3 direction changes within the `duration`
window** (each swing leg traveling at least `sensitivity` pointer units) —
the physical back-and-forth motion, which keeps ordinary pointer use quiet.

- `sensitivity`, `duration` and `cooldown` fall back to these defaults when
  omitted; `command` is the only **required** key.
- Unknown keys are reported as warnings and ignored.
- A broken or unreadable config file produces a precise error and exit
  code 1.

## The command is a full shell script

`command` runs through `/bin/sh -c` with your environment and cwd
inherited — anything sh can do, your shake can do. Use TOML literal
strings (`'''…'''` multi-line or `'…'` single-line) so shell quoting
survives untouched:

```toml
# single-line: note the TOML '…' wrapper keeps the inner double quotes
command = 'grim -g "$(slurp)" "$HOME/Pictures/shake-$(date +%s).png"'
```

```toml
# multi-line script ('''…'''): everything is verbatim — quotes, $(), #, \
command = '''
shot="$HOME/Pictures/shake-$(date +%s).png"
grim -g "$(slurp)" "$shot"
wl-copy -t image/png < "$shot"
notify-send "Screenshot saved & copied: $shot"
'''
```

**Quoting pitfall** (why `command = "grim -g '$(slurp)'"` does nothing):
the single quotes are *shell-level* here — `sh -c` passes `$(slurp)`
literally to `grim` and it fails on the bogus geometry. Command
substitution needs double quotes at the shell level:
`grim -g "$(slurp)"`. In TOML terms: wrap the command in TOML single
quotes (`'…'` / `'''…'''`) and put shell double quotes where the shell
needs them.

If you use `"""…"""` (basic multi-line), TOML escape processing applies
(`\n` becomes a real newline, `\` at end-of-line folds lines) — for
shell text, prefer `'''`.

Run shaker as your own user (the `input` group membership covers device
access) so `WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR` reach Wayland clients like
`grim`/`slurp`; under `sudo` those variables are stripped and the
compositor is unreachable.

Calibrate your values with

```sh
./build/shaker --debug
```

which prints a per-sample trace (speed, swing count + detection state) on
stderr. See `specs/001-shake-trigger-framework/quickstart.md` for the full
walk-through.

## Background operation

shaker is a foreground process (observable on stdout). To keep it resident,
use e.g. `shaker &` or a systemd user service:

```ini
# ~/.config/systemd/user/shaker.service
[Unit]
Description=Shake-triggered command daemon
[Service]
ExecStart=/path/to/build/shaker
Restart=on-failure
[Install]
WantedBy=default.target
```

```sh
systemctl --user enable --now shaker
```

## Specification

Feature specs, design documents and task lists live under
`specs/001-shake-trigger-framework/` (constitution, plan, research,
data model, contracts, quickstart, tasks).
