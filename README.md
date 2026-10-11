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

`meson install` also installs the udev rule `99-shaker-mouse-access.rules`
(mouse event devices get mode `0600`) into `/etc/udev/rules.d`. Override the
destination with `-Dudev_rules_dir=/usr/lib/udev/rules.d` at setup time.

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
# Shake factor: how many times longer the pointer's path must be than
# the area it covered within `duration` (KDE Plasma default: 4).
sensitivity = 4.0

# How far back (seconds) the pointer's path is considered.
duration = 1.0

# How long (seconds) to ignore shakes after one triggers.
cooldown = 2.0

# Executed through /bin/sh -c when a shake triggers (required key).
command = "notify-send 'Shake detected'"
```

A shake is recognized the way KDE Plasma's *Shake Cursor* and PowerToys'
*Find My Mouse* do it: within the last `duration` seconds, the pointer's
path must be more than `sensitivity` times longer than the diagonal of
the box it stayed in (and that box must be at least 100 pointer units).
Ordinary pointing — fast, curved, or with overshoot corrections — covers
about as much ground as its path (factor ≈ 1–2); only repeated
back-and-forth over the same spot piles up path. Motion while a mouse
button is held (dragging, selecting) is ignored.

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

which prints a per-sample trace (speed, trail segments, shake factor + detection state) on
stderr. See `specs/001-shake-trigger-framework/quickstart.md` for the full
walk-through.

## Background operation

shaker is a foreground process (observable on stdout). Start it from your
compositor's autostart: it then inherits the session environment
(`WAYLAND_DISPLAY`, session D-Bus) that the configured command needs, and
lives exactly as long as the session. Your user must be in the `input`
group (`sudo gpasswd -a $USER input`, then log in again).

niri (`~/.config/niri/config.kdl`):

```kdl
spawn-at-startup "/path/to/build/shaker"
```

sway / Hyprland equivalents: `exec /path/to/build/shaker` /
`exec-once = /path/to/build/shaker`.

## Specification

Feature specs, design documents and task lists live under
`specs/001-shake-trigger-framework/` (constitution, plan, research,
data model, contracts, quickstart, tasks).
