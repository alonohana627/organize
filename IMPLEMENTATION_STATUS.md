# Implementation status

Updated 2026-10-19 · branch `phase/p1-browser` · the first implementation is committed (`4a91682`, `4b81d78`); the review-round changes below are not committed yet (awaiting the owner's OK).

## Where the roadmap stands

| Phase | State |
| --- | --- |
| **P0** foundations | Done: CMake presets (clang + libc++, C++23), pinned vendored deps, SQLite wrapper with migrations/backup/FTS5, worker and audio spikes turned into real modules. USearch feasibility (P0.5) **not done**: vector search is not started. |
| **P1** first useful browser | Done except DAW verification (below): create/open library, referenced-folder import with bounded probes, progress and cancel; paged sortable table; search; preview play/pause/stop/seek/volume with a lazy waveform; drag to DAW (file URLs); notes, ratings, favorites, tags, manual collections; durable undo/redo; missing-source display; verified backups and crash recovery. |
| **P2** reliability and organization | Partly done: multiple libraries with federated search; rename/move tracking with review items and root rebinding; hierarchical tags/categories with rename/move/merge and undoable history; smart collections; CSV/JSON export; customizable shortcuts; dock/column/layout presets; migration with verified pre-migration backup. **Not done:** managed copy import (P2.1; the UI shows it disabled), the offline metadata mirror and safe eject (P2.3), a staged migration *restore* path, million-row selection snapshots. |
| **P3** explicit analysis | Partly done: durable runs/jobs with leases, cancel epochs, pause/resume/cancel, worker process confined by a job object (Windows: CPU-rate and memory caps) or `RLIMIT_AS` + `nice` (POSIX: memory cap, lower priority, no CPU cap); waveform, loudness, tempo, key, byte and PCM hashes; provenance and manual overrides that analysis never overrides. **Not done:** waveform segment *cache browsing* view (R11 waveform browser), duplicate *review* UI (hashes are computed and stored, not yet grouped), held-out real-music accuracy evaluation. |
| **P4** advanced preview | Mostly done: loops, varispeed 0.25–4×, independent tempo/pitch (±24 semitones) via signalsmith-stretch. **Not done:** 30-minute load test, quality/latency report. |
| **P5** semantic search | Not started (model selection, embeddings, ANN). |
| **P6** tempo-aware preview, 5M scale | Not started. |
| **P7** VST3 | Not started. |

Deliberate deviations from the specification, at the owner's request:
- Linux is a target in addition to Windows.
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
  - the parsers are overflow-checked (`ByteCursor`, `checked_add`, no allocation driven by untrusted
    sizes) and fuzz-tested.

  Decoding still uses miniaudio, stb_vorbis and FFmpeg. The CRC for the Ogg tail check
  (`src/media/probe_ogg.cpp`) is also written here, because no vendored library exports it.
- **The worker's two IPC queues are a mutex plus a deque** (`FrameSender`, `EventQueue` in
  `src/worker/worker_process.cpp`). The vendored moodycamel `ReaderWriterQueue` is single-producer, but
  frames come from many supervisor threads, the sender has a "broken" state, and the event queue drops
  the oldest Progress message from the middle of the queue when full; both queues are bounded. No
  lock-free queue library offers that policy.

Not a deviation any more: the preview's **declick fades**. Intact audio is no longer faded (attacks at
frame 0 stay untouched); only mid-signal starts, resume and underrun recovery fade, and loop-seam
crossfades are opt-in (`transport::SetSeamFades`, reported in the snapshot).

Platform code is isolated (`#ifdef _WIN32` with POSIX branches, in `.cpp` files and `detail/` headers);
the Linux build is not yet verified on a Linux machine.

## Review round 2026-10-19 (CODE_REVIEW.md): what changed

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
- CI: see `docs/DEVELOPING.md`. The Linux job is parked (owner's decision, 2026-10-19); its last run
  (release and ASan/UBSan) built cleanly and passed every suite except an include-graph false
  positive in GUI-less builds, since fixed. Windows builds everything from prebuilt MSYS2 packages; Linux builds the core,
  CLI and worker without the desktop app (`MSO_BUILD_GUI=OFF`), because no prebuilt Qt uses libc++
  and building Qt in CI is too slow. `.github/scripts/build-qt-libcxx.sh` stays for building the desktop app
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
- New `tests/sqlite_tests.cpp` (suite `catalog`).
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
  index, takes 10 ms, mostly fsync. `folder:Loops` typed as query text takes 86 ms (`RowMatcher`
  cost; a known follow-up).
- Library lock file `library.lock` (`LockFileEx` on Windows, `flock` on POSIX, `app/file_lock.hpp`): a
  library held by another process opens read-only (`OpenLibraryInfo::in_use_elsewhere`), so the CLI
  opens a library read-only while the desktop app holds it. Migrations and crash recovery run only
  under the lock.
- The workspace file is written atomically under `workspace.lock`.
- CLI: exit codes 0 ok, 1 error, 2 usage, 3 partial result, 130 interrupted; global `--worker <path>`;
  `analyze --timeout`, and `analyze` cannot hang (it stops on a paused queue, a missing worker, the
  timeout, 15 minutes without a finished job, or Ctrl+C, which cancels its runs); `play --backend null`;
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
- New `worker` test suite. `src/worker/worker_platform_posix.cpp` has never been compiled on Linux.

**Desktop UI (responsiveness)**
- Ratings, favorites and category edits are optimistic: the row and the inspector change on click, the
  SQLite write runs in the background and the row is then reconciled with `Application::rows(keys)`; on
  failure the stored values are restored and a notification bar shows the error until dismissed.
- `CatalogChangedEvent`s refresh the affected rows in place (by sample id) instead of reloading the
  table. Changes to rows not on screen re-read the visible rows (throttled to 500 ms, scroll position
  kept).
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

### Integration run (end of the review round)

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

### During the review round (2026-10-19, infra hand-off; other modules were still being edited)

- `cmake --preset release` configures; the release build (RelWithDebInfo, `-Werror`, the new warning
  set) of the whole tree: 0 warnings, 0 errors. The Debug build (libc++ debug hardening) likewise.
- `ctest` on that release build, an intermediate state **superseded by the integration run above**:
  16 entries; 11 passed and 5 failed: `import` and `app` (an import returned `INTERNAL: bind
  parameter 1: bad parameter or other API misuse (rc=21)`, the `for_each_row` bug described under
  "Catalog"), `media` (an FFmpeg channel-change case), `ui` (one case) and `include-graph` (the
  transitional forwarders `include/mso/analysis/worker_{process,protocol}.hpp`, since removed).
- On the committed tree (`HEAD`) with this round's CMake: Debug and `MSO_SANITIZERS=address;undefined`
  builds pass every suite (the sanitizer build includes the worker process tests), and
  `cmake --install` produces a folder whose `MusicSampleOrganizer.exe --exit-when-ready` and
  `mso.exe capabilities` run with only `C:\Windows\System32;C:\Windows` on `PATH`.
- `python deps/update_deps.py --check`: 55 vendored files match the manifest. `--verify-upstream`:
  every file matches its pinned upstream in content; 46 files differ in line endings only (see
  "Known gaps").

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
- Linux build and CI have never run (no Linux machine here; no Git remote yet). The CI workflow, the
  Linux Qt-with-libc++ build script and the pinned LLVM repository setup are therefore untested.
- The vendored git dependencies were originally fetched on Windows with `core.autocrlf=true`, so 46
  vendored files carry CRLF line endings where upstream has LF (content otherwise identical). The
  fixed `update_deps.py` fetches upstream bytes; running `python deps/update_deps.py` once rewrites
  those files to the upstream LF bytes and records the new digests.
- Tempo/key accuracy is proven only on synthetic signals, not on a labeled real-music corpus.
- `folder:Loops` typed as query text takes 86 ms on 100K rows (`RowMatcher` cost), against 3.5 ms
  for the same folder clicked in the navigator; a known follow-up.
- Samples on FAT volumes imported before `file_id` became a 16-byte BLOB are not recognised by file
  id on their next scan.
- Worker IPC uses anonymous stdin/stdout pipes instead of the specification's ACL'd named pipe and
  handshake nonce; the handshake checks the child's process id.
- Memory budgets, 30-minute audio stability, D1M/D5M scale and network-blocked runs are not measured.
- The UI lacks: a waveform browser view, a review-items panel, a relevance-sort choice, persistent
  "select all results" snapshots, and drag-onto-navigator.

## Next actions

1. Get the owner's OK to commit, push to GitHub, and let CI verify Linux.
2. Managed copy import with the operation journal (P2.1), using the existing `file_operations` table.
3. Duplicate review: group `byte_hash`/`pcm_sha256` matches into `duplicate_edges` and show them.
4. Waveform browser view backed by the stored waveform segments (add `Library::waveform_overview`).
5. Offline metadata mirror and safe eject (P2.3).
