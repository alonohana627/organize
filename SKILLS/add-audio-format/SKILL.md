---
name: add-audio-format
description: Support another audio container or codec - bounded metadata probe, streaming decoder backend (miniaudio, stb_vorbis or FFmpeg), extension pre-filter, capability list and generated-fixture tests.
---

# Add an audio format

Import only **probes** (bounded: ≤4 MiB read, ≤2 s); decoding happens for preview and explicit analysis.

## Probe

1. `src/media/container_detect.cpp`: recognise the container from magic bytes (the extension is only a
   hint).
2. Add `src/media/probe_<format>.cpp` implementing the declaration in `include/mso/media/probe_formats.hpp`. Read through
   `BoundedReader` only (it enforces the byte and time budget; on a limit return what you have with
   `partial = true`). Use checked 64-bit arithmetic for every size and offset: hostile headers must never
   over-read, over-allocate or loop forever.
3. Fill `ProbeResult`: actual `format`/`codec`, rate, channels, `bit_depth` only for PCM/lossless,
   `frames` + `duration_exact` only when truly exact (estimates are `duration_exact = false`), tags via
   `tag_mapping` (BPM in milli-BPM, keys through `mso::parse_key`), loops.
4. Register the extension in `src/media/extensions.cpp` (`is_candidate_extension`).

## Decoder

Prefer an existing backend in `src/media/decoder*.cpp`: miniaudio (WAV/AIFF/FLAC/MP3), stb_vorbis (Ogg
Vorbis) or FFmpeg (everything else, compiled when `MSO_HAVE_FFMPEG` is defined; input restricted to our
own file handle, no network, no nested opens). Route the new codec in `open_decoder`, and make sure
`decoder_capabilities()` lists it only when it is really compiled in. Without a backend the decoder must
fail with `UNSUPPORTED_CODEC` and a clear message.

## Tests (`tests/media_tests.cpp`)

Generate fixtures at runtime (hand-built bytes, or `ffmpeg` from PATH with a skip when it is absent):
probe fields, exact vs estimated duration, a truncated/corrupt file failing cleanly, a file larger than
the budget returning `partial`, decode frame counts and seek accuracy. Never commit audio files.
Then run the CLI check (`SKILLS/cli-end-to-end-check`) with a file of the new format.
