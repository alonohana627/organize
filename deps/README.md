# Third-party dependencies

Everything the core needs is vendored here at a pinned version and built by CMake as part of the
project (`deps/CMakeLists.txt`). The shipped application never downloads anything; fetching sources
is a deliberate developer step.

| Directory | Version | License | Used for |
| --- | --- | --- | --- |
| `sqlite/` | 3.53.4 amalgamation | Public domain | Catalog database, FTS5 lexical search, online backup |
| `nlohmann_json/` | 3.12.0 | MIT | History patches, outbox envelopes, settings, export, worker IPC |
| `utf8proc/` | 2.12.0 | MIT | Unicode NFKC case folding for name, sort and tag keys |
| `stduuid/` | 1.2.3 | MIT | RFC 4122 v4 UUIDs |
| `picosha2/` | 1.0.1 | MIT | SHA-256 (migration checksums, copy integrity, byte-exact duplicates) |
| `miniaudio/` | 0.11.25 | Public domain / MIT-0 | WASAPI preview output, resampling, WAV/FLAC/MP3 and Vorbis (stb_vorbis) decoding |
| `pocketfft/` | cpp branch @ c90e55b | BSD-3-Clause | FFT for onset/tempo and chroma (key) analysis |
| `thread_pool/` | 5.1.0 (BS::thread_pool) | MIT | Bounded worker pool for parallel import probes |
| `readerwriterqueue/` | 1.0.7 (moodycamel) | BSD-2-Clause | Bounded blocking SPSC queue (import pipeline; available to the audio engine) |
| `cli11/` | 2.7.2 | BSD-3-Clause | Command-line parsing for the `mso` tool |
| `signalsmith_stretch/` | 1.4.0 | MIT | Time-stretch and pitch-shift for independent tempo/pitch preview (V2) |
| `signalsmith_linear/` | 0.6.5 | MIT | FFT/STFT helpers required by signalsmith-stretch |
| `csv_parser/` | 5.5.0 (vincentlaucsb) | MIT | RFC 4180 CSV writing for metadata export (one patch: CRLF records) |
| `doctest/` | 2.5.3 | MIT | Test framework for `tests/` |

The exact source URL, tag and file list of each one is in [`manifest.json`](manifest.json).

Music Sample Organizer is licensed under the GNU GPL v3 (see `../LICENSE`). Every dependency above
(public domain, MIT, MIT-0, BSD-2/3-Clause) and FFmpeg/Qt under LGPL are GPL-3.0 compatible, so
an established library is always preferred over writing utility code ourselves. A new dependency
must be GPL-3.0 compatible and is added through `manifest.json`.

## Optional system libraries

These are too large to vendor and are discovered at configure time. The build works without them
and reports what is unavailable.

| Library | Provides | MSYS2 CLANG64 package |
| --- | --- | --- |
| Qt 6 Widgets (6.5+) | The desktop application `MusicSampleOrganizer.exe` | `mingw-w64-clang-x86_64-qt6-base` |
| FFmpeg (libavformat, libavcodec, libswresample) | AAC, Opus, ALAC, WMA decoding | `mingw-w64-clang-x86_64-ffmpeg` |

## Updating

```sh
python deps/update_deps.py              # re-fetch everything at the pinned refs
python deps/update_deps.py sqlite       # re-fetch one dependency
python deps/update_deps.py --check      # verify all vendored files are present (no network)
```

To upgrade, change `ref`/`version` (archives also need `url` and `sha256`; branch-pinned git sources also need `commit`) in `manifest.json`, run the
script for that dependency, rebuild, run the tests, and review the diff before committing. Keep each
dependency's license file next to its sources.

## Local modifications (patches)

Vendored files are never edited by hand. A change to a dependency is a patch file under
`patches/<name>/` at the repository root (created with `git diff` against the pristine vendored
files, paths relative to the repository root), listed in that dependency's `"patches"` array in
`manifest.json`. `update_deps.py` re-applies the patches in order after every fetch, and `--check`
reports a listed patch that is missing.

Current patches: `patches/csv_parser/0001-crlf-record-terminator.patch` (RFC 4180 CRLF record
terminator instead of LF).
