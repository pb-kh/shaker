T017 validation record — quickstart.md §5 outcomes:
- US1 rows: LIVE seat0 run: 8 devices enumerated, real trigger fired,
  spawn executed (marker file created), post-trigger re-trigger held off
  inside the cooldown window, clean SIGTERM on signal.
- US3 rows: missing file → exit 1 + path-bearing message; duration=-1 →
  exit 1 + message; unknown flag → exit 2; --help/--version exit 0.
- US3-3/SC-003: partial config (only command) starts and runs; defaults
  verified in unit suite.
- SIGINT/SIGTERM: exit 0 + 'exiting on signal' stderr line.
- Build gate: from-scratch rebuild has ZERO warnings; C++23 fuse passes.
- Test gate: meson test fully green (25 detection-core assertions +
  44 config assertions).
- FR-016/SC-005: spawn is detached; no detection stall possible.
- Manual calibration row (--debug ritual on sustained physical input):
  deferred to the user's interactive session (head bench limitation);
  trace emission verified code-level.
