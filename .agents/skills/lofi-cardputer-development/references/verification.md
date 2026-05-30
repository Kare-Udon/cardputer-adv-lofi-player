# Verification

Run commands from the `lofi_cardputer/` root.

## Default Levels

```bash
./scripts/run_host_tests.sh
./scripts/run_full_self_test.sh
```

For firmware changes, also build:

```bash
idf.py build
```

If the goal is device update, flash and report it separately:

```bash
idf.py -p /dev/cu.usbmodem1101 -b 115200 flash
```

Useful flash proof: `Chip is ESP32-S3`, app partition write, `Hash of data verified`, and `Hard resetting via RTS pin... Done`.

## Match Checks To Changes

- UI, icons, colors, fonts, carousel, animation: host preview first, then firmware framebuffer or hardware smoke.
- Library, Queue, Lo-Fi lists: cover last-row selection, `ScreenModel.rows`, scroll correction, LVGL row height, and visible row count.
- Settings, sleep timer, screen off, volume, repeat/shuffle: verify core state, persistence, restore, UI, playback loop, host tests, build, and real flash.
- Lo-Fi preset/DSP: verify canonical parameters, nonzero intensity, DSP metrics, real logs, or framebuffer.
- SD/media cache/AAC cache/album art: verify built/skipped logs, `LIBRARY_SYNC`, no crash/reboot, and decoder/heap logs during playback.
- README/GIF/release media: source frames should come from device framebuffer or equivalent hardware evidence.

## Evidence Boundaries

- Host preview is not firmware pixels.
- Firmware framebuffer proves the firmware LCD drawing path for 240x135 pixels. It does not prove panel brightness, viewing angle, or optical color.
- `AUTO SNAP` proves screen-model state, not pixels.
- Hardware smoke proves serial/device behavior, not camera screen proof or subjective listening.
- If goal readiness or handoff is blocked, do not call final acceptance complete.

## Hardware Smoke

Common full path:

```bash
./scripts/run_full_self_test.sh --hardware --flash-no-stub --flash-baud 57600 --port /dev/cu.usbmodem1101 --exercise-controls --exercise-ui-pages --exercise-live-lofi-toggle --exercise-pause-resume
```

Refresh smoke logs only:

```bash
/Users/udon/.espressif/python_env/idf5.4_py3.14_env/bin/python scripts/run_hardware_smoke.py --port /dev/cu.usbmodem1101 --exercise-controls --exercise-ui-pages --exercise-live-lofi-toggle --exercise-pause-resume --quiet --log-out build-host/hardware-smoke/latest_no_flash_ui_lofi_pause.log
```
