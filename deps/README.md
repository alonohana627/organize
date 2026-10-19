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
| `miniaudio/` | 0.11.25 | Public domain / MIT-0 | Preview output (WASAPI on Windows; PulseAudio/ALSA on Linux), resampling, WAV/FLAC/MP3 and Vorbis (stb_vorbis, one patch) decoding (compiled by `miniaudio_impl.c`, below) |
| `pocketfft/` | cpp branch @ c90e55b | BSD-3-Clause | FFT for onset/tempo and chroma (key) analysis |
| `thread_pool/` | 5.1.0 (BS::thread_pool) | MIT | Bounded worker pool for parallel import probes |
| `readerwriterqueue/` | 1.0.7 (moodycamel) | BSD-2-Clause | Bounded SPSC queues: import pipeline, audio engine command and PCM rings |
| `cli11/` | 2.7.2 | BSD-3-Clause | Command-line parsing for the `mso` tool |
| `signalsmith_stretch/` | 1.4.0 | MIT | Time-stretch and pitch-shift for independent tempo/pitch preview (V2) |
| `signalsmith_linear/` | 0.6.5 | MIT | FFT/STFT helpers required by signalsmith-stretch |
| `csv_parser/` | 5.5.0 (vincentlaucsb) | MIT | RFC 4180 CSV writing for metadata export (one patch: CRLF records) |
| `doctest/` | 2.5.3 | MIT | Test framework for `tests/` (`"test_only"`: not shipped, not in the About box or the installer notices) |

The exact source URL, tag, pinned commit (or archive/download SHA-256), file list, license file and
post-patch SHA-256 of every vendored file are in [`manifest.json`](manifest.json). Its
`system_dependencies` list Qt and FFmpeg (with where their source code is) for the generated About-box
notices and `THIRD_PARTY_NOTICES.txt`, which `cmake --install` and the MSI ship next to `LICENSE`.

Project files in `deps/` that the manifest does not produce: `CMakeLists.txt` (targets `mso::<name>`,
except nlohmann_json which is `mso::json`), this README, `manifest.json`, `update_deps.py`,
`.clang-format` (keeps formatters off vendored code) and **`miniaudio_impl.c`**, the project's one
glue translation unit: it compiles the miniaudio and stb_vorbis implementations in the order stb_vorbis
requires (header-only declarations first). Upstream `README.md` files are vendored for picosha2 and
stduuid (their usage documentation; listed in the manifest). Not vendored, because nothing compiles
them: miniaudio's `miniaudio.c` (replaced by `miniaudio_impl.c`) and signalsmith-linear's
`linear-xsimd-dispatch.cpp`.

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

FFmpeg's license depends on its build: LGPL-2.1-or-later, or GPL when built with `--enable-gpl`. The
MSYS2 package is built with `--enable-gpl --enable-version3` (GPL-3.0-or-later), which is compatible
with this GPL-3.0 project. On Linux both FFmpeg and Qt come from the system or, for Qt, from
`.github/scripts/build-qt-libcxx.sh` (see `docs/BUILDING.md` for why Qt must be built with libc++).

## Updating

```sh
python deps/update_deps.py                    # re-fetch everything at the pinned commits/digests
python deps/update_deps.py sqlite             # re-fetch one dependency
python deps/update_deps.py --check            # offline verification (CI runs this)
python deps/update_deps.py --verify-upstream  # fetch to a temporary directory and compare; changes nothing
```

- **Pins.** Git sources are fetched by their exact `commit` (the `ref` tag must still point there; a
  moved tag is an error, never followed); archives and single-file downloads must match `sha256`.
  Downloads use a 60 s network timeout. A git dependency without a `ref` is refused.
- **Staging.** Everything is fetched, verified and patched in a temporary directory first, then copied
  into `deps/` (each file replaced atomically), so a failed download or patch never leaves a
  half-updated file. The set of files is not one transaction: an interrupted copy leaves some files
  new and some old, which `--check` reports until the run is repeated. Files a dependency no longer
  lists are deleted. Git's `core.autocrlf` is disabled for the fetch, so vendored files are the
  upstream bytes on every machine (`.gitattributes` keeps `deps/**` byte-exact). Every dependency was
  re-fetched this way on 2026-10-19, and `--verify-upstream` reported every one equal to its pinned
  upstream (with patches applied), so the tree no longer holds CRLF copies of LF upstream files.
- **Digests.** After a fetch the script records `content_sha256`, the post-patch SHA-256 of every
  vendored file. `--check` verifies them offline, so a hand edit or an unapplied patch fails, and it
  reports files in a dependency directory that the manifest does not produce.
- `"normalize_newlines": true` (csv_parser only) converts upstream CRLF to LF before patching, because
  the vendored copy and its patch are LF.

To upgrade, change `ref`, `commit` and `version` (archives and downloads: `url` and `sha256`) in
`manifest.json`, run the script for that dependency, rebuild, run the tests, and review the diff before
committing. Keep each dependency's license file next to its sources and name it in `license_file`.

## Local modifications (patches)

Vendored files are never edited by hand. A change to a dependency is a patch file under
`patches/<name>/` at the repository root (created with `git diff` against the pristine vendored
files, paths relative to the repository root), listed in that dependency's `"patches"` array in
`manifest.json`. `update_deps.py` re-applies the patches in order after every fetch (in the staging
directory; a patch that does not apply aborts the update), and `--check` detects a missing patch file
or a vendored file whose content is not the recorded post-patch result.

Current patches:
- `patches/csv_parser/0001-crlf-record-terminator.patch`: RFC 4180 CRLF record terminator instead of LF.
- `patches/miniaudio/0001-stb-vorbis-setup-allocation-overflow.patch`: stb_vorbis made safe on
  damaged files (found by the reviews and by `tests/fuzz`). Each change carries a "Music Sample
  Organizer patch" comment in `stb_vorbis.c`:
  - setup allocations sized as `size_t` and capped at 128 MiB per stream
    (`STB_VORBIS_MAX_SETUP_MEMORY`);
  - vendor and comment lengths and the comment count checked against the bytes left in the stream
    before allocating (in push mode a long comment still means "push more data"), strings that run past
    the end of their packet refused, the comment list never left half-initialized, and in FILE mode
    the bytes left counted down once instead of an `ftell` per comment;
  - `uint32` shifts in `get32_packet`;
  - codebooks limited to 24 bits of entries x dimensions (the libvorbis rule), with an overflow check,
    and `lookup1_values` compared in floating point without an overflowing cast;
  - a sparse codebook's temporary tables freed when its setup fails (they leaked per corrupt file);
  - a failed Huffman decode (-1) no longer looked up in `sorted_values`;
  - `Mapping::submap_floor` and `submap_residue` sized for the 16 submaps a mapping may have.
  Re-check this list whenever the patch changes.
