# Playback, Audio, And Cache

## State Model

- Settings and playback controls must update `PlaybackState`, serialization, parsing, restore, Settings UI, and playback loop together.
- Do not intercept behavior only in one page or overlay.
- Restore user settings independently from queue restore. Even with `queue=0`, restore volume, brightness, repeat, shuffle, screen off, and Lo-Fi parameters.
- Sleep timer style features should reuse existing pause/stop paths and avoid long-running high-frequency tasks.

## Lo-Fi DSP

- Derive Lo-Fi UI state from canonical playback parameters, not preset names.
- Rebuild canonical DSP parameters when restoring named presets.
- Verify nonzero intensity with real logs, framebuffer, or metrics.
- For live preset changes, check `LOFI_DSP_UPDATE` or equivalent logs.

## Codecs And Media Cache

- Enable codecs only for real media needs. Check memory and partition impact before adding decoders or encoders.
- M4A/AAC startup issues should favor background AAC cache, not synchronous full-container scans on the playback path.
- Media cache is opportunistic. No new `AAC_CACHE task started` log can mean all candidates are cached or none qualify.
- Judge cache health with `LIBRARY_SYNC`, `AAC_CACHE built/skipped`, candidate state, crash/reboot absence, and playback logs.

## Album Art And Framebuffer Shadow

- Prefer sidecar JPEG or background-generated fixed `72x72` RGB565 thumbnail cache for Now Playing covers.
- Do not decode large images every UI frame.
- Framebuffer shadow for debug screenshots must be allocated on demand and released after capture.
- Release shadow buffers before playback or track changes, then verify AAC/M4A decoder heap behavior with real logs.

## Useful Checks

```bash
./scripts/run_host_tests.sh
python3 scripts/validate_audio_pause_path.py
python3 scripts/validate_lofi_dsp_metrics.py
python3 scripts/validate_audio_evidence_bundle.py
python3 scripts/validate_hardware_smoke_logs.py
```
