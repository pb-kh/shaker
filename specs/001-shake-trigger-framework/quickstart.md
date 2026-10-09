# Quickstart: Shake-Triggered Command Daemon

**Feature**: `001-shake-trigger-framework`
Goal: from a fresh checkout to a working shake-triggered command in under 5
minutes (spec SC-007), then validate every acceptance scenario.

---

## 1. Build

```sh
meson setup build          # vendors the doctest subproject on first run
meson compile -C build     # zero warnings is a release gate
meson test -C build        # pure detection-core + config unit tests pass
```

Internet access is needed the first time only (WrapDB tarball for
`doctest`). Offline: run `meson wrap install doctest` once on a connected
machine and copy `subprojects/` over.

## 2. Configure

Skip this step entirely if you like: running shaker without a config file
**auto-creates the documented defaults** at
`~/.config/shaker/config.toml` and just runs (contract §1). If you want
explicit control:

```sh
mkdir -p ~/.config/shaker
cat > ~/.config/shaker/config.toml <<'EOF'
sensitivity = 40.0     # pointer units per swing leg — expect to tune this
duration    = 0.25     # seconds the swing sequence may take
cooldown    = 2.0      # seconds between allowed triggers
command     = "notify-send 'Shake detected'"
EOF
```

Rules recap (contracts/config-schema.md): `command` is the only required key;
the three numbers have defaults and must satisfy
`sensitivity > 0`, `duration > 0`, `cooldown ≥ 0`.

## 3. Run (and calibrate)

```sh
./build/shaker --debug
```

- Start line on stdout; calibration trace on stderr
  (`debug: t=+…s speed=… swings=… state=…`).
- **Calibration ritual**: while watching the trace —
  (1) idle baseline: `swings=0`, state IDLE;
  (2) ordinary pointer use: direction changes are rare — `swings` stays
      below 3 per window → no trigger;
  (3) actual shake: `swings` reaches 3 inside the window → trigger.
      Raise `sensitivity` if ordinary wiggles ever complete a window;
      lower it if your shakes are missed; raise `duration` for slow,
      lazy shaking styles.
- Membership in the `input` group avoids sudo (`sudo gpasswd -a $USER input`),
  which is the friction-free way to run it in a user service (below).

## 4. Background running (recommended)

Foreground is the contract (constitution VI); backgrounding is your choice:

```console
$ shaker &            # classic
```

or a systemd user unit readable from README's suggested snippet:

```ini
# ~/.config/systemd/user/shaker.service
[Unit]
Description=Shake-triggered command daemon
[Service]
ExecStart=%h/projects/shaker/build/shaker
Restart=on-failure
[Install]
WantedBy=default.target
```

```sh
systemctl --user enable --now shaker
```

## 5. Acceptance-scenario validation

| Spec scenario | Steps | Expected |
|---------------|-------|----------|
| US1-1 below-sensitivity | move gently 10 min with defaults; watch `--debug` | zero `shake!` lines (SC-002 analog for defaults) |
| US1-2 full qualifying shake | shake ≥ `duration` above threshold | one `shake!` line on stdout; command runs within 0.25 s (SC-001) |
| US1-3 failing command | `command = "false"` in config; shake twice | two `spawned` runs (one per window); daemon never exits |
| US2-1 sensitivity tuning | halve sensitivity; milder shake | now triggers (wasn't) |
| US2-2 cooldown | cooldown = 5.0; shake at +1s, +5.1s, +6s | only +5.1s+ triggers (continuous shakes suppressed — G-3) |
| US2-3 duration | swings taking longer than `duration` to complete a window | no trigger (G-2) |
| US2-4 straight drag | fast one-way cursor movement (no reversals) | no trigger (G-6) |
| US3-1 missing file | `rm` the config file (`config.toml.bak` backup kept) | file auto-created with defaults + info stderr note; daemon runs; nothing is overwritten if user config exists. Unwritable location → `cannot write default config` + exit 1 |
| US3-2 broken config | `duration = -1` | stderr: `key "duration" must be > 0`; exit 1 |
| US3-3 partial config | remove numbers, keep command | starts; defaults shown in `--help`/docs; trigger works (SC-003) |
| FR-013/14 | unplug/replug device (`[−]`,`[+]` lines) | detection resumes ≤ 2 s (SC-004); waits at startup when absent |
| FR-017 | Ctrl+C | exit code 0, `exiting on signal` line |

## 6. Unit-testable core (for developers)

The detection state machine under `include/shaker/detection.hpp` consumes
only `(t, dx, dy)` samples — see `tests/test_detection.cpp` scenarios
mirroring G-1…G-5 (no device needed):

```sh
meson test -C build test_detection test_config -v
```
