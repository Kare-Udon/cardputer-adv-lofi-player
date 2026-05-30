# Project Map

## Scope

- Project root: `/Users/udon/Documents/code/self/M5Stack_Cardputer_Adv/lofi_cardputer`
- Parent workspace: `/Users/udon/Documents/code/self/M5Stack_Cardputer_Adv`
- Parent Cardputer skill: `.codex/skills/m5stack-cardputer-development/`

The parent directory is a multi-project Cardputer workspace. `lofi_cardputer/` is a child git repo. Do not treat parent-root rules as repo rules, and do not generalize Lo-Fi rules to every child project.

## Key Paths

- `main/`: ESP-IDF entry, board bring-up, LVGL/LCD UI, audio task, board glue.
- `src/`: playback core, storage, input, WAV parser, Lo-Fi DSP, AAC cache.
- `tests/`: host C++ tests. This path may be ignored by `.gitignore`; separate local evidence from tracked changes.
- `scripts/`: host preview, hardware smoke, framebuffer capture, acceptance evidence, asset/font conversion, release packaging.
- `assets/`: source icons, fonts, README media, project assets.
- `docs/`: current debug and verification docs.
- `build-host/`: local evidence output. Do not commit by default.

## Common Entry Points

- App: `main/app_main.cpp`
- Board/LCD/codec glue: `main/lofi_board.cpp`
- Pins: `main/board_pins.h`
- Playback core: `src/lofi_core.cpp`, `src/lofi_core.hpp`
- Storage: `src/lofi_storage.cpp`
- Input: `src/lofi_input.cpp`
- DSP: `src/lofi_dsp.cpp`
- AAC cache: `src/lofi_aac_cache.cpp`
- Full self-test: `scripts/run_full_self_test.sh`
- Hardware smoke: `scripts/run_hardware_smoke.py`

## Boundaries

- Run `git status --short` before edits.
- Keep changes task-scoped.
- Do not commit debug screenshots, temporary plans, or generated evidence unless requested.
- Confirm release scope and version before changing release files, README, translations, or webflash packages.
