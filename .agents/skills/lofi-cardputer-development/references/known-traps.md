# Known Traps

- **Build is not flash**: `idf.py build` creates binaries only. Device update needs flash/hash and board-side proof.
- **Host preview is not final UI**: use framebuffer for firmware pixels, and camera/user feedback for physical appearance.
- **Serial reopen can reset the board**: use `interactive-debug` for continuous UI work.
- **Patch context may be stale**: inspect current lines before patching active files such as `src/lofi_core.cpp`.
- **UI-only settings fixes are incomplete**: settings, sleep timer, volume confirmation, and Lo-Fi presets must update core state, persistence, restore, and playback loop.
- **List model and LVGL can diverge**: keep `ScreenModel.rows`, scroll, row height, and visible rows aligned.
- **Resources consume flash and RAM**: after CJK fonts, icons, codecs, or framebuffer changes, record binary size and free partition space.
- **Framebuffer shadow can break AAC**: allocate it only on demand and release it before playback or track changes.
- **Debug features can leak into release**: serial controls, sample library, framebuffer dump, and self-test media must stay behind debug automation.
- **Ignored tests can mislead review**: report actual commands run, and distinguish tracked code from ignored local tests.
- **Partial smoke is not final acceptance**: camera proof, listening confirmation, and readiness/handoff gates may still be blocked.
