# UI And Assets

## Rules

- Use host preview for quick layout only.
- Prefer firmware framebuffer, `AUTO SNAP`/`UI` logs, hardware smoke, camera proof, or user feedback for UI decisions.
- Keep one verified LVGL display path. Do not keep old LCD fallback or temporary color-test routes as insurance.

## Pages And Lists

- For Library, Queue, and Lo-Fi lists, keep `ScreenModel.rows`, scroll correction, LVGL row height, and visible row count aligned.
- Cover deep-list last-row selection. This catches invisible-cursor regressions.
- Help/shortcut pages are firmware UI, not plain docs. Update hardware keys, serial actions, core state, and LVGL pages together.

## Fonts And CJK

- Verify font and mojibake fixes through firmware rendering.
- After adding CJK or bitmap fonts, prove target characters with real logs, framebuffer, or font validation scripts.
- Large fonts, multicolor icons, full CJK, and codecs consume app partition and RAM. Record `lofi_cardputer.bin` size and partition free space after resource changes.
- If free space nears single digits, subset fonts, remove unused codecs, externalize resources, or adjust partitions.

## Icons, Album Art, Boot Splash

- Convert source SVG/PNG into fixed-size RGB565 C assets offline.
- Generate center and side icon sizes separately. Check with roundtrip output or contact sheets before firmware inclusion.
- Multicolor icons should still use prebuilt fixed-size RGB565 assets.
- Boot splash is first-screen firmware behavior. Use a ScreenModel/LVGL path, prebuilt RGB565 asset, serial framebuffer dump action, host tests, build, diff check, flash hash, and real framebuffer proof.
- README/GIF frames should come from real firmware framebuffer or equivalent hardware evidence.

## Useful Scripts

```bash
python3 scripts/render_home_carousel_preview.py --validate --out-dir build-host/home-carousel-preview
python3 scripts/render_home_carousel_preview.py --validate-framebuffer build-host/home-carousel-preview/home_library_preview.png
python3 scripts/validate_ui_preview.py
python3 scripts/validate_ui_renderer_parity.py
python3 scripts/validate_ui_evidence_bundle.py
python3 scripts/validate_cjk_font.py
```
