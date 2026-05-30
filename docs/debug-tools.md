# Lo-Fi Cardputer Debug Tools Guide

This document summarizes the debug, automation, screenshot, and hardware-validation tools that exist in the current firmware and local tooling. It describes real code paths in the current tree, not planned interfaces.

## Build Modes

Debug tools are controlled by `CONFIG_LOFI_DEBUG_AUTOMATION`.

Build the normal release-style firmware with the release defaults:

```bash
idf.py -B build-release \
  -DSDKCONFIG=build-release/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.release.defaults" \
  set-target esp32s3 build
idf.py -B build-release -p PORT flash monitor
```

Build the debug firmware to enable serial controls, status logs, screenshots, and self-test tools:

```bash
idf.py -B build-debug \
  -DSDKCONFIG=build-debug/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.debug.defaults" \
  set-target esp32s3 build
idf.py -B build-debug -p PORT flash monitor
```

Related configuration files:

- `sdkconfig.defaults`: release-oriented defaults, `WARN` logging, console disabled.
- `sdkconfig.release.defaults`: explicitly disables `CONFIG_LOFI_DEBUG_AUTOMATION`.
- `sdkconfig.debug.defaults`: enables `CONFIG_LOFI_DEBUG_AUTOMATION`, USB Serial/JTAG console, and `INFO` logging.
- `main/Kconfig.projbuild`: defines `CONFIG_LOFI_DEBUG_AUTOMATION`.
- `src/lofi_build_config.hpp`: maps the ESP-IDF Kconfig option to the C++ `LOFI_DEBUG_AUTOMATION_ENABLED` macro.

## Tool Inventory

| Category | Entry Point | Output Or Effect | Use |
| --- | --- | --- | --- |
| Serial virtual keys | USB Serial/JTAG input characters | Uses the normal `lofi::Action` state machine | Automated navigation and UI reproduction |
| Framebuffer screenshot | Serial input `d` | `FB_DUMP_BEGIN/DATA/END` | Capture firmware-rendered pixels |
| Boot splash screenshot | Serial input `v` | `FB_DUMP_BEGIN/DATA/END` | Inspect boot splash rendering |
| Logical screen snapshot | Automatic on selected redraws | `AUTO SNAP ...` and `UI ...` lines | Confirm page, state, rows, and soft keys |
| Runtime status log | Boot and roughly every 5 seconds | `LOFI_STATUS ...` | Observe UI, library, keyboard, audio, and save state |
| Media format counts | Library summary and status log path | `MEDIA_FORMATS ...` | Check scanned mp3/wav/aac/m4a counts |
| I2C probe summary | Once after startup | `I2C_PROBE ...` | Check whether keyboard, IMU, and audio codec addresses respond |
| WAV self-test | Serial input `t` | Writes and plays `SELFTEST.WAV` | Validate SD write, WAV decode, and I2S playback startup |
| MP3 self-test | Serial input `y` | Writes and plays `SELFTEST.MP3` | Validate SD write, MP3 decode, and I2S playback startup |
| Sample library fallback | Automatic when no SD music is found | UI shows sample tracks | Allow UI smoke checks without a populated music card |
| Host debug script | `cardputer_debug.py` | Logs, PNGs, navigation macros | Wrap firmware debug endpoints in repeatable commands |

## Firmware Serial Console

The debug firmware configures USB Serial/JTAG as nonblocking input and prints:

```text
SERIAL_INPUT ready enter=ok space=play n=next p=prev s=shuffle r=repeat l=lofi m=menu b=back h=home g=scan [=left ]=right <=seek_back >=seek_forward d=frame_dump v=boot_splash_dump t=wav_selftest y=mp3_selftest
```

Supported character commands:

| Character | Action |
| --- | --- |
| `Enter` / `\n` | Confirm / open |
| `Space` | Play / pause |
| `n` | Next track |
| `p` | Previous track |
| `s` | Shuffle |
| `r` | Repeat |
| `l` | Lo-Fi |
| `m` | Menu |
| `b` | Back |
| `h` | Home |
| `?` | Help |
| `g` | Scan |
| `[` or `,` | Left |
| `]` or `.` | Right |
| `<` | Seek backward |
| `>` | Seek forward |
| `+` or `=` | Up / volume up |
| `-` or `_` | Down / volume down |
| `d` | Dump the current framebuffer |
| `v` | Draw the boot splash, then dump the framebuffer |
| `t` | Write and play the WAV self-test file |
| `y` | Write and play the MP3 self-test file |

Virtual keys feed the same `lofi::Action` path as the physical keyboard, so they are suitable for reproducing user flows without bypassing the UI state machine.

## Framebuffer Screenshots

After serial input `d`, the debug firmware prints the current LCD shadow framebuffer:

```text
FB_DUMP_BEGIN seq=1 width=240 height=135 format=rgb565_lcd_word bytes=64800 hash=0x...
FB_DUMP_DATA seq=1 y=0 x=0 n=32 hex=...
FB_DUMP_END seq=1 hash=0x...
```

Properties:

- The data comes from the firmware LCD drawing path, not a host preview.
- It can validate layout, text placement, icon rendering, and firmware-side pixels.
- It does not prove real panel brightness, viewing angle, physical color appearance, or optical readability.
- If playback is active, normal screenshot capture disables framebuffer capture afterward to avoid maintaining the shadow framebuffer indefinitely.

Serial input `v` first draws the boot splash and then emits the same framebuffer dump format. Use it to inspect startup-screen rendering.

## Logical Screen Snapshots

The debug firmware emits a structured summary after selected redraws:

```text
AUTO SNAP rev=3 hash=0x... page="Lo-Fi" status="..." rows=5 cover=0 bg=0 bg_frame=0 volume_overlay=-1 mode_overlay="" soft="Back|Apply|Save"
```

It also emits more human-readable `UI ...` lines generated by `screen_to_lines()`.

Use this for:

- Checking the current page title, status, row count, soft keys, and overlays.
- Helping scripts decide whether `show` or `goto` reached the expected page.
- Cross-checking against framebuffer PNGs: `AUTO SNAP` proves state, while `FB_DUMP` proves pixels.

## Runtime Status Logs

The debug firmware prints a status line at startup and then roughly every 5 seconds:

```text
LOFI_STATUS tick=... page=... nav=... tracks=... albums=... artists=... sd=... playing=... current=... pos=... queue=... state=... kbd=... audio=... audio_pos=... msg=...
```

Covered fields:

- UI: current page and navigation stack depth.
- Library: tracks, albums, artists, and queue size.
- SD and state persistence: `sd` and `state`.
- Keyboard: readiness, input task, queue depth, event counts, event age, dropped counts, init stage, and error codes.
- Audio: playback status, position, and latest message.

`MEDIA_FORMATS` is printed around library summaries and status logging. Use it to confirm scanned media format counts.

## Hardware Probe And Audio Self-Tests

After startup, the debug firmware prints one I2C probe summary:

```text
I2C_PROBE tca8418=ESP_OK bmi270=ESP_OK es8311=ESP_OK
```

This only proves that the device addresses respond. It is not a full functional test.

There are two serial audio self-test commands:

- `t`: writes `/sdcard/Music/LOFI/SELFTEST.WAV`, then starts the WAV playback path at muted volume.
- `y`: writes `/sdcard/Music/LOFI/SELFTEST.MP3`, then starts the MP3 playback path at muted volume.

Related logs:

```text
WAV_SELFTEST_WRITE result=ok path=/sdcard/Music/LOFI/SELFTEST.WAV bytes=...
WAV_SELFTEST result=ESP_OK path=/sdcard/Music/LOFI/SELFTEST.WAV volume=0
MP3_SELFTEST_WRITE result=ok path=/sdcard/Music/LOFI/SELFTEST.MP3 bytes=...
MP3_SELFTEST result=ESP_OK path=/sdcard/Music/LOFI/SELFTEST.MP3 volume=0
```

These tests primarily validate SD writes, decoder entry points, the audio task, and I2S playback startup. The current implementation uses `volume=0`, so it is better suited for automation-path validation than subjective listening tests.

## Sample Library Fallback

When debug automation is enabled and no SD music is found, the firmware inserts an internal sample path list so the UI can enter a populated-library state for smoke testing.

This fallback only exists in the debug firmware. Release firmware does not include these sample paths and does not fake a library when no music is found.

## Host Debug Script

The workspace provides the host-side helper script:

```text
../.codex/skills/m5stack-cardputer-development/scripts/cardputer_debug.py
```

Common commands:

```bash
DEBUG_SCRIPT="../.codex/skills/m5stack-cardputer-development/scripts/cardputer_debug.py"
python3 "$DEBUG_SCRIPT" preflight --port PORT --timeout 8 --out build-host/cardputer-debug/preflight.log
python3 "$DEBUG_SCRIPT" read-log --port PORT --seconds 8 --out build-host/cardputer-debug/latest.log
python3 "$DEBUG_SCRIPT" send-keys --port PORT --keys "menu,lofi,enter,back"
python3 "$DEBUG_SCRIPT" frame-capture --port PORT --out build-host/cardputer-debug/frame.png --scale-out build-host/cardputer-debug/frame@4x.png
python3 "$DEBUG_SCRIPT" interactive-debug --port PORT --out-dir build-host/cardputer-debug/live
```

Script capabilities:

- `preflight`: reads serial briefly and requires one of `LOFI_STATUS`, `SERIAL_INPUT`, `MEDIA_FORMATS`, `AUTO SNAP`, or `FB_DUMP_BEGIN`.
- `read-log`: captures serial logs for a fixed duration.
- `send-keys`: encodes aliases such as `menu,lofi,enter` into firmware serial characters.
- `frame-capture`: requests `d`, parses `FB_DUMP_*`, and writes a PNG plus the raw log.
- `interactive-frame`: keeps serial open and captures repeated screenshots on demand.
- `interactive-debug`: keeps one long-lived serial connection and supports `capture`, `key`, `goto`, `show`, `graph`, `log`, and `preflight`.
- `parse-frame-log`: converts an existing `FB_DUMP_*` log to PNG offline.
- `self-test`: tests the script's framebuffer parser, color decoder, and key alias mapping.

On Cardputer Adv, opening `/dev/cu.usbmodem*` can reset ESP32-S3 USB Serial/JTAG. Prefer `interactive-debug` for continuous UI work so the port is not repeatedly reopened.

## Release Stripping Checks

Use three checks to confirm that release firmware does not retain debug tools.

Build configuration:

```bash
rg -n "CONFIG_LOFI_DEBUG_AUTOMATION|CONFIG_ESP_CONSOLE_NONE|CONFIG_LOG_DEFAULT_LEVEL" build-release/sdkconfig
```

Expected:

```text
# CONFIG_LOFI_DEBUG_AUTOMATION is not set
CONFIG_ESP_CONSOLE_NONE=y
CONFIG_LOG_DEFAULT_LEVEL_WARN=y
```

Binary strings:

```bash
rg -a -n "SERIAL_INPUT|FB_DUMP|AUTO SNAP|LOFI_STATUS|MEDIA_FORMATS|SELFTEST|FRAMEBUFFER_CAPTURE" build-release/lofi_cardputer.bin
```

Expected: no output.

Runtime negative check:

```bash
DEBUG_SCRIPT="../.codex/skills/m5stack-cardputer-development/scripts/cardputer_debug.py"
python3 "$DEBUG_SCRIPT" read-log --port PORT --seconds 5 --out build-host/cardputer-debug/release-read-log.log
python3 "$DEBUG_SCRIPT" frame-capture --port PORT --timeout 5 --out build-host/cardputer-debug/release-frame.png
```

Expected:

- Release logs do not contain `SERIAL_INPUT`, `AUTO SNAP`, `LOFI_STATUS`, `MEDIA_FORMATS`, or `SELFTEST`.
- `frame-capture` times out because release firmware has no framebuffer dump endpoint.

## Code Entry Points

Main implementation locations:

- `main/Kconfig.projbuild`: debug automation switch.
- `src/lofi_build_config.hpp`: C++ compile-time switch macro.
- `main/app_main.cpp`: serial input, status logs, I2C probe, self-tests, and logical screen logs.
- `main/lofi_board.cpp`: shadow framebuffer, framebuffer dump, and keyboard diagnostic snapshot.
- `src/lofi_core.cpp`: `screen_to_lines()`, `screen_hash()`, and `screen_auto_snapshot()`.
- `.codex/skills/m5stack-cardputer-development/scripts/cardputer_debug.py`: host-side automation script.
