# Serial And Hardware Debug

## Debug Vs Release

Debug tools are gated by `CONFIG_LOFI_DEBUG_AUTOMATION`.

- `sdkconfig.release.defaults`: release-style firmware. Disables serial automation, framebuffer dump, self-test media, sample-library fallback, and verbose debug logs.
- `sdkconfig.debug.defaults`: debug firmware. Enables USB Serial/JTAG console, `AUTO SNAP`, `LOFI_STATUS`, framebuffer dump, and WAV/MP3 self-tests.
- `main/Kconfig.projbuild` defines the option.
- `src/lofi_build_config.hpp` maps it to C++.

When changing debug tools, ensure they do not leak into release firmware.

## Serial Actions

Debug serial input uses the normal `lofi::Action` path:

- `Enter`: confirm/open
- `Space`: play/pause
- `m`: menu
- `b`: back
- `h`: home
- `g`: scan
- `l`: Lo-Fi
- `n`/`p`: next/previous
- `[`/`]`: left/right
- `<`/`>`: seek
- `+`/`-`: up/down or volume
- `d`: current framebuffer dump
- `v`: boot splash framebuffer dump
- `t`/`y`: WAV/MP3 self-test

`AUTO SNAP` proves screen-model state. `FB_DUMP_*` proves firmware LCD pixels.

## Long-Lived Session

Reopening `/dev/cu.usbmodem*` can reset ESP32-S3 USB Serial/JTAG. For repeated capture, keys, show, or logs, use:

```bash
python3 ../.codex/skills/m5stack-cardputer-development/scripts/cardputer_debug.py interactive-debug --port /dev/cu.usbmodem1101 --out-dir build-host/cardputer-debug/live
```

Inside the session:

```text
preflight 5
graph
goto now-playing
capture now-playing
log 2 after-now-playing
quit
```

Do not navigate with one one-shot command and capture with another; the second open may reset the board.

## When Hardware Stalls

- If there are no app logs, frame capture times out, or `esptool read_mac` fails, check cable, physical reset, and USB enumeration before UI code.
- If flash stub fails mid-write, try no-stub and lower baud, such as `--flash-no-stub --flash-baud 57600`.
- Request permission for serial, flash, hardware smoke, ESP-IDF build process inspection, or system-level access when needed.
