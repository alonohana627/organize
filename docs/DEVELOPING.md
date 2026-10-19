# Developing Music Sample Organizer

The [README](../README.md) is written for the people who use the app. This page is the developer
entry point: what the code does, how it is laid out, and how it is built and checked. The project
is C++23 (clang + libc++), Qt 6 and SQLite, licensed under the GNU GPL v3.

- **Build:** [`BUILDING.md`](BUILDING.md) (Windows via MSYS2 CLANG64 is verified; Linux is prepared)
- **What is implemented and verified:** [`IMPLEMENTATION_STATUS.md`](../IMPLEMENTATION_STATUS.md)
- **Command-line tool:** [`COMMAND_LINE.md`](COMMAND_LINE.md)
- **Guides for coding agents:** [`SKILLS/`](../SKILLS/README.md) · repository rules in [`AGENTS.md`](../AGENTS.md)
- **Dependencies:** [`deps/README.md`](../deps/README.md) · icons and other resources: [`resources/README.md`](../resources/README.md)
- **Specification:** `docs/01`–`09`, `contracts/schema.sql` (the original engineering handoff; see below)

## Continuous integration

[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) is configured to build with warnings as errors
and run every CTest entry, using only prebuilt packages for the toolchain, Qt and FFmpeg. The owner's
focus is Windows, so every build runs there (MSYS2 CLANG64: clang + libc++, Qt 6, FFmpeg), covering the
whole project including the desktop app and its UI tests. **Status:** the legs below are configured; a
leg counts as verified only once a run of it is green, and `IMPLEMENTATION_STATUS.md` ("CI on GitHub")
cites that run. The first run of the Windows legs, 39254087106, passed every test in `ci`, `ci-debug`
and `ci-sanitize` (17 of 17 CTest entries each) and the install check, but it installed an outdated
clang (22.1.8) and failed before building the `.msi`; the workflow now upgrades MSYS2 first
(`update: true`) and uses the WiX Toolset the runner already has. A run with those fixes is not
recorded yet.

- **`ci`**: release build and tests, then an install-and-launch check without MSYS2 on `PATH` and the
  `.msi` (kept as a run artifact for 7 days).
- **`ci-debug`**: Debug build with libc++'s debug hardening (bounds, iterator and comparator checks).
- **`ci-sanitize`**: Debug build with AddressSanitizer and UndefinedBehaviorSanitizer; reports are
  symbolized (file:line).
- **`fuzz`**: **off.** Fuzzing is deferred by the owner to priority P10 (2026-10-19). The job stays in
  the workflow behind `if: ${{ false }}`; the libFuzzer targets in `tests/fuzz/`, their seed corpus and
  `tools/run_fuzzers.py` stay in the repository and can still be run by hand (`docs/BUILDING.md`).
- **Lint** checks the vendored-dependency digests and `clang-format`.

Each Windows leg keeps its own ccache, saved even when a run fails or is cancelled. **The Linux job is
parked** (`if: ${{ false }}`). Its last run, 39229983572, **failed**: `include-graph` in both presets
(fixed since), and in the sanitizer preset also `cli` (a use-after-free, fixed since) and `worker` (two
memory-cap tests that cannot pass under ASan). It builds the core, the CLI and the worker only: no
prebuilt Qt uses libc++, and building Qt in CI is too slow (see `BUILDING.md`).

A missing FFmpeg (or, on Windows, Qt) fails the build instead of being skipped.

## What the code does

- **Libraries** are portable folders (`library.json`, `catalog.sqlite`, derived data, verified backups).
  Up to eight can be open and searched together. Source folders are referenced in place; their
  absolute paths are machine-local bindings, so a library survives drive-letter changes. One process
  writes a library at a time (`library.lock`); a library held by another process opens read-only, so
  the CLI can read a library while the desktop app has it open.
- **Import** enumerates and probes metadata only (bounded: ≤4 MiB and ≤2 s per file) in parallel, commits
  in batches of ≤500, and is cancellable and resumable. Rescans recognise renames on the same volume by
  file ID (keeping tags, ratings and history), mark vanished files *missing* (never deleting records),
  and flag files whose content changed for review. Unchanged files are not re-probed. Cloud-only files
  (OneDrive placeholders) are counted and never read; symbolic links and junctions are skipped unless
  following them is enabled (with cycle detection); folders nest at most 256 deep; long Windows paths
  work.
- **Formats:** WAV/RF64/W64, AIFF/AIFC, FLAC, MP3, Ogg Vorbis natively; AAC/M4A, Opus, ALAC, WMA, WavPack,
  CAF and more through FFmpeg (local files only, no network protocols).
- **Search** compiles the query language to parameterized SQL plus escaped FTS5 terms; results page
  through keyset cursors (no OFFSET scans), sort on the whole result, and merge across libraries.
  Relevance ranking fuses libraries with reciprocal rank fusion. The desktop app answers most queries
  from an in-memory index (below).
- **Organize:** ratings, favorites, notes, display names, hierarchical tags (rename, move, merge with
  aliases), categories, manual and smart collections. Every edit is a durable history action with
  compare-and-set undo/redo that survives restarts and refuses to clobber later edits.
- **Preview:** a streaming playback engine (WASAPI on Windows, PulseAudio/ALSA on Linux) with a lock-free
  real-time callback, seek, volume, loops with optional seam fades, varispeed and independent
  tempo/pitch (signalsmith-stretch), and automatic recovery from a lost output device (own output
  thread, retry with backoff).
- **Automatic in the desktop app:** added folders are scanned and analyzed (BPM, key, waveforms,
  loudness, duplicate hashes) without further steps. A default library ("My Samples") is created in
  the workspace on first start, folders are rescanned at start-up and when a drive reconnects, and
  only new or changed samples are analyzed. The automatic profile is the workspace preference
  `analysis.automatic_profile` (default `Full`; empty disables it). The CLI stays explicit.
- **Analysis** runs as durable jobs in a separate `mso-worker` process. On Windows it is confined by
  a job object (memory and CPU-rate caps); on POSIX by an address-space limit (`RLIMIT_AS`) and a lower
  scheduling priority (`nice`), with no CPU cap. Results are fenced by lease, cancel epoch and content
  revision; user overrides always win over analysis.
- **Export** metadata to CSV (RFC 4180, spreadsheet-safe by default) or JSON, atomically.

## Repository layout

```text
include/mso/<module>/   all headers          src/<module>/   implementation (.cpp only)
  domain     identity, text, hashing, errors (std::expected), taxonomy and analysis flags
  catalog    SQLite catalog, migrations, history/undo, scans, analysis queue
  search     query language, SQL compiler, paged/federated search
  media      bounded probes and streaming decoders
  files      file identity and the importer
  audio      real-time preview engine
  analysis   DSP (tempo, key, waveform, loudness, hashes)
  worker     mso-worker IPC protocol, process supervision and confinement (+ the mso-worker program)
  app        workspace, application services, in-memory sample index, library lock, analysis supervisor, export
  cli        the mso tool        ui   the Qt desktop app
include/mso/<module>/detail/   internal headers; the module include graph is checked (CTest include-graph)
tests/      doctest suites per area, shared helpers in test_support.{hpp,cpp}; tests/fuzz/ libFuzzer targets
deps/       vendored libraries (+ patches/)
resources/  application icon (SVG source + generated PNG/ICO), Windows .rc, Linux .desktop entry
SKILLS/     task guides for coding agents    docs/   specification, build and developer docs
```

## How the UI stays responsive

The catalog records every committed mutation in an outbox: `catalog::Library::changes_since(revision)`
returns a `ChangeSet` (changed `sample_ids`, `structure_changed`, or `full_reload` when too much changed
or the outbox was pruned), and `acknowledge_changes(consumer, revision)` lets the outbox be pruned below
the lowest revision every consumer has applied. The search module exposes rows without SQL paging:
`search::for_each_row` streams every live `SampleRow` set-based, `load_rows` hydrates specific ids,
`matching_ids` evaluates FTS text and scope in SQLite, and `RowMatcher` / `matches` evaluate a query on
a row with the SQL compiler's semantics. `is_row_evaluable` says whether a query can be evaluated on
rows: no text terms, no `collection:`, no multi-segment `tag:` and no invalid `category:` path.
`RowMatcher` takes the libraries' merged-tag aliases (`TagAliases`), so a merged tag matches by its old
name, and tests `folder:` against `SampleRow::path_key`, the stored location key, with `starts_with`.

`app::Application` builds an in-memory `SampleIndex` per library from `for_each_row` on a loader thread
and keeps it current from `changes_since` plus `load_rows`. `search` and `count` are served from it
(folder, tag, category and collection scopes, sorting, paging and row-evaluable filters in memory;
text relevance and still-loading indexes fall back to `SearchService`). Its cursors are per-library
keyset cursors (sort key, library, sample id; version 102), and the federated merge finds each
library's position by binary search, so value edits never skip rows. A row whose own sort value changes
across the cursor can reappear, as with `SearchService`, and the UI de-duplicates rows by key. A
cursor becomes `STALE_CURSOR` when rows are added or removed, when the taxonomy (tags, categories,
collections) changes, and when the index is reloaded. The indexes of all open libraries
hold at most 1,000,000 rows together (`ApplicationOptions::index_row_limit`); a library beyond that is
searched in SQLite (`IMPLEMENTATION_STATUS.md` records the memory decision). `apply_edit`, `undo` and
`redo` patch the index before returning, and `rows(keys)` returns current rows for specific
`SampleKey`s (library UUID + sample id): a missing key is omitted, a real failure is an error. Libraries
are reached only through `with_library(uuid, work)`, which keeps the library open while `work` runs.
`LibraryAccessChangedEvent` reports a library opened read-only becoming writable once the CLI releases
its lock. `close_library` returns at once (a job or an index load still holding the library finishes
in the background); *opening* that library again while it is still closing waits up to 10 s
(`ApplicationOptions::close_wait`), then returns a retryable `DB_BUSY`.

`CatalogChangedEvent` is coalesced to at most one per 150 ms per library and carries the changed sample
ids, `structure_changed` (reload navigator trees) and `full_reload` (re-run the query; imports set it).
The UI applies edits optimistically, refreshes changed rows in place from those ids, rebuilds a
navigator tree only on structure changes, and keeps the previous rows on screen until a new page
arrives. More precisely:

- On `STALE_CURSOR` and on a full reload the table refreshes *anchored*: it re-queries from the first row
  of the failing page with enough pages to cover what was loaded, keeping rows, scroll position and
  selection (capped at 500,000 rows). Pages are de-duplicated by `SampleKey`, and pending edits overlay
  every incoming page.
- Writes are ordered per library (one queue each); exports and backups run on a 2-thread long-task
  pool.
- History is read on demand: when its panel is shown, after this window's edits, otherwise at most
  every 2 s while visible. Navigator reads are coalesced to at most one per second per library.
- `MainWindow` delegates to controllers (edit, library, export, analysis, layout, undo routing).

## The specification

This repository started as a specification-only engineering handoff. The numbered documents in `docs/`
and `contracts/schema.sql` remain the authoritative requirements; `MUSIC_SAMPLE_ORGANIZER_SPEC.md` is the
generated single-file version (`python tools/build_handoff.py`), and `python tools/validate_spec.py`
checks the documents and reference schema. Where the implementation deliberately differs from the
specification (for example, Linux is now a target too), `IMPLEMENTATION_STATUS.md` says so.
