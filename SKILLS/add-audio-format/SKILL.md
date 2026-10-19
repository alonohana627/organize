---
name: add-audio-format
description: Support another audio container or codec - bounded metadata probe, streaming decoder backend (miniaudio, stb_vorbis or FFmpeg), extension pre-filter, capability list and generated-fixture tests.
---

# Add an audio format

Import only **probes** (bounded: ≤4 MiB read, ≤2 s); decoding happens for preview and explicit analysis.

Container and tag parsing is written in this project (`src/media/probe_*.cpp`, `id3.cpp`,
`vorbis_comment.cpp`, `ape_tag.cpp`) rather than delegated to TagLib or miniaudio's dr_* metadata
readers. This is a recorded exception to owner rule 1 (`IMPLEMENTATION_STATUS.md`, deviations); TagLib
was reviewed and not adopted because:

- every read goes through `BoundedFileReader` with a byte budget and a deadline, so a probe that hits a
  limit returns a partial result (`PROBE_LIMIT`); TagLib and the dr_* metadata APIs read whole tags and
  chunks, pictures included, and cannot return partial results;
- tags are walked with header-sized reads and binary items (ID3 APIC/GEOB/PRIV, MP4 `covr`, Vorbis
  `METADATA_BLOCK_PICTURE` across Ogg pages, ASF `WM/Picture`, APE binary items) are skipped unread;
- the probe needs container facts TagLib does not expose: acid tempo, `smpl` loops, AIFF `MARK`/`INST`,
  RF64 `ds64`, exact frame counts, gapless trim;
- the parsers are overflow-checked (`ByteCursor`, `checked_add`) and fuzzed: `probe_memory()` runs the
  same parsers over a byte buffer for the libFuzzer targets in `tests/fuzz/`.

Keep it that way: a new format gets a bounded parser here, not an unbounded library call. All internal
media headers are in `include/mso/media/detail/` (`probe_formats.hpp`, `bounded_reader.hpp`,
`byte_cursor.hpp`, `tag_mapping.hpp`, ...). The platform file helpers other modules also need
(`native_path` for long Windows paths, `UniqueWin32Handle`, native error mapping) are the public
`include/mso/media/native_file.hpp`. The Ogg page CRC is hand-written too, because no vendored
library exports one.

## Probe

1. `src/media/container_detect.cpp`: recognise the container from magic bytes (the extension is only a
   hint).
2. Add `src/media/probe_<format>.cpp` implementing the declaration in
   `include/mso/media/detail/probe_formats.hpp`. Read through
   `BoundedFileReader` only (it enforces the byte and time budget; on a limit return what you have with
   `partial = true`). Small header reads are cheap: the reader serves them from a 16 KiB read-ahead
   window and charges only the bytes it hands out, so walk headers with small reads rather than
   loading large blocks. Use checked 64-bit arithmetic (`ByteCursor`, `checked_add`) for every size and
   offset: hostile headers must never over-read, over-allocate or loop forever. Skip binary tag items
   (pictures, private frames) by their header length without reading them.
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

Order matters: Vorbis goes to stb_vorbis before FFmpeg, because FFmpeg's Vorbis decoder drops the first
128 frames and so cuts sample attacks; FFmpeg takes the files stb_vorbis refuses (over 2 GiB, or
unparseable). Every backend keeps the decoder contract:

- seeking at or past the end is not an error, and the next read returns 0 frames;
- the channel count and sample rate never change after open (FFmpeg remixes and resamples a mid-stream
  layout or rate change to the first frame's layout and rate, keeping the samples the old resampler
  still held);
- `frames_exact()` is true only when a full read delivers exactly `total_frames()` on that backend
  (FFmpeg's Vorbis decoder is 128 frames short of the granule count, so it reports an estimate; the MP3
  probe trims at least the 529-frame decoder delay at the end, as dr_mp3 does);
- a read that fails part-way (a drive that drops out) is an error, never a silent end of the stream:
  stb_vorbis turns a failed `fread` into EOF, so its backend checks `ferror()`, and the FFmpeg backend
  checks the read callback's error at every end of file. A seek starts afresh (`clearerr`,
  `FfmpegInput::clear_read_error`), so the file plays again once the drive is back. The tests lock a
  byte range of the file (`LockFileEx`) to make reads fail.

A decoder library is vendored code: a fix to it is a patch in `patches/<name>/` listed in
`deps/manifest.json` (for example `patches/miniaudio/0001-stb-vorbis-setup-allocation-overflow.patch`,
which bounds stb_vorbis's header allocations, frees a failing codebook's temporary tables, keeps all
sixteen submaps of a mapping and never looks up a failed codebook decode), never a hand edit. To change
a patch: reverse-apply it to a copy of the vendored file to get upstream, edit a second copy, write
the patch with `git diff` (paths `a/deps/...`), then run `python deps/update_deps.py <name>` and
`python deps/update_deps.py --check`.

## Tests (`tests/media_tests.cpp`)

Generate fixtures at runtime (hand-built bytes, `mso::test::write_wav`, or `ffmpeg` from PATH; when
ffmpeg is absent the test is skipped through `doctest::skip(...)`, unless `mso::test::ffmpeg_required()` is true, which
CTest sets in `MSO_REQUIRE_FFMPEG` builds such as CI, where it must FAIL):
probe fields, exact vs estimated duration, a truncated/corrupt file failing cleanly, a file larger than
the budget returning `partial`, decode frame counts and seek accuracy, and `probe_memory()` agreeing
with `probe_file()`. Crafted hostile inputs are built in the test too (see the stb_vorbis comment-header
cases). Never commit audio files.
Then run the CLI check (`SKILLS/cli-end-to-end-check`) with a file of the new format.
