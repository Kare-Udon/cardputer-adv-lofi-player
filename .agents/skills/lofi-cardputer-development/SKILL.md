---
name: lofi-cardputer-development
description: Develop, debug, test, or review the lofi_cardputer ESP-IDF firmware for M5Stack Cardputer Adv. Use for Lo-Fi player UI, LVGL pages, playback/core state, SD/media cache, audio/DSP/codecs, CJK/fonts/icons/RGB565 assets, debug firmware, serial automation, framebuffer screenshots, hardware smoke, release media, and verification in this child repo.
---

# lofi_cardputer Development

Use this skill for `/Users/udon/Documents/code/self/M5Stack_Cardputer_Adv/lofi_cardputer`.

This is project-specific. It does not replace the parent Cardputer hardware skill at `.codex/skills/m5stack-cardputer-development/`. For pins, buses, board bring-up, or new Cardputer projects, use the parent skill first.

## Default Flow

1. Discover skills from the parent workspace, but run project commands with `lofi_cardputer/` as `cwd`.
2. Run `git status --short` first. Do not overwrite dirty user work or release files.
3. Load only the 1-2 references needed for the task.
4. Confirm the real entry points before editing: `main/` owns app, board, UI, and audio task code; `src/` owns playback core, storage, input, and DSP; `scripts/` owns validation and evidence tools.
5. Report evidence precisely: host test, ESP-IDF build, flash, firmware framebuffer, hardware smoke, camera proof, or listening proof.

## Reference Selection

- Project layout and boundaries: [project-map.md](references/project-map.md)
- Validation commands and evidence rules: [verification.md](references/verification.md)
- LVGL, CJK fonts, icons, RGB565 assets, README media: [ui-assets.md](references/ui-assets.md)
- Playback state, settings, DSP, codecs, SD/cache, album art: [audio-playback-state.md](references/audio-playback-state.md)
- Debug firmware, serial automation, framebuffer dumps, hardware smoke: [serial-hardware-debug.md](references/serial-hardware-debug.md)
- Known failure patterns: [known-traps.md](references/known-traps.md)

## Core Rules

- Host preview is only a UI draft. UI completion needs firmware framebuffer, device logs, hardware smoke, camera proof, or user device feedback.
- A build is not a board update. Claim device update only after flash evidence plus board-side proof.
- For repeated screenshots, keys, and logs, use a long-lived serial session. Reopening `/dev/cu.usbmodem*` can reset ESP32-S3 USB Serial/JTAG.
- Prefer fixed-size prebuilt RGB565 C assets for icons, boot splash, and album-art thumbnails. Do not rely on LVGL runtime scaling or per-frame large-image decoding.
- Settings and playback controls must update core state, persistence, restore, UI, and playback loop together.
- Keep automation in debug firmware or compile-time gates. Release firmware should not expose serial automation, framebuffer dump, self-test media, or sample-library fallback.
- Request approval when hardware, flash, serial access, ESP-IDF build permissions, downloads, or `npx` asset tools require it.
