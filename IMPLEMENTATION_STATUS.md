# Implementation status

Updated 2026-10-19 · branch `phase/p1-browser` · nothing committed yet (awaiting the owner's OK).

## Where the roadmap stands

| Phase | State |
| --- | --- |
| **P0** foundations | Done: CMake presets (clang + libc++, C++23), pinned vendored deps, SQLite wrapper with migrations/backup/FTS5, worker and audio spikes turned into real modules. USearch feasibility (P0.5) **not done**: vector search is not started. |
| **P1** first useful browser | Done except DAW verification (below): create/open library, referenced-folder import with bounded probes, progress and cancel; paged sortable table; search; preview play/pause/stop/seek/volume with a lazy waveform; drag to DAW (file URLs); notes, ratings, favorites, tags, manual collections; durable undo/redo; missing-source display; verified backups and crash recovery. |
| **P2** reliability and organization | Partly done: multiple libraries with federated search; rename/move tracking with review items and root rebinding; hierarchical tags/categories with rename/move/merge and undoable history; smart collections; CSV/JSON export; customizable shortcuts; dock/column/layout presets; migration with verified pre-migration backup. **Not done:** managed copy import (P2.1; the UI shows it disabled), the offline metadata mirror and safe eject (P2.3), a staged migration *restore* path, million-row selection snapshots. |
| **P3** explicit analysis | Partly done: durable runs/jobs with leases, cancel epochs, pause/resume/cancel, worker process with job-object CPU/memory caps; waveform, loudness, tempo, key, byte and PCM hashes; provenance and manual overrides that analysis never overrides. **Not done:** waveform segment *cache browsing* view (R11 waveform browser), duplicate *review* UI (hashes are computed and stored, not yet grouped), held-out real-music accuracy evaluation. |
| **P4** advanced preview | Mostly done: loops, varispeed 0.25–4×, independent tempo/pitch (±24 semitones) via signalsmith-stretch. **Not done:** 30-minute load test, quality/latency report. |
| **P5** semantic search | Not started (model selection, embeddings, ANN). |
| **P6** tempo-aware preview, 5M scale | Not started. |
| **P7** VST3 | Not started. |

Deliberate deviations from the specification, at the owner's request:
- Linux is a target in addition to Windows.
- The desktop app analyzes automatically: after each import it queues the `Full` profile for the
  folder (only new or changed samples run), rescans folders at start-up and on reconnect, and creates
  a default library in the workspace. The spec required explicit analysis only; the CLI still is.

Platform code is isolated (`#ifdef _WIN32` with POSIX branches); the Linux build is not yet
verified on a Linux machine.

## Verified on this machine (Windows 11, MSYS2 CLANG64, clang 23.1.3, libc++, Qt 6.11.2, FFmpeg 9.0.2)

- `cmake --preset release && cmake --build --preset release` with `-Werror`: 0 warnings, 0 errors.
- `ctest --preset release`: **9/9 suites pass** (domain, catalog, search, import, media, audio, analysis,
  export, ui), about 14 s. In detail: `mso_tests` 124 cases / 32,082 assertions, `mso_ui_tests` 7 cases.
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
- Linux build and CI have never run (no Linux machine here; no Git remote yet).
- Tempo/key accuracy is proven only on synthetic signals, not on a labeled real-music corpus.
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
