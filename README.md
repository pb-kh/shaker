# shaker

A minimal C++23 [Meson](https://mesonbuild.com) project that connects to
[libinput](https://www.freedesktop.org/wiki/Library/libinput/) via its udev
backend and prints input events (device add/remove, keyboard, pointer motion
and buttons) to stdout.

## Dependencies

- Meson ≥ 1.0 and Ninja
- A C++23-capable compiler (GCC 13+ or Clang 18+)
- [libinput](https://gitlab.freedesktop.org/libinput/libinput) ≥ 1.24
  (pkg-config module `libinput`, usually with `libudev`)

On Arch: `sudo pacman -S meson libinput systemd-libs` (udev is pulled in via systemd).

## Building

```sh
meson setup build
meson compile -C build
```

## Running

```sh
./build/shaker
```

The program uses the udev backend attached to `seat0`. Opening `/dev/input/*`
device nodes typically requires root or membership in the `input` group:

```sh
sudo ./build/shaker
```

Press Ctrl+C to quit.
