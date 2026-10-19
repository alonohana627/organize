# Implementation status

Updated 2026-10-19 · branch `phase/p1-browser` · committed: the first implementation (`4a91682`), the first review round (`6b10cc9`), the Windows installer (`6b5f1d4`) and the second review round (`7115e88`). The **third review round** (section below, from the third `CODE_REVIEW.md`) is not committed yet (awaiting the owner's OK).

## Where the roadmap stands

| Phase | State |
| --- | --- |
| **P0** foundations | Done: CMake presets (clang + libc++, C++23), pinned vendored deps, SQLite wrapper with migrations/backup/FTS5, worker and audio spikes turned into real modules. USearch feasibility (P0.5) **not done**: vector search is not started. |
| **P1** first useful browser | Done except DAW verification (below): create/open library, referenced-folder import with bounded probes, progress and cancel; paged sortable table; search; preview play/pause/stop/seek/volume with a lazy waveform; drag to DAW (file URLs); notes, ratings, favorites, tags, manual collections; durable undo/redo; missing-source display; verified backups and crash recovery. |
| **P2** reliability and organization | Partly done: multiple libraries with federated search; rename/move tracking with review items and root rebinding; hierarchical tags/categories with rename/move/merge and undoable history; smart collections; CSV/JSON export; customizable shortcuts; dock/column/layout presets; migration with verified pre-migration backup. **Not done:** managed copy import (P2.1; the UI shows it disabled), a staged migration *restore* path, million-row selection snapshots. |
| **P3** explicit analysis | Partly done: durable runs/jobs with leases, cancel epochs, pause/resume/cancel, worker process confined by a job object (Windows: CPU-rate and memory caps) or `RLIMIT_AS` + `nice` (POSIX: memory cap, lower priority, no CPU cap); waveform, loudness, tempo, key, byte and PCM hashes; provenance and manual overrides that analysis never overrides. **Not done:** waveform segment *cache browsing* view (R11 waveform browser), duplicate *review* UI (hashes are computed and stored, not yet grouped), held-out real-music accuracy evaluation. |
| **P4** advanced preview | Mostly done: loops, varispeed 0.25–4×, independent tempo/pitch (±24 semitones) via signalsmith-stretch. **Not done:** 30-minute load test, quality/latency report. |
| **P5** semantic search | Not started, and deferred well past the current work (owner, 2026-10-19). |
| **P6** 5M scale | Not started. Tempo-aware preview is scrapped (owner, 2026-10-19). |
| **P7** VST3 | Not started. |
| **P10** fuzzing | Deferred by the owner (2026-10-19, "we had enough" for now): the CI fuzz job is off and the targets are not extended (no decoder target, no new seeds, no CTest replay of the seeds). The 11 targets in `tests/fuzz/`, their seed corpus and `tools/run_fuzzers.py` stay in the repository and run by hand. `CODE_REVIEW.md` §8 lists the deferred findings (§5.3). |

Deliberate deviations from the specification, at the owner's request:
- Scrapped (2026-10-19), not to be built: tempo-aware preview against a project tempo (P6), and the
  offline metadata mirror with safe eject (P2.3). Semantic search (P5) is deferred far out.
- Linux is a target in addition to Windows; Windows comes first (owner, 2026-10-19), and Linux-only
  work waits (see `CODE_REVIEW.md` §8).
- **Priorities (owner, 2026-10-19; `AGENTS.md` rule 17):** stability, bug catching and performance come
  first; the app runs on the user's own workstation. Basic robustness stays: no crash or memory
  corruption from bad or damaged files, and nothing from a file or a library is ever run as code.
  Security-only hardening is out of scope. Accordingly, **worker IPC over anonymous stdin/stdout pipes**
  (the child's process id is checked in the handshake) is an accepted decision, not a gap: the
  specification's ACL'd named pipe and handshake nonce will not be built.
- **In-memory sample index (owner's request), against docs/06's 768 MiB browse budget:** the desktop app
  keeps every open library's sample rows in an in-memory index for responsiveness. Measured with
  `mso bench` (release, Windows, 1M rows with realistic pack-style names averaging 46 bytes, third
  round): 295 bytes per row after load (282 MiB), 314 bytes per row once sort orders are built
  (300 MiB). The indexes of all open libraries are capped at 1,000,000 rows together
  (`ApplicationOptions::index_row_limit`, about 300 MiB). A library that would exceed the cap is searched
  in SQLite with identical results, and the user is told; it is indexed again once it fits. A reload
  keeps the previous index until the new one is ready, so the index can briefly use twice its memory
  (about 600 MiB at the cap), which still fits the 768 MiB browse budget together with the UI and
  SQLite's page cache. (The second round's 248–265 bytes per row came from short synthetic names that
  fit the small-string buffer; with realistic names that code needed 466–490 bytes per row, over budget
  during a reload. `mso bench --names short` reproduces the old figure.) The 64 MiB
  hydrated-row budget is unchanged: full rows are built only for returned pages.
- The desktop app analyzes automatically: after each import it queues the `Full` profile for the
  folder (only new or changed samples run), rescans folders at start-up and on reconnect, and creates
  a default library in the workspace. The spec required explicit analysis only; the CLI still is.

Deliberate exceptions to owner rule 1 ("use libraries, never hand-roll"), with the reason:
- **Container and tag parsers are written in this project** (`src/media/probe_*.cpp`, `id3.cpp`,
  `vorbis_comment.cpp`, `ape_tag.cpp`; internal headers in `include/mso/media/detail/`) instead of
  TagLib or miniaudio's dr_wav/dr_flac/dr_mp3 metadata readers. Reviewed again this round and kept;
  TagLib is not adopted, because:
  - every read goes through one `BoundedFileReader` with a byte budget and a deadline (≤4 MiB and ≤2 s
    per file), so a probe that hits a limit returns a partial result (`PROBE_LIMIT`); TagLib and the
    dr_* metadata APIs read whole tags and chunks, pictures included, and cannot return partial results;
  - tags are walked with header-sized reads, and binary items (ID3 APIC/GEOB/PRIV, MP4 `covr`, Vorbis
    `METADATA_BLOCK_PICTURE` across Ogg pages, ASF `WM/Picture`, APE binary items) are skipped unread;
  - the probe needs container facts TagLib does not expose: acid tempo, `smpl` loops, AIFF `MARK`/`INST`,
    RF64 `ds64`, exact frame counts and gapless trim;
  - the parsers are overflow-checked (`ByteCursor`, `checked_add`, no allocation driven by sizes read
    from the file). They are fuzzed with libFuzzer under AddressSanitizer and UndefinedBehaviorSanitizer
    through `media::probe_memory`, one target per family (RIFF/AIFF/CAF, FLAC, MPEG with ID3, Ogg with
    Vorbis comments and OpusTags, MP4, ASF, APEv2; `tests/fuzz/`): 60 s each, about 0.5–1 million
    inputs per target, no finding (2026-10-19, see "Verified"). The CI fuzz job never completed a run
    (its only run failed at configure, see "CI on GitHub") and is now off: fuzzing is deferred to P10.
    This is crash-proofing against damaged files. (Until this round the claim rested on a
    400-iteration seeded random test without sanitizers.)

  Decoding still uses miniaudio, stb_vorbis and FFmpeg. The CRC for the Ogg tail check
  (`src/media/probe_ogg.cpp`) is also written here, because no vendored library exports it.
- **The worker's two IPC queues are a mutex plus a deque** (`FrameSender`, `EventQueue` in
  `src/worker/worker_process.cpp`). The vendored moodycamel `ReaderWriterQueue` is single-producer, but
  frames come from many supervisor threads, the sender has a "broken" state, and the event queue drops
  the oldest Progress message from the middle of the queue when full; both queues are bounded. No
  lock-free queue library offers that policy.
- **Preview-engine concurrency helpers are written in this project** (`include/mso/audio/detail/`):
  - Seqlock: no vendored library provides one. The callback must publish multi-field progress (epoch,
    consumed and audible positions) without locking or allocating, and `std::atomic` of such a struct
    is not lock-free.
  - PlayheadHistory: latency-compensation logic over a fixed array that the callback writes without
    allocating. No vendored library offers a bounded random-access history.
  - ScopedDenormalFlush: miniaudio's `ma_disable_denormals` is static to its implementation section
    and no other vendored library exports one. It is a thin wrapper over the compiler intrinsics.
  - The transport mailbox's Stop side channel and value coalescing, on top of the vendored moodycamel
    `ReaderWriterQueue`: no vendored queue can amend or reorder an element once queued. The transport
    needs "the newest seek, volume, speed or pitch wins" and "Stop is never refused", without the
    producer blocking or allocating.

  The decoder and output threads now wake up through moodycamel's `LightweightSemaphore`.
- **The audio output thread's request queue is a bounded (32) mutex plus deque of `std::packaged_task`:**
  requests come from many threads and carry futures or completions, and the vendored moodycamel queue
  is single-producer.

Not a deviation any more: the preview's **declick fades**. Intact audio is no longer faded (attacks at
frame 0 stay untouched); only mid-signal starts, resume and underrun recovery fade, and loop-seam
crossfades are opt-in (`transport::SetSeamFades`, reported in the snapshot).

Platform code is isolated (`#ifdef _WIN32` with POSIX branches, in `.cpp` files and `detail/` headers).
The Linux build compiled in CI run 39229983572 (core, CLI and worker, release and ASan/UBSan), but that
run failed tests (see "Not verified / known gaps"), and no Linux run is current.

## Third review round 2026-10-19 (third CODE_REVIEW.md): what changed

**Build, CI, dependencies and docs** (infra)
- CI (`.github/workflows/ci.yml`): MSYS2 now upgrades before installing (`update: true`). With
  `update: false`, the first run of the Windows legs (39254087106) installed clang 22.1.8 from the
  runner image's stale package database instead of the current 23.1.3 used here. The "Toolchain
  versions" step also prints the installed clang, libc++, Qt and FFmpeg packages.
- Fuzzing is deferred by the owner to P10 (see the roadmap): the CI fuzz job is off
  (`if: ${{ false }}`), and the targets are not extended. Its only run (39254087106) failed at configure
  because the `cursor` seeds and five `worker_protocol` seeds were never committed: `.gitignore`
  ignored `*.bin`. `.gitignore` now keeps everything under `tests/fuzz/corpus/`; those six files must
  be added in the next commit.
- `cmake/DependencyNotices.cmake`: the literal CR byte in a `string(REPLACE)` is now the escape `"\r"`,
  so the file is plain LF text.
- `cmake/Packaging.cmake`: with CMake older than 3.29 (which ignores `CPACK_WIX_INSTALL_SCOPE`) packaging
  is skipped with a warning instead of producing a per-user installer.
- `cmake/InstallThirdPartyLicenses.cmake`: pacman runs under `LC_ALL=C` (its field labels are
  translated otherwise), and `share/licenses/PACKAGES.txt` is written on every path, so the pointer in
  `THIRD_PARTY_NOTICES.txt` never leads nowhere.
- Docs: CI legs are described as configured until a green run is cited; corrected the WiX lookup, the
  close-versus-open wait, the stale-cursor conditions, the partial-selection wording, the preview
  re-anchor claim, `folder:Loops`, the CRLF counts and the stb_vorbis patch list in `deps/README.md`.
- The second review's request to commit the deps line-ending refresh separately from the stb_vorbis
  patch could not be followed: both are already in one commit, `7115e88`.

**Catalog and domain** (catalog)
- Rescans of OneDrive or link-heavy roots: the skip-revive statement now uses `locations_live_path`;
  marking 500 skipped paths seen fell from 1612 ms to 29–83 ms at 100k locations (release).
- A plan test explains every catalog statement on `locations` and `jobs` (41 statements) and fails on a
  full scan or a temporary sort, except six documented, deliberate plans.
- Case-insensitive keys follow NTFS for BMP letters whose case pair round-trips (`kFoldVersion` bumped):
  the first open after the upgrade takes a verified `pre-rekey` backup and rebuilds every unique key
  collision-safe; real collisions stay apart and are listed in one `normalization_conflict` review
  item, never merged. Known exceptions (ß/ẞ, Georgian Mkhedruli/Mtavruli) are pinned in tests.
- Backups read from a reader snapshot (imports and edits keep running) and can be cancelled
  (CANCELLED, partial file removed). `list_roots(WithoutSampleCounts)` costs 0.013 ms instead of 116 ms.
- Paused runs keep their jobs paused through release, failure and lease expiry; `scan_frontier` rows
  are cleaned up; chunked edits whose first chunks change nothing resume correctly; `record_scan_gaps`
  records a batch of gaps in one transaction; `OpenOptions::on_progress` reports long opens.
- The FTS5 content check still runs on the writer: SQLite treats it as an INSERT. The reader's
  `PRAGMA integrity_check` covers the index structure.

**Search** (search)
- Rows that fail to hydrate are offered again once, then skipped, so paging always ends; the library is
  marked incomplete in that page's coverage. Cursors are version 4.
- The folder plan choice is cached per library revision (256 entries); a plan test covers every
  location condition the compiler emits.
- Row hydration no longer reads the whole library: SQLite had picked a `(deleted, …)` sort index for
  `s.deleted=0` and sorted every live sample; `+s.deleted=0` makes it search by id. Release, 1M rows:
  `load_rows` of one id 45.3 ms → 0.01 ms (it was almost all of an edit's index update), 200 ids
  51.2 → 3.6 ms, the index load (`for_each_row`) 1977 → 1064 ms, the first unfiltered SQL page
  46.7 → 1.1 ms, `matching_ids` for one file name 41.6 → 0.02 ms; a very broad text term at 100K (`kick`)
  is about 5 ms slower. A plan test pins the three hydration statements.
- `for_each_row` takes a stop token: a library closed during its first index load stops streaming and
  releases its snapshot at once (`SampleIndex::load` uses it).

**Media and import** (media-files)
- stb_vorbis patch extended: all 16 submaps kept (GHSL-2023-168) and a failed codebook lookup no longer
  read through a null table (GHSL-2023-172), each with a regression test; the patch comment maps every
  advisory to its fix.
- Read errors in the Vorbis and FFmpeg decoders are reported (retryable) instead of a silent end, and a
  seek recovers once the drive is back (FFmpeg's own I/O error state is cleared too).
- WOF-compressed files (`compact /exe`, CompactGUI) import as ordinary files. Skipped entries are
  reported to the catalog only for audio extensions; gaps are recorded in batches.
- Read-ahead is capped at the probe's byte budget (`bytes_fetched()` reports disk traffic).

**Analysis, worker and audio** (engine)
- Plugging in a headset (a Windows default-device change) is no longer a lost device: preview follows
  the new default and keeps playing, and the device name and latency update.
- A device that stops silently, or whose driver stops calling back, is detected within about 1 s by a
  watchdog on the output thread and re-opened automatically.
- New `PreviewEngine::*_async` device calls; the UI no longer blocks on device enumeration, choice or
  restart. At most 32 device requests wait; destroying the engine answers them CANCELLED.
- A device switch no longer replays audio from before the switch.
- Analysis reports a file on an unreachable server, share or drive as SOURCE_OFFLINE (retried), never as
  FILE_MISSING. A worker with a stuck task exits within 1 s; the end-of-input and protocol-violation
  exits are tested (Windows).

**Application and CLI** (app-cli)
- Index reloads are coalesced per library (one queued, one running; requests during a load become one
  follow-up); background index updates at most every 100 ms. An edit the outbox cannot describe (a tag
  renamed on more than 10,000 samples) returns only after the reload it caused.
- Sort orders are patched in place: at 1M rows a committed rating edit went from 111 ms to 37 ms, a
  1,000-row BPM edit from 480 ms to 162 ms, and the index load from 3.98 s to 2.79 s (release, realistic
  names). Index memory: see the deviation at the top.
- AnalysisSupervisor deadline, abandon and backoff are unit-tested against a `--self-test` worker.
  `mso search --json` streams its rows; `mso backup` honours Ctrl+C (exit 130); `docs/COMMAND_LINE.md`
  lists every option. New `LibraryOpenProgressEvent` (at most one per 100 ms) for long upgrades;
  `ApplicationOptions::event_handler` is installed before start-up reopens libraries, so their upgrade
  progress reaches the UI too.

**Desktop UI** (ui)
- One anchored table refresh at a time plus at most one owed, 250 ms after the commit, so paging keeps
  working during long analysis; rows held in any page count as known. Row reads that arrive in an older
  generation are read again instead of dropped.
- The table stops paging at 500,000 rows. Committing a refresh with 20,000 selected rows went from
  2.30 ms to 0.69 ms.
- Preview device calls go through the engine's async API. Backups run as cancellable Jobs tasks.
  Smart collections refuse a query that does not parse.
- A library upgrade shows as an "Upgrading library …" row in the Jobs panel (backup, migrations, key
  rebuild, integrity check); names that collided after the upgrade produce one notice naming the
  library, the count and the pre-upgrade backup.

### CI on GitHub

Run 39254087106 (2026-10-19, commit `7115e88`, the second round; the first run of the Windows legs):
- The runner installed **clang 22.1.8**, not 23.1.3 (`update: false`, fixed above); cmake 4.4.3,
  Python 3.14.7, FFmpeg 9.0.2, Qt 6.11.2. The results below are therefore from clang 22.
- `ci`: release build with `-Werror` passed, `ctest`: `100% tests passed out of 17`, install and the
  smoke test of the installed app without MSYS2 on `PATH` passed. **The MSI was not built:** the WiX
  step failed because `choco install wixtoolset --version=3.14.1` refused to downgrade the WiX 3.14.1
  the image already has (package 3.14.1.20250415). The step now uses the installed toolset.
- `ci-debug`: build passed, `100% tests passed out of 17`.
- `ci-sanitize`: build passed, `100% tests passed out of 17` (ASan + UBSan).
- `fuzz`: failed at configure (`Fuzz target cursor has no seed corpus`), see above. The job is now off.
- `lint`: passed (55 vendored files match the manifest; clang-format 23.1.3 clean).

Not yet observed on GitHub: a run with clang 23.1.3 (`update: true`) and a built MSI. Until a green
run is cited here, those two are "configured; not yet run".

### Integration run (third round)

### Integration run of the third round (integrator, 2026-10-19, Windows, MSYS2 CLANG64, clang 23.1.3)
- Release preset (`-Werror`, GUI on): 0 warnings, 0 errors; `clang-format --dry-run -Werror` clean.
- `ctest`: `100% tests passed out of 17`. `tests/mso_tests.exe`: `test cases: 423 | 423 passed |
  0 failed | 3 skipped` (opt-in benchmarks), `assertions: 96068 | 96068 passed`. `mso_ui_tests.exe`
  (offscreen, null audio): `test cases: 78 | 78 passed | 0 failed | 1 skipped`, `assertions: 963 | 963
  passed`.
- Sanitize preset (Debug, ASan + UBSan): `ctest`: `100% tests passed out of 17`; no AddressSanitizer or
  runtime-error line in the log.
- CLI end to end on WAV, FLAC, MP3, Opus and AAC files with a Hebrew folder: 6 added; edit, undo, redo;
  a smart collection created and deleted; `analyze --profile Full` 6 analyzed, 0 failed; `folder:Loops`
  returns the 3 loops; a rename re-imports as `1 renamed`; `backup` writes a verified backup; `check`
  reports `integrity: ok; foreign keys: 0 violation(s); search index: ok`.
- A workspace from the second round opened in the desktop app (`--screenshot`, exit 0): the library took
  a `pre-rekey` backup, rebuilt its keys, and the Jobs panel showed "Upgrading library My Samples —
  Done. The library is ready."; `mso check` reports it intact and `folder:Loops` still finds 3 samples.
- `mso bench --rows 100000 --repetitions 30` (release, medians, now with realistic 46-byte names, so not
  comparable with the second round's short names), SQLite vs in-memory index: first page 0.64 vs
  0.33 ms; folder click 9.99 vs 0.57 ms; duration sort 14.36 vs 0.33 ms; `bpm:120..130` 12.85 vs 3.04 ms;
  folder filter with duration sort 85.67 vs 22.65 ms; ten pages 151.24 vs 1.64 ms; text `kick` 3.71 vs
  22.41 ms; index load 296 ms; index memory 291 bytes/row after load, 333 with sort orders; a rating edit
  committed and patched 1.34 ms (index patch alone 0.10 ms); a 1,000-row BPM edit 128 ms.
- Not run here: fuzzing (deferred to P10); the GitHub CI legs of this round (they run on push).

## Second review round 2026-10-19 (second CODE_REVIEW.md): what changed

**Build, CI, dependencies and checks** (infra)
- Windows sanitizers: the `sanitize` preset builds and runs on Windows (MSYS2 CLANG64 ships the ASan and
  UBSan runtimes). CTest prepends the toolchain's `bin` to `PATH` on Windows, so `ctest` works from any
  shell, and passes `ASAN_SYMBOLIZER_PATH` (llvm-symbolizer next to clang), so reports have file:line.
  Its first run found the `mso collection --delete` use-after-free (fixed in the CLI). The worker's 2 GiB
  job-object cap holds under ASan (peak worker commit about 86 MiB).
- libFuzzer on Windows (`tests/fuzz/`, option `MSO_BUILD_FUZZERS`, preset `fuzz`, runner
  `tools/run_fuzzers.py`), for crash-proofing against bad or damaged input: one target per entry point:
  the media probe per container family (`media::probe_memory`), `stb_vorbis_open_memory` plus decoding
  and seeking, the worker protocol (`worker::decode_frames` and both message decoders), the query
  parser (plus the in-memory `RowMatcher`), and both paging-cursor decoders (`SearchService` and the
  in-memory index). Seed corpora are checked in (`tests/fuzz/corpus/<target>/`, byte-exact). clang's
  driver refuses `-fsanitize=fuzzer` for the MinGW target, so the build spells out the coverage flags
  and links MSYS2's `libclang_rt.fuzzer-x86_64.a` (`cmake/ProjectOptions.cmake`); fuzz builds are
  optimized with libc++'s extensive hardening. Findings are listed under "Verified".
- CI (`.github/workflows/ci.yml`): Windows legs `ci` (release, tests, install, smoke test, MSI artifact),
  `ci-debug` (new preset: Debug with libc++ debug hardening, `-Werror`) and `ci-sanitize` (ASan + UBSan,
  symbolized), plus a `fuzz` job (every target 60 s) and the lint job. ccache is restored and saved per
  leg with `actions/cache/save` under `if: always()`, so failed and cancelled runs still save it; MSYS2
  skips the base-system upgrade (`update: false`); runners pinned to `windows-2025`. The Linux job stays
  parked and its comment now states its real last result. The first GitHub run of these legs is recorded
  under "CI on GitHub" in the third round; the fuzz job has been off since (deferred to P10).
- Installer (`cmake/Packaging.cmake`): `CPACK_WIX_INSTALL_SCOPE perMachine` set explicitly (with CMake
  3.28 policies the WiX 3 default was a per-user registration of a Program Files install). The install
  tree and the MSI now carry `THIRD_PARTY_NOTICES.txt` (generated from the manifest: every shipped
  vendored library with its license text, plus Qt and FFmpeg with where to get their source),
  `share/licenses/<package>/` with the license files of every MSYS2 package a shipped DLL comes from,
  and `share/licenses/PACKAGES.txt` (package, version, SPDX license; 80 packages in a local build). The
  README's screenshot and `docs/COMMAND_LINE.md` are installed next to it. A GUI-less build packages
  the CLI without Start-menu entries.
- Dependencies: every git dependency re-fetched at its pinned commit; `--verify-upstream` reports all
  14 equal to their pinned upstream (patches applied), so the CRLF copies are gone. `doctest` is marked
  `test_only` (not in the About box, the notices or the installer). `update_deps.py` refuses a git
  dependency without `ref` and replaces each file atomically. New patch:
  `patches/miniaudio/0001-stb-vorbis-setup-allocation-overflow.patch` (see `deps/README.md`).
- Module graph: each module links the vendored libraries it uses itself (picosha2 moved from `domain`
  to its users; `analysis` no longer links nlohmann). The include-graph check also rejects including
  another module's `detail/` headers.
- `contracts/schema.sql` carries the indexes and the column added by migrations 4–6.

**Catalog** (catalog-domain)
- Migration 6: indexes `jobs_ready` and `scan_frontier_state`, and the column
  `scan_frontier.directory_key`. Paused runs settle.
- Benchmarks, 100k samples and 100k queued jobs, debug build: a lease took 130.6 ms before and 1.363 ms
  after, including the fsync'd commit. `finish_scan` with 20k unseen locations and 5k frontier
  directories took 5,581.6 ms for one pass before and 458.8 ms in total after.
- Case-insensitive path keys use the simple uppercase mapping, like NTFS; stored keys are rebuilt on
  open, and real collisions open a `normalization_conflict` review item.
- Integrity checks run on a reader snapshot, except the FTS5 check, which needs the writer.
- Corrupt job rows fail with `DB_CORRUPT` instead of blocking the queue.
- One non-throwing JSON helper for every module: `include/mso/domain/json_fields.hpp`.
- `tests/sqlite_tests.cpp` has its own suite, `sqlite` (one more CTest entry).

**Media and import** (media-files)
- Skipped entries (cloud-only, links, unsupported) are marked seen in the scan and never become
  missing (`Library::mark_scan_entries_seen`); skipped directories are recorded as scan gaps.
- `BoundedFileReader` reads through a 16 KiB read-ahead window (charging only the bytes handed out).
- The FFmpeg probe is bounded by the deadline through an interrupt callback.
- The importer keeps a cache of 8 directories' stored stats; identity reads run in parallel.
- The MP3 gapless trim matches dr_mp3.
- `media/native_file.hpp` is public, replacing the detail header other modules included.
- `media::probe_memory` probes bytes in memory with the same parsers (for fuzzing and tests).
- stb_vorbis is patched (heap overflow from the review, and the fuzzer's findings; `deps/README.md`).

**Search** (search)
- `SampleRow::path_key` carries the stored location key; `folder:` compares it with `starts_with` and
  resolves folder rules once per library and root. Release measurements at 100K rows: in memory,
  `folder:Loops` went from 52–75 ms to 2.6–3.4 ms, and a mixed `RowMatcher` query from 5.3–7.4 ms to
  2.5–3.3 ms. In SQL, a small folder (1% of rows) went from 27–97 ms to 11–15 ms for the first page and
  from 56 ms to 11–15 ms for the count; a large folder (25%) is unchanged at about 3.4 ms first page and
  about 60 ms count (the plan is chosen by folder size, under 1/16 of the library or not). The search
  benchmark now asserts the docs/06 budgets.
- `RowMatcher` takes the merged-tag aliases, so `tag:` finds a merged tag by its old name;
  `folder:/Drums` works (a leading slash is ignored).
- The SQL cursor format is v3: relevance retries rows whose hydration failed.
- A bad stored smart-collection query is a per-library coverage failure; other libraries still answer.

**Analysis and worker** (analysis-worker)
- `worker::decode_frames()` decodes a byte buffer as a peer reads its pipe (the fuzzing entry point).
- The worker exits by itself within 1 s of Shutdown even when its task is stuck (third round; it was 2 s).
- New self-test faults `foreign-limits` and `excess-waveform-bins`.

**Application and CLI** (app-cli)
- The in-memory index (deviation above): `mso bench` reports index memory per row. 100k rows: 992 B/row
  before and 322 B/row after load (262 after queries); load 324 ms. 1M rows: load 1.8 s; first page
  2.6 ms; folder filter with duration sort 149 ms in the index against 600 ms in SQLite; 10 pages 3.8 ms
  against 1.2 s; a rating edit plus index patch 79 ms. In memory at 100k, `folder:` went from 92 ms to
  20 ms in the full bench path, and a plain `bpm:` range filter from 2.7 ms to 6.4 ms.
- Index cursors are per-library keyset cursors (version 102), and the federated merge uses binary
  search. A row whose own value changes across the cursor can reappear, as with `SearchService`; the UI
  de-duplicates by key. Structure changes still return `STALE_CURSOR`.
- `Application::rows()` has an error channel (a missing key is omitted; a failure is an error).
- `LibraryAccessChangedEvent`: a library opened read-only upgrades to writable once the CLI releases
  its lock. Closing a library returns at once; *opening* it again while a job or a load still holds it
  waits up to 10 s (`ApplicationOptions::close_wait`), then returns a retryable `DB_BUSY`. An index
  cursor goes stale when rows are added or removed, when the taxonomy changes and on a reload.
- CLI: `mso collection --delete` use-after-free fixed; see `docs/COMMAND_LINE.md` for exit 3 on a paused
  queue, the 35-minute stall limit and `backup` needing the lock.

**Desktop UI** (ui)
- STALE_CURSOR and full reloads answer with an anchored refresh that keeps rows, scroll position and
  selection (capped at 500,000 rows); pages are de-duplicated by `SampleKey`; pending edits overlay
  every incoming page.
- Writes are ordered per library (one queue each); a 2-thread long-task pool runs exports and backups.
- History is read on demand (when shown, after this window's edits, otherwise at most every 2 s while
  visible); navigator reads are coalesced to at most one per second per library.
- New `AnalysisController` and `LayoutController`; new tests (`tests/ui_application_tests.cpp`).
- The preview panel has an Output device menu and a Retry notice when the device disappears. Since the
  third round, edits and analysis act on the whole selection even when only part of it is loaded (keys
  come from every page); only drag and copy need loaded rows. The welcome text fits any window size; the
  navigator menu no longer keeps item pointers across a nested event loop.

**Audio preview** (audio)
- Every epoch change decays the old level, so a seek while playing does not click.
- Speed, pitch, mode, loop and seam-fade changes continue from the consumed position instead of the
  decoded one; the new epoch is published only after its first block is ready, so nothing is skipped;
  at most one callback period can repeat (third `CODE_REVIEW.md` §4.8, partly fixed in the third round).
- A lost output device is reported (`output_device_lost`) and re-opened automatically with backoff
  (500 ms doubling up to 10 s); `restart_output_device()` retries at once and resumes playback.
- Every backend and device call runs on the engine's own output thread, so COM init and uninit are
  balanced.

**Everywhere:** `[[nodiscard]]` on every public function returning `mso::Result` (the third review
counted 250 of 252 declarations; the two without it were private `Workspace` members).

## First review round 2026-10-19 (CODE_REVIEW.md): what changed

**Build, CI and dependencies**
- One static library per module (`mso_module(<name> MODULES ...)`, `cmake/Modules.cmake`); the declared
  module graph is enforced by the `include-graph` CTest entry. New modules: `worker` (IPC protocol and
  process supervision, headers in `include/mso/worker/`) and `cli` (all CLI sources except `main.cpp`,
  linked into `mso_tests` for in-process CLI tests). Internal headers live in
  `include/mso/<module>/detail/` (catalog, media, audio, search, worker).
- AUTOMOC only on the Qt targets; `mso_app` no longer links the audio module (it uses none of it).
- Warnings added (all clean on the whole tree): `-Wold-style-cast -Wconversion` (minus sign and
  int-to-float conversion), `-Wsuggest-override`, `-Wzero-as-null-pointer-constant`, `-Wcast-qual`,
  `-Wextra-semi`, `-Wnewline-eof`, `-Wundef`, `-Wcomma`, `-Wformat=2`, `-Wnull-dereference`, `-Wmove`,
  `-Wdeprecated`, `-Wdocumentation` and more (`cmake/ProjectOptions.cmake` lists the ones still off and why).
- Debug builds use libc++'s debug hardening mode; `MSO_SANITIZERS=address;undefined` (preset `sanitize`).
- `MSO_REQUIRE_GUI` / `MSO_REQUIRE_FFMPEG` make a missing Qt or FFmpeg fatal; the latter also makes the
  FFmpeg-dependent tests fail instead of skipping (`MSO_REQUIRE_FFMPEG=1` in the test environment).
- `cmake --install` produces a self-contained folder (windeployqt plus every toolchain DLL); the
  installed app starts with only `C:\Windows\System32` on `PATH`.
- `mso/generated/dependencies.hpp` (About-box notices with full license texts, plus Qt and FFmpeg) is
  generated from `deps/manifest.json` at configure time.
- `deps/update_deps.py`: every git dependency pinned by commit and fetched by commit; downloads all
  SHA-256-pinned with a network timeout; fetch, verify and patch in a staging directory; files dropped
  from the manifest are deleted; post-patch `content_sha256` per vendored file, verified offline by
  `--check` (detects hand edits, unapplied patches and stray files); `--verify-upstream`. The unused
  `miniaudio/miniaudio.c` and `signalsmith_linear/platform/linear-xsimd-dispatch.cpp` are no longer
  vendored. SQLite additionally uses `OMIT_DEPRECATED`, `OMIT_SHARED_CACHE` and
  `LIKE_DOESNT_MATCH_BLOBS`; its options are PRIVATE to the amalgamation.
- CI: see `docs/DEVELOPING.md`. The Linux job is parked (owner's decision, 2026-10-19). Its last run,
  39229983572, built the core, CLI and worker in both presets but **failed**: `include-graph` in both
  (a GUI-less false positive, fixed since), and in `ci-sanitize` also `cli` (the `collection --delete`
  use-after-free) and `worker` (two memory-cap tests that cannot pass under ASan); CTest reported "79%
  tests passed, 3 tests failed out of 14". Windows passed in run 39233493989 (16/16 entries, install
  and smoke test). Windows builds everything from prebuilt MSYS2 packages; Linux builds the core, CLI and
  worker without the desktop app (`MSO_BUILD_GUI=OFF`), because no prebuilt Qt uses libc++ and building
  Qt in CI is too slow. `.github/scripts/build-qt-libcxx.sh` stays for building the desktop app
  locally on Linux (`docs/BUILDING.md` explains the libstdc++ link problem).
- Tests: CTest entries are generated per `TEST_SUITE` plus a catch-all per executable, with timeouts
  and labels; shared helpers (scratch directories, WAV writers, deadline polling, worker path) are in
  `tests/test_support.{hpp,cpp}`.

**Catalog**
- Migration 5 adds three indexes.
- Outbox envelope v2 (`samples`, `structure`, `all_samples`) is consumed by `Library::changes_since` /
  `acknowledge_changes`; the outbox is pruned below the lowest acknowledged revision of all consumers
  and capped at 256 MiB.
- History payload v2 records `tag_links`; v1 actions still replay.
- Whether a root's file system is case-sensitive is detected when the root is added (`add_root`).
- Backups keep the 10 most recent; user history is never pruned automatically (spec docs/03 §9).
- Readers wait for a free connection with a timeout (`acquire_reader`).
- New `tests/sqlite_tests.cpp` (suite `catalog`; its own suite `sqlite` since the second round).
- Fixed: `catalog::detail::for_each_row` now always resets the statement when it finishes. A statement
  rebound after `SQLITE_DONE` without a reset gave `SQLITE_MISUSE` ("bind parameter 1"), the import
  failure seen in the infra hand-off run below.

**Media and import**
- Internal media headers are all under `include/mso/media/detail/`; the hand-written probe parsers are
  kept (see the rule-1 exception above).
- Vorbis decodes with stb_vorbis first, because FFmpeg's Vorbis decoder drops the first 128 frames
  and so cuts sample attacks. FFmpeg takes the files stb_vorbis refuses (over 2 GiB, or unparseable).
- Decoder contract: seeking at or past the end is not an error (the next read returns 0 frames); the
  channel count and sample rate never change after open. FFmpeg remixes and resamples a mid-stream
  layout or rate change to the first frame's layout and rate.
- `ImportProgress` has new counters: `links_skipped`, `link_cycles_skipped`, `cloud_only_skipped`,
  `unsupported_entries_skipped`, `unsafe_paths_skipped`. Every exit path delivers a final report with
  `finished` set.
- Cloud-only files (OneDrive placeholders) are counted and never read. Symbolic links and junctions
  follow `ImportOptions::follow_reparse_points` (off by default), with cycle detection. Directory depth
  is capped at 256. `\\?\` long paths are supported.
- `file_id` is stored as 16 raw bytes (BLOB). Files on FAT volumes imported before this change do not
  match by id on the next scan.

**Application and CLI**
- The desktop app keeps an in-memory `SampleIndex` per open library (`include/mso/app/sample_index.hpp`;
  see "How the UI stays responsive" in `docs/DEVELOPING.md`). Benchmark, release build,
  `mso bench --rows 100000 --repetitions 30`, uncached (the app owner's run):

  | Operation | SQLite | Index |
  | --- | --- | --- |
  | Folder click, first page | 4.6 ms | 3.5 ms |
  | Duration sort desc, first page | 15.9 ms | 0.21 ms (14 ms the first time the order is built) |
  | Name sort | 3.4 ms | 0.22 ms |
  | `bpm:120..130` | 9.6 ms | 0.76 ms |
  | Text `kick` | 7.5 ms | 11.7 ms (the SQL id query dominates) |
  | 10 consecutive pages | 119 ms | 0.28 ms |

  Loading the index takes 245–320 ms for 100K rows. A rating edit, committed and patched into the
  index, takes 10 ms, mostly fsync. `folder:Loops` typed as query text took 86 ms (`RowMatcher`
  cost); the second round fixed it (2.6–3.4 ms in memory, see its "Search" entry).
- Library lock file `library.lock` (`LockFileEx` on Windows, `flock` on POSIX, `app/file_lock.hpp`): a
  library held by another process opens read-only (`OpenLibraryInfo::in_use_elsewhere`), so the CLI
  opens a library read-only while the desktop app holds it. Migrations and crash recovery run only
  under the lock.
- The workspace file is written atomically under `workspace.lock`.
- CLI: exit codes 0 ok, 1 error, 2 usage, 3 partial result, 130 interrupted; global `--worker <path>`;
  `analyze --timeout`, and `analyze` cannot hang (it stops on a paused queue, a missing worker, the
  timeout, 15 minutes (35 since the second round) without a finished job, or Ctrl+C, which cancels its runs); `play --backend null`;
  the bench has an index section; `mso --json search ... --count` prints one JSON document; progress lines appear
  only on a terminal and never with `--json`; `--version` comes from the generated header.
- New test suites `app` and `cli`.

**Search** (semantics; docs/03 §6 carries an amendment note pointing here)
- Equality at the precision typed: `bpm:120` matches 119.5 ≤ BPM < 120.5.
- Two-valued `NOT`: `X` and `NOT X` partition the library (unknown values fall on the `NOT` side).
- `folder:` and the root/folder scopes use a sample's primary (first live) location; a folder scope
  requires its `root_id`.
- `analyzed:none` (alias `false`) matches samples without any analysis result.
- Smart-collection expansion is capped at 1,024 query nodes.
- Relevance sort keeps non-text OR matches, ranked after the text hits.
- New domain headers: `analysis_flags.hpp` (the `analyzed:` bits, shared by catalog, search and UI),
  `taxonomy.hpp`, `overloaded.hpp`.
- Benchmark test, skipped by default: `mso_tests --test-suite=search --no-skip --test-case=*benchmark*`.
  The search owner's release run: `for_each_row` over 100K rows 141 ms, first page 2.9 ms.

**Analysis and worker**
- Worker protocol 1.1: heartbeats carry `task_progress`, and the supervisor detects stalled tasks
  (`task_stall_deadline`, default 60 s): it sends Cancel, waits a grace period, then terminates the
  worker; the task ends as a retryable `WORKER_CRASH`.
- Algorithm versions bumped: `waveform-minmaxrms-2`, `tempo-flux-autocorr-2`, `key-chroma-ks-2`
  (results from the previous versions are re-analyzed).
- Worker headers moved to `include/mso/worker/` (sources in `src/worker/`, library `mso_worker`).
  New analysis API: `analysis::compute_waveform_overview` (`waveform_preview.hpp`), `limits.hpp`,
  `byte_order.hpp`. The decision not to adopt reproc for process spawning is recorded in
  `include/mso/worker/worker_process.hpp`.
- The `--self-test-fault` worker modes exist for the tests only.
- New `worker` test suite. `src/worker/worker_platform_posix.cpp` compiled on Linux in CI run
  39229983572; running it there is not verified (two of its tests cannot pass under ASan).

**Desktop UI (responsiveness)**
- Ratings, favorites and category edits are optimistic: the row and the inspector change on click, the
  SQLite write runs in the background and the row is then reconciled with `Application::rows(keys)`; on
  failure the stored values are restored and a notification bar shows the error until dismissed.
- `CatalogChangedEvent`s refresh the affected rows in place (by sample id) instead of reloading the
  table. Changes to rows not on screen re-read the visible rows (throttled to 500 ms). The first round
  said the scroll position was kept; after a stale cursor it was not (the view went back to row 400).
  The second round's anchored refresh keeps it.
- The navigator tree is held in memory and rebuilt per library only when the structure changes, never
  on a timer.
- Folder clicks and sorting are answered from the application's in-memory sample index; the previous
  rows stay visible until the new page arrives.
- Single-key shortcuts (Space, Esc, Delete, F, 0–5) act only while the sample table has focus.
- Drag URLs are built from catalog data at drag start.
- The About box lists every dependency with its license text, generated from `deps/manifest.json`.
- `MainWindow` delegates to new controllers: `edit_controller`, `library_controller`,
  `export_controller`, `undo_router` and `notification_bar`.
- The preview waveform uses `analysis::compute_waveform_overview`.
- The file-manager action is labelled "Show in Folder" off Windows ("Show in Explorer" on Windows).

**Audio preview**
- No fades on intact audio (see above); loop-seam crossfades opt-in; minimum loop length 10 ms; the
  reported playhead is compensated for output latency; internal headers in `include/mso/audio/detail/`.

## Verified on this machine (Windows 11, MSYS2 CLANG64, clang 23.1.3, libc++, Qt 6.11.2, FFmpeg 9.0.2)

### Second review round (2026-10-19)

**Integration run** (integrator, 2026-10-19, Windows 11, MSYS2 CLANG64, all modules integrated):
- Release preset (RelWithDebInfo, `-Werror`, GUI on): 0 warnings, 0 errors; `clang-format --dry-run
  -Werror` clean over `include/`, `src/`, `tests/`.
- `ctest` (release, `MSO_REQUIRE_FFMPEG=1`): `100% tests passed out of 17`.
- `tests/mso_tests.exe`: `test cases: 379 | 379 passed | 0 failed | 2 skipped` (the two opt-in
  benchmarks), `assertions: 86533 | 86533 passed`.
- `mso_ui_tests.exe` (offscreen, null audio): `test cases: 58 | 58 passed | 0 failed`,
  `assertions: 594 | 594 passed`.
- Sanitize preset (Debug, ASan + UBSan, `-Werror`): `ctest`: `100% tests passed out of 17`, no
  AddressSanitizer or UBSan report in the log. The debug preset was not run separately (the sanitize
  preset is a Debug build).
- Fuzzing: not re-run by the integrator; the infra owner's last run (after the final stb_vorbis patch)
  ran all 11 targets for 60 s each with no finding.
- CLI end to end on WAV, FLAC, MP3, Opus and AAC files with a Hebrew folder: 6 added; edit, undo, redo;
  `collection --create` then `--delete` (the old use-after-free) exit 0; `analyze --profile Full`
  analyzed 6, failed 0; `folder:Loops` returns the 3 loops; a rename re-imports as `1 renamed`; `check`
  prints `integrity: ok; foreign keys: 0 violation(s); search index: ok`.
- A workspace created by the previous build opened in the desktop app (`--screenshot`, exit 0); the
  library migrated to migration 6 and `mso check` reports it intact.
- `mso bench --rows 100000 --repetitions 30` (release, medians, SQLite vs in-memory index): first page
  2.55 vs 0.21 ms; folder click 3.75 vs 0.41 ms; duration sort 11.64 vs 0.20 ms; `bpm:120..130`
  10.00 vs 1.80 ms; folder filter with duration sort 60.01 vs 7.63 ms; ten pages 119.89 vs 0.70 ms;
  text `kick` 5.62 vs 10.59 ms; index load 186 ms; index memory 322 bytes/row after load, 270 after
  the queries; rating edit committed and patched 7.76 ms; inserts 12,872 rows/s.

Recorded by the owners during the round (other modules were still changing, so these are not the
final state):
- Windows `sanitize` preset (infra): built and ran on Windows for the first time. First run: 15 of 16
  CTest entries passed; `cli` failed with the expected AddressSanitizer heap-use-after-free (CLI11 flag
  callback writing through a `shared_ptr<bool>` freed when `register_collection` returned,
  `src/cli/edit_commands.cpp`), fixed by the CLI owner since. No UBSan report. The worker suite passes
  under ASan on Windows.
- Fuzzing (infra), `fuzz` preset, 60 s per target, 5–6 targets in parallel on this PC:
  `probe_riff`, `probe_flac`, `probe_mpeg_id3`, `probe_ogg`, `probe_mp4`, `probe_asf`, `probe_ape`,
  `query_parser`, `worker_protocol` and `cursor` ran clean from the first run (about 0.4–1.5 million
  inputs each; `cursor` about 160,000). `stb_vorbis` found four defects in the vendored decoder within seconds: a
  left shift of a negative value in `get32_packet` (UBSan), a 1.8 GB allocation from a comment length
  read from the file, a float-to-int overflow in `lookup1_values` (UBSan), and a leak of the codebook
  setup buffer on corrupt codebooks (up to 2 MB per open, found as the fuzzer's RSS limit; present in
  upstream 1.22 too). All four are fixed by the recorded stb_vorbis patch, and their inputs are
  regression seeds in `tests/fuzz/corpus/stb_vorbis/`. After the fixes, a final 60 s run of all 11
  targets (6 in parallel) was clean: `stb_vorbis` about 145,000 inputs, `cursor` about 173,000, the
  others 0.4–1.4 million each.
- MSI (infra): `cpack` with WiX 3.14 builds the package from a local (sanitizer) build; the generated
  WiX sources carry `InstallScope="perMachine"`. The package was not installed on a clean PC.

### Integration run (end of the first review round)

2026-10-19, all modules integrated, release preset (RelWithDebInfo, `-Werror`, GUI on):

- Build: 0 warnings, 0 errors. `clang-format --dry-run -Werror` (23.1.3) clean over `include/`, `src/`, `tests/`.
- `ctest --preset release` (with `MSO_REQUIRE_FFMPEG=1`): `100% tests passed out of 16` (every suite,
  both catch-alls and `include-graph`).
- `tests/mso_tests.exe`: `test cases: 315 | 315 passed | 0 failed | 1 skipped` (the skipped case is the
  100K search benchmark), `assertions: 77556 | 77556 passed`.
- `mso_ui_tests.exe` (offscreen, null audio): `test cases: 36 | 36 passed | 0 failed`,
  `assertions: 287 | 287 passed`, including the end-to-end case (import, search, navigator, preview,
  drag check, optimistic rating, main window).
- CLI end to end (`SKILLS/cli-end-to-end-check`) on generated WAV, FLAC, MP3, Opus and AAC files including
  a Hebrew folder and file name: 7 added; `kick bpm:120..130` finds the loop; edit, undo and redo;
  `analyze --profile Full` analyzed 7, failed 0, both loops read 128 BPM; a rename and a deletion
  re-import as `1 renamed, 1 marked missing`; `check` prints `integrity: ok; foreign keys: 0
  violation(s); search index: ok`; `play --backend null` exits 0.
- `mso bench --rows 100000 --repetitions 30` (medians): inserts 10,023 rows/s; index load 316 ms;
  first page by name 3.31 ms (SQLite) vs 0.21 ms (index); folder click 4.46 vs 3.47 ms; duration sort
  15.13 vs 0.21 ms; `bpm:120..130` 13.69 vs 0.72 ms; ten pages 156.40 vs 0.28 ms; text `kick` 7.32 vs
  11.59 ms; `folder:` filter with duration sort 79.27 vs 85.61 ms (known follow-up); rating edit
  committed and patched into the index 10.59 ms.

### During the first review round (2026-10-19, infra hand-off; other modules were still being edited)

- `cmake --preset release` configures; the release build (RelWithDebInfo, `-Werror`, the new warning
  set) of the whole tree: 0 warnings, 0 errors. The Debug build (libc++ debug hardening) likewise.
- `ctest` on that release build, an intermediate state **superseded by the integration run above**:
  16 entries; 11 passed and 5 failed: `import` and `app` (an import returned `INTERNAL: bind
  parameter 1: bad parameter or other API misuse (rc=21)`, the `for_each_row` bug described under
  "Catalog"), `media` (an FFmpeg channel-change case), `ui` (one case) and `include-graph` (the
  transitional forwarders `include/mso/analysis/worker_{process,protocol}.hpp`, since removed).
- On the committed tree (`HEAD`) with this round's CMake: Debug and `MSO_SANITIZERS=address;undefined`
  builds passed every suite of that time (the `cli` suite did not exist yet; the second round's
  Windows sanitizer run found a use-after-free in it), and
  `cmake --install` produces a folder whose `MusicSampleOrganizer.exe --exit-when-ready` and
  `mso.exe capabilities` run with only `C:\Windows\System32;C:\Windows` on `PATH`.
- `python deps/update_deps.py --check`: 55 vendored files match the manifest. `--verify-upstream`:
  every file matched its pinned upstream in content; 46 files differed in line endings only (the count
  of that run). The second round's re-fetch (`7115e88`) converted 44 vendored files to LF;
  `readerwritercircularbuffer.h` stays CRLF because upstream ships it that way.

### Before the review round

- `MusicSampleOrganizer.exe --exit-when-ready` and `--screenshot` start, render and exit cleanly
  (offscreen platform); the window shows a real library with WAV/FLAC/MP3/Opus/M4A/Hebrew-named files.
- End-to-end CLI run on generated audio (see `SKILLS/cli-end-to-end-check`): import, search, edit,
  undo/redo, collections, tag rename + undo, rename tracking (tags kept), missing detection, content
  change review, `analyze --profile Full` through `mso-worker` (120.18 / 128.00 / 140.05 BPM on generated
  120/128/140 loops; no tempo for a one-shot), JSON/CSV export, `check` = integrity ok.
- Synthetic-metadata benchmark, `mso bench --rows 100000` (release build, this PC, warm, 30 repetitions;
  catalog and search only, no audio decoding):

  | Measurement | Median | p95 |
  | --- | --- | --- |
  | Catalog insert | 9,649 rows/s | |
  | First page, name sort | 0.61 ms | 0.72 ms |
  | Text `kick` | 3.91 ms | 4.57 ms |
  | `kick bpm:120..130 duration:<2s` | 16.19 ms | 16.96 ms |
  | `folder:Loops`, duration sort | 74.43 ms | 75.86 ms |
  | Relevance `kick OR snare` | 67.07 ms | 81.46 ms |
  | 10 consecutive keyset pages | 184.41 ms | 185.85 ms |
  | Exact count `kick` | 11.70 ms | |
  | Database size | 77.5 MiB | |

  These are D100K synthetic-metadata numbers on a developer PC, not the B1 reference machine and not
  the D1M/D5M release gates.

## Not verified / known gaps

- Real DAW drag-and-drop (Ableton and another DAW) has not been tried by a person.
- Audible playback on a real device was not listened to; tests use the null/offline backends.
- CI runs on GitHub (`alonohana627/organize`). The Linux job (core, CLI, worker; release and
  ASan/UBSan) has never passed: its last run, 39229983572, failed `include-graph`, `cli` and `worker`
  (see above), and it is parked. The desktop app has never been built on Linux, and
  `.github/scripts/build-qt-libcxx.sh` has not completed a run. The Windows legs passed their tests on
  GitHub in run 39254087106, but with clang 22.1.8 and without the MSI (see "CI on GitHub"); a run
  with the current toolchain and a built MSI is not recorded yet. The fuzz job is off (P10).
- The MSI has not been installed and uninstalled on a clean PC, and its size is not recorded. Some of
  the 80 shipped MSYS2 packages (FFmpeg among them) carry no license file; `PACKAGES.txt` names their
  SPDX license (GPL-3.0 is `LICENSE` itself).
- Tempo/key accuracy is proven only on synthetic signals, not on a labeled real-music corpus.
- Samples on FAT volumes imported before `file_id` became a 16-byte BLOB are not recognised by file
  id on their next scan.
- Memory budgets other than the index's, 30-minute audio stability, D1M/D5M scale and network-blocked
  runs are not measured.
- Linux-only review items wait (`CODE_REVIEW.md` §8): the Linux symbolizer, `INSTALL_RPATH`, the libc++
  probe, PDEATHSIG and fd tests, the POSIX process group for Ctrl+C, `QFile::decodeName`.
- The UI lacks: a waveform browser view, a review-items panel, a relevance-sort choice, persistent
  "select all results" snapshots, and drag-onto-navigator.

## Next actions

1. Managed copy import with the operation journal (P2.1), using the existing `file_operations` table.
2. Duplicate review: group `byte_hash`/`pcm_sha256` matches into `duplicate_edges` and show them.
3. Waveform browser view backed by the stored waveform segments (add `Library::waveform_overview`).
