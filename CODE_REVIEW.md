# Code review: Music Sample Organizer (third review)

**Reviewed:** 2026-10-19, the **working tree** of `phase/p1-browser`: the uncommitted fix round for
the second review, on top of commit `6b5f1d4`. Another session was still finishing the round
(`tests/test_support_tests.cpp` changed during the review), so a few line numbers may drift.

**Scope:** every file changed since `6b5f1d4` and every new file, read in full together with the
callers and tests of each changed function. That is about 12,200 lines added and 5,250 removed in
240 files once line-ending-only changes are ignored. It includes the new `tests/fuzz/`,
`tools/run_fuzzers.py`, `patches/miniaudio/`, `cmake/InstallThirdPartyLicenses.cmake`, the output
supervisor, the new UI controllers, `domain/json_fields` and `media/native_file`.

**Method:**
- Read-only. Nine reviewers each took one slice. Every Critical, High and Medium from the second review
  was re-checked and given a status. Every new High was re-opened and traced by hand before it went
  into this report ("verified").
- "Possible" means the reasoning is sound but the behaviour was not reproduced.
- Measured in a scratch copy, not in the repo:
  - catalog query plans: a schema built from the working-tree migrations, run in Python's SQLite 3.50.4
    with 100k rows;
  - one libc++ `exists()` probe;
  - the stb_vorbis patch, applied to the `6b5f1d4` file.
- Read from existing logs:
  - `build/integ-asan/Testing/Temporary/LastTest.log` (Windows ASan + UBSan, Debug, GUI on: 17/17
    CTest entries passed);
  - `build/agent-infra-fuzz/run3.log` (11/11 fuzz targets clean).

**Scope rules from the owner (2026-10-19):**
- Windows only. Linux and POSIX items are listed once in §8 as deferred.
- The app runs on heavy workstations, so stability (no crashes on bad files, no hangs, no data loss),
  bug catching and **performance** come first.
- Security hardening is not a priority. Findings that only matter against an attacker are dropped,
  for example lock-file tampering. A crash on a malformed file is still a stability finding.

**Yardstick:** the rules in `AGENTS.md` and the deviations documented in `IMPLEMENTATION_STATUS.md`,
plus memory safety, performance (a hot-path finding is rated one level higher), standard conformance,
readability and ease of change.

---

## 1. Summary

**Overall: 8.5 / 10.** That is up from 7.5 in the second review and 6.5 in the first.

### What the fix round achieved

- **Every second-review Critical and High is fixed, with a test.** That covers the CLI use-after-free,
  the stb_vorbis overflow (a recorded, digest-pinned patch), the navigator menu use-after-free, skipped
  entries becoming missing, the lease query, keyset index cursors, anchored refresh, `folder:` matching
  cost and the false Linux CI claims. Almost every Medium is fixed too.
- **Bug catching is now real on Windows:**
  - the `sanitize` preset passes 17/17 under ASan + UBSan, and its first run caught the CLI bug;
  - eleven libFuzzer targets cover every untrusted-input parser, and they already found and fixed four
    stb_vorbis defects;
  - CI has Windows `ci-debug`, `ci-sanitize` and a time-boxed fuzz job;
  - 250 of 252 header Result APIs are `[[nodiscard]]`.
- **The SampleIndex is bounded and correct:** a 1M-row cap with fallback to SQLite, a recorded memory
  deviation, compact rows, and keyset cursors resumed per stream.
- **Analysis, worker, search, domain and dependencies** are at 9 or above.

### What keeps it from 9

- **Two new Highs, both performance or stability on common Windows paths:**
  - **Scans of OneDrive or link-heavy folders:** the new skip handling reads every location in the
    root once per skipped file (measured at 3.5 ms per path at 100k locations, under the writer lock).
  - **The table during long analysis:** the new anchored refresh can restart itself before it finishes,
    so paging freezes (possible).
- **One crash on a crafted Vorbis file** that the patch does not cover: stb_vorbis writes one byte past
  the `submap` arrays.
- **A handful of Medium regressions in new code:**
  - a Windows default-device change is reported as a lost device;
  - index reload storms and index patch cost;
  - the UI thread waits on audio device enumeration;
  - WOF-compressed sample folders are skipped.
- **None of the new CI legs has run on GitHub yet**, because the tree is uncommitted.

### Fix first

| # | Severity | Where | Problem |
|---|---|---|---|
| 1 | High (verified, measured) | `src/catalog/library_scan.cpp:772-775` | The revive `UPDATE … WHERE root_id=? AND path_key=? AND state='missing'` cannot use the partial unique index `locations_live_path` (SQLite does not infer `state<>'retired'`), so it scans every location in the root per skipped path: 3.5 ms each at 100k, about 1.75 s of writer time per 500-path batch, about 6 min per scan of a OneDrive root with 100k online-only files. → Add `AND state<>'retired'` (measured plan: `SEARCH … USING INDEX locations_live_path`). |
| 2 | High (verified code, livelock possible) | `src/ui/sample_table_model.cpp:326-353` with `src/ui/main_window.cpp:506-510, 522-524` | `refresh()` has no guard against a refresh already running. The 500 ms window timer fires whenever an event names a sample that is not hydrated (evicted pages count). Each call restarts the walk of every loaded page, and while `refreshing_` is set, evicted rows do not re-hydrate and `canFetchMore` is false. During long analysis of a deep view the walk may never commit, so paging freezes. → When a refresh is running, set a "dirty again" flag and refresh once after the commit; count keys present in any page's `keys` as known. |
| 3 | Medium (verified, crafted file) | `deps/miniaudio/extras/stb_vorbis.c:758-759, 4188-4193` | `submap_floor[15]` and `submap_residue[15]` take up to 16 submaps (`get_bits(f,4)+1`). `j == 15` writes one byte past the struct and later indexes `residue_config` out of range, so a crafted or damaged `.ogg` crashes the desktop app on preview or waveform. The fuzzer did not reach this. → In the recorded patch, make both arrays `[16]` or reject `submaps > 15`; add a seed and a test. |
| 4 | Medium (verified by reading miniaudio) | `src/audio/output_device.cpp:184-189`, `output_supervisor.cpp:181-183` | When Windows changes the default device (a headset is plugged in), miniaudio reroutes and fires "stopped". The engine treats that as DEVICE_LOST, flushes, tears the rerouted device down after 500 ms and resumes Paused. A failed stop (unplug of a chosen endpoint) fires nothing, so the loss is possibly missed and the engine plays silently. → Check `ma_device_get_state` before closing, handle `rerouted`, and add a stalled-callback watchdog. |
| 5 | Medium (verified by reading) | `src/app/application.cpp:300-306, 382-385, 454-457, 472-503` | Index reloads are queued with no dedup. Every poll and `request_update` during a running reload queues another full load: after one large tag rename during analysis, tens of seconds of back-to-back 1M-row reloads, each doubling memory and each forcing a full UI refresh. → A `load_pending` flag, and skip synchronize while `loading()`. |
| 6 | Medium | `src/app/sample_index.cpp:743-772` | `patch_orders` copies and re-merges every cached sort order on every synchronize, under the exclusive lock, even for one changed row (the owner measured 79 ms per rating edit at 1M rows). During analysis, searches and paging stall several times a second. → Insert or erase changed slots in place with `lower_bound`; skip orders whose key did not change; coalesce. |
| 7 | Medium (verified) | `src/ui/preview_panel.cpp:249, 258, 310, 437, 451, 474, 490` | Space/Play while the device is lost, Retry, opening the Output menu and choosing a device all block the UI thread on `outcome.get()` (`output_supervisor.cpp:63`) while WASAPI enumerates or re-opens endpoints (hundreds of ms to seconds). → Run them on `engine_pool_` and deliver results asynchronously. |
| 8 | Medium | `src/files/file_identity.cpp:45-51, 120-130` | `IO_REPARSE_TAG_WOF` (files compressed with `compact /EXE` or CompactGUI, common on big sample drives) is classified "unsupported" and skipped, so such folders never import. → Treat WOF like a deduplicated file (an ordinary file); add a test. |

### Findings by severity

| Severity | New in this review | Still open from the second review |
|---|---|---|
| Critical | 0 | 0 |
| High | 2 | 0 |
| Medium | 16 | 2 partly fixed (§3) |
| Low / Nit | about 70 | a few, listed per module |

---

## 2. Scorecard

| Module / area | Score (2nd, 1st) | One-line verdict |
|---|---|---|
| domain | **9** (9, 7) | NTFS-style simple uppercase keys, key version checked on open, shared `json_fields`. |
| catalog | **8** (7.5, 6.5) | Every High/Medium fixed and plan-checked; migration 6 correct and pinned; one new hot-path High (skip revive); a key-collision path can duplicate a library. |
| search | **9** (8, 7) | RowMatcher per-row cost fixed (stored `path_key`, memo per root and taxonomy id); aliases resolved; SQL/memory parity re-checked field by field. |
| media | **8.5** (8, 7) | stb_vorbis patched properly with tests and a fuzz target; the `submap` overflow remains; read-ahead and exact-length contracts fixed. |
| files | **8.5** (7.5, 6) | Skips no longer become missing; flat-folder stat cache; duplicate-link detection; WOF files are skipped. |
| analysis | **9.5** (9, 8) | Hot path clean; duplicate stages refused; only pocketfft's documented scratch allocation is left. |
| worker | **9** (8.5, 7) | Exits on its own with a stuck task; bins and limits cross-checked; 36/36 under ASan on Windows. |
| audio | **8** (8, 7) | Every second-review finding fixed, and callback re-traced clean; the new output supervisor is sound but its loss detection misreads default-device changes. |
| app | **8** (7, 5) | SampleIndex capped, keyset cursors, close and reopen, read-only upgrade; reload storms and patch cost remain. |
| cli | **8.5** (7, 5) | Use-after-free fixed at the root (all option storage in `CommandContext`), every binding audited, ASan-clean. |
| ui (core) | **7.5** (7, 6) | Anchored refresh, ordered writes, error channel, refusal of partial selections; the refresh can livelock, and row reads are dropped across generations. |
| ui (panels) | **8.5** (7, 6) | Navigator use-after-free fixed by value capture, reads coalesced, history on demand, device recovery UI; device calls block the UI thread. |
| Build & CMake | **9** (8, 7) | WiX per machine, third-party licenses shipped, fuzz and sanitizer plumbing; a literal CR in one CMake file. |
| CI | **7.5** (6, 4) | Windows `ci`, `ci-debug`, `ci-sanitize`, fuzz and MSI legs designed well; none has run yet; `update: false` may pin an old toolchain. |
| Fuzzing & sanitizers | **8** (new) | 11 real targets, Windows-aware build, reproducible crashes; the decoders used for preview are not fuzzed. |
| Dependencies & patches | **9** (9, 7) | Patch recorded and re-applied; 55/55 digests match; the line-ending refresh should be its own commit. |
| Layout | **9.5** (9, 9) | No cross-module `detail/` include, now enforced by the graph check; the test accessor left the production tree. |
| Tests & coverage | **8** (7, 6) | Four UI controllers and every new component tested; ASan 17/17; the analysis supervisor and drag resolver are still only covered end to end. |
| Documentation | **8.5** (6, 7) | Linux, MSI and status claims now honest and cited; a few present-tense CI and fuzz claims are not yet observed. |
| Codebase-wide scans | **9.5** (9, 9) | `[[nodiscard]]` on 250/252 Result APIs; `catch (...)` only as commented thread barriers. |
| **Overall** | **8.5** (7.5, 6.5) | |

### Owner-rule compliance (`AGENTS.md`)

| Rule | domain | catalog | search | media | files | analysis | worker | audio | app | cli | ui |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 Libraries | Pass | Pass | Pass | Pass* | Pass | Pass | Pass* | Pass* | Pass | Pass | Pass |
| 2 Vendored only via patches | Pass | Pass | Pass | Pass | Pass | Pass | Pass | Pass | Pass | Pass | Pass |
| 3 Names, `///` | Pass | Pass | Partial | Pass | Pass | Pass | Pass | Pass | Pass | Pass | Pass |
| 4 Layout | Pass | Pass | Pass | Pass | Pass | Pass | Pass | Pass | Pass | Pass | Pass |
| 5 Tests | Pass | Partial | Pass | Partial | Pass | Pass | Pass | Pass | Partial | Pass | Partial |
| 6 Docs move with code | Pass | Pass | Partial | Pass | Pass | Pass | Pass | Pass | Partial | Pass | Partial |
| 7 Windows first | Pass | Pass | Pass | Pass | Pass | Pass | Pass | Pass | Pass | Pass | Pass |

\* Documented deviations whose stated reasons hold: hand-written container and tag parsers, the
worker's mutex-plus-deque queues, and the audio Seqlock, playhead history and mailbox coalescing
(now listed in `IMPLEMENTATION_STATUS.md:69-81`).

Search, rule 3 (Partial): a few internal helpers lack `///`. Rule 13 (bounded queues and caches):
Partial in catalog (`scan_frontier`) and app (reload queue, id map). Rule 16 (real results): Pass,
apart from three present-tense claims (§5.6).

---

## 3. Status of the second review

### Its fix-first table

| # | Second-review finding | Status | Evidence |
|---|---|---|---|
| 1 | `collection --delete` use-after-free (Critical) | Fixed | `--delete` unbound and read through `count()` (`src/cli/edit_commands.cpp:180, 198`). All option storage lives in `CommandContext::command_options` (`command_context.hpp:65-76`), and every binding in `src/cli` was audited. Test `cli_tests.cpp:287-311`. ASan-clean in `build/integ-asan`. |
| 2 | stb_vorbis setup overflow | Fixed (one gap: Fix first #3) | `patches/miniaudio/0001-…patch`, listed in `deps/manifest.json:128-130`, re-applied by `update_deps.py:181-189`. It reproduces the recorded digest. Tests `media_tests.cpp:2015-2097`, fuzz target `fuzz_stb_vorbis.cpp`. |
| 3 | Navigator menu use-after-free | Fixed | `navigator_panel.cpp:465-469` reads a `NodeInfo` by value before `exec()`. No `QTreeWidgetItem*` survives a nested loop. Test `ui_application_tests.cpp:157-226`. |
| 4 | Skipped entries become missing | Fixed (cost: Fix first #1) | `importer.cpp:485-492, 520-530`, `library_scan.cpp:758-800`. Tests `import_tests.cpp:469-516`. |
| 5 | Lease query O(N²) | Fixed (measured) | Migration 6 `jobs_ready` plus `INDEXED BY` (`library_analysis.cpp:557-561`). Plan has no temp B-tree: 0.002-0.027 ms per lease at 100k jobs. |
| 6 | Index offset cursors | Fixed | Keyset `CursorKey`, `partition_point`, per-stream resume (`sample_index.cpp:1133-1143, 1368-1384, 1484-1498`). Tests under rating and BPM edits (`app_tests.cpp:700-770`). |
| 7 | STALE_CURSOR truncated the view to 400 rows | Fixed (new High: Fix first #2) | Anchored refresh over every loaded row (`sample_table_model.cpp:326-353, 933-940`). Tests `ui_tests.cpp:1061, 1103, 1128`. |
| 8 | SampleIndex memory unbounded | Fixed (estimate caveat below) | 1M-row cap with SQLite fallback (`application.cpp:345-365`), compact rows (`sample_index.cpp:58-216`), deviation recorded (`IMPLEMENTATION_STATUS.md:29-41`). |
| 9 | RowMatcher per-row `path_key` | Fixed | Stored `l.path_key` in `SampleRow` (`sample_rows.cpp:172, 262`), rules memoized per root and per taxonomy id (`row_matcher.cpp:164-224`). Benchmark asserts under 15 ms at 100k when run by hand. |
| 10 | Linux CI parked, docs say it passed | Fixed (Windows legs added, Linux deferred) | Windows `ci-debug` and `ci-sanitize` legs (`ci.yml:55-58`); every statement corrected (`DEVELOPING.md:33-36`, `IMPLEMENTATION_STATUS.md:89, 241-242, 487-490`). |

### Second-review Mediums

All fixed except these two:
- **Index memory measured with best-case names (§4.9).** The 248-265 B/row figure comes from synthetic
  names short enough for libc++'s small-string buffer. Real pack names allocate.
- **Backups still cannot be cancelled (§4.11).** Exports now run on their own pool and observe
  shutdown, but exit waits for a running backup.

The Mediums fixed in this round include:
- `finish_scan` frontier index (14 ms vs 62 s), paused-run settling, artifact algorithm versions,
  corrupt-job skipping;
- alias resolution for merged tags;
- partial-selection refusal, per-library write ordering, the `rows()` error channel, the removed
  re-select;
- history reloaded on demand, navigator reads coalesced and ordered;
- inspector Format line and drafts, preview device recovery and the engine thread contract;
- declick on every epoch change, re-anchor at the consumed position, the federated merge resume;
- reload no longer holding the sync lock, close and reopen with a job running, read-only upgrade
  after a CLI run;
- the stuck `play` loop, WiX per-machine scope, shipped third-party licenses;
- sanitizer-aware worker tests on Windows (not needed: the job cap coexists with ASan), the four
  untested UI controllers, `[[nodiscard]]`.

One security-only Low was dropped under the owner's priorities: the lock file can be deleted while
it is held (`file_lock.cpp`). The Linux-only items are in §8.

---

## 4. Findings by module

Format: **[Severity]** `file:line`: defect. *Why it matters.* → Fix. Each module opens with what
remains open from the second review, if anything.

### 4.1 domain: 9/10

All second-review findings fixed: simple-uppercase keys without normalization on case-insensitive roots
(`core.cpp:115-135`), `kFoldVersion` bumped and checked on open (`library_lifecycle.cpp:409-474`), domain
links trimmed, `format_duration` boundary.

- **[Low] (possible)** `src/domain/core.cpp:115-135`: simple uppercase maps ſ→S, ı→I, µ→Μ and ς→Σ. If
  NTFS's `$UpCase` table keeps any of those pairs apart, two real files get one key (and the rekey
  collision path in §4.2 then applies). → Confirm against Windows ordinal ignore-case comparison and pin
  the pairs in a test.
- **[Nit]** `format_duration` truncates 9.995-9.999 s to "0:09" but rounds 10.0 s to "0:10".

### 4.2 catalog: 8/10

Every second-review High and Medium is fixed and was plan-checked:
- the lease walks `jobs_ready` with no sort;
- the `finish_scan` gap subquery uses `scan_frontier_state` and is dropped when there are no gaps;
- paused runs settle and their waiting jobs park as `paused`, with migration 6 converting old ones;
- algorithm versions travel with their facts;
- corrupt jobs fail and the walk continues;
- conflicting undos store a receipt and no-op edits create no action;
- case-only renames are re-spelled; `ensure_folder` is iterative; probes run without a snapshot;
- old paused sessions are not resumed; integrity checks run on a reader; empty folders are pruned;
- `prepare` no longer allocates.

Migrations 1-5 are byte-identical and all six SHA-256s match the pinned values. Partly fixed: the FTS5
`integrity-check` still runs on the writer.

- **[High] (verified, measured)** `src/catalog/library_scan.cpp:772-775`: see Fix first #1. *Every scan of
  a root with cloud-only files or links pays O(skipped × locations) under the writer lock.*
- **[Medium] (possible; traced by reading)** `src/catalog/library_lifecycle.cpp:449-474`: when rebuilt path
  keys collide, the rekey rolls back and the library opens writable with old-scheme keys, while runtime
  code produces new-scheme keys (`library_scan.cpp:133`). The next scan of a case-insensitive root:
  - `find_location_by_path` misses every file;
  - `try_relink` drops each candidate because its old path still exists;
  - every file is therefore inserted as a new sample;
  - `finish_scan` marks the originals, with their tags, ratings and notes, missing.

  *One colliding pair duplicates the library.* → Resolve collisions inside the rekey (commit the rest,
  disambiguate the colliding rows, add a review item), or open read-only and refuse scans until resolved.
- **[Low]** `library_history.cpp:765-775, 797-803`: regression from the no-op fix. A chunked edit whose
  first chunks change nothing commits a `running` receipt without an action. A retry after a crash in
  that window replays it as partial and never edits the rest. → Resume from `next_offset` when the
  receipt has no action.
- **[Low]** `library_analysis.cpp:537-540, 695, 708`: jobs of a paused run go back to `queued` or
  `interrupted` on lease reclaim, transient failure and release. → Use the same
  `CASE run.state WHEN 'paused'` as `unblock_reachable_jobs`.
- **[Low]** `library_scan.cpp` (`finish_scan`, `begin_scan:645-651`): `scan_frontier` rows are never
  deleted, so the table grows one row per directory per scan (invariant 13). → Delete them for completed
  and cancelled sessions.
- **[Low]** `library_lifecycle.cpp:422-441`: the first open after upgrade rekeys every folder and location
  twice in one transaction, with no progress and no backup (seconds at 1M rows; start-up looks hung). →
  Report progress or chunk.
- **[Nit]** `library.hpp:347-348` wedges a doc comment inside `friend`; the `AnalysisFacts` doc overstates
  invalidation (flags stay set); `dump_json(...).substr(0, 200)` can split UTF-8 (`patch_operations.cpp:79-87`).

### 4.3 search: 9/10

All second-review findings fixed:
- RowMatcher per-row cost;
- merged-tag aliases in SQL and memory, with chain tests;
- `ceiling_divide`;
- taxonomy validation, with stored-query errors counted as library errors;
- relevance retry of failed hydration;
- one evaluability rule;
- one compiler per expansion;
- an index-driven `folder:` filter;
- bounded `matching_ids`;
- three new parity tests.

Parity was re-checked field by field (numbers, flags, codec, key, available, folder, NOT, unknowns).
There is still no injection path.

- **[Medium] (possible)** `src/search/search_service.cpp:489-503, 553-563`: if one library's hydration
  keeps failing (key fetch succeeds, `load_rows` fails, e.g. a damaged page), every page returns a
  `next` cursor retrying the same rows. `mso search` loops forever (`cli/search_commands.cpp:115-129`), and
  export spins until cancelled. → Count retries per library in the cursor and give up after one, or stop
  when a page hydrated nothing and only failed libraries remain.
- **[Low]** `query_compiler.cpp:444-463`: `primary_location_filter` probes up to library/16 index entries
  on every compile of a `folder:` value or folder scope, so a broad folder pays it on every page. →
  Cache the plan choice per (library, revision, condition).
- **[Low] (cross-slice)** `src/app/sample_index.cpp:412-430`: the index copies `tag_paths` and recomputes
  `path_key` for verbatim-path rows per row before matching. → A matcher entry point over ids and stored
  keys.
- **[Low] (possible)** `src/ui/main_window.cpp:710-721`: the UI saves smart-collection text without
  `SearchService::validate` (only the CLI validates), so an unparsable query later shows as DB_CORRUPT.
- **[Nit]** `folder:"/ Drums"` keeps the leading space; category text without an id re-folds per row;
  missing `///` on `compile_filter`, `LibraryFilter`, `clamp_page_size`, `TaxonomyPaths::Node`;
  `IMPLEMENTATION_STATUS.md:300` still calls `folder:Loops` an 86 ms known follow-up.

### 4.4 media: 8.5/10

All second-review findings fixed:
- the stb_vorbis patch: `size_t` setup sizes with a 128 MiB cap, lengths checked against bytes left,
  codebook bounds, a leak, `lookup1_values` in double;
- FFmpeg EOF versus read error;
- exact-length contracts (FFmpeg Vorbis is an estimate; MP3 trim matches dr_mp3);
- a CPU deadline for `find_stream_info`;
- a 16 KiB read-ahead window;
- WavPack tags, rates and channels;
- `native_path` from the absolute path;
- the nits.

Partly fixed: each `read_exact` still returns its own `std::vector`; implicit-SBR HE-AAC still reports the
core rate (documented).

- **[Medium] (verified)** `deps/miniaudio/extras/stb_vorbis.c:758-759, 4158, 4188-4193`: see Fix first #3.
- **[Low] (latent)** `stb_vorbis.c:1360-1375, 3712, 3724, 3736`: in push mode `stream_bytes_left` sees only
  the bytes pushed so far, so a long comment (cover art) now fails `invalid_setup` instead of
  `need_more_data`. The app uses pull mode only, so nothing breaks today. → Set `f->eof` in push mode.
- **[Low] (possible)** `stb_vorbis.c:1367`: in FILE mode `stream_bytes_left` calls `ftell` per comment; a
  crafted header with millions of empty comments could take seconds to open. → Compute once before the
  loop.
- **[Low]** `src/media/decoder_vorbis.cpp:52-58`: stb_vorbis turns an `fread` failure into EOF, so on the
  default Vorbis path a drive that drops out ends preview silently (the FFmpeg path was just fixed for
  this). → Check `ferror()` on the stdio handle on a short read.
- **[Low] (possible)** `ffmpeg_input.cpp:49-52`: `read_error_` is sticky; after one transient read failure
  every later EOF reports that stale error. → Clear on success or in `seek()`.
- **[Low]** No tests for the resampler drain on a mid-stream change, EOF-with-read-error, or
  `frames_exact() == false` for Vorbis through FFmpeg.
- **[Nit]** `bounded_reader.cpp:54-56`: scattered reads refill 16 KiB each but are charged only their own
  bytes (disk traffic can exceed `max_bytes`; `media.hpp:68` says "slightly more"); a successful
  `find_stream_info` just past the deadline is reported as `ProbeLimit`; `deps/README.md:99-104` omits
  part of the patch.

### 4.5 files: 8.5/10

Second-review High and Medium fixed with targeted tests: skipped files are marked seen and skipped
directories become gaps; `StoredStatCache` loads each directory once (8-directory LRU). Also fixed:
duplicate links (global visited set), setup exceptions, the cross-module include (public
`media/native_file.hpp`), surrogate round-trip, visible skips in tests. Partly fixed: identities are read
in parallel, but unchanged files are still opened; dedup is still untested; `ScannedFile::file_id` is
still a `std::string`.

- **[Medium]** `src/files/file_identity.cpp:45-51, 120-130` with `importer.cpp:224-226`: see Fix first #8.
- **[Low] (per-entry hot path on OneDrive roots)** `importer.cpp:219-226, 432, 520-530`: skips are sent
  before the extension filter, so every cloud-only or link *non-audio* file (documents, DAW projects)
  becomes a `mark_scan_entries_seen` path, which is the High in §4.2 multiplied. → For files, send the
  skip only when `has_candidate_extension`.
- **[Low]** `importer.cpp:42, 386-391`: with DFS interleaving, a subtree touching 8 or more directories
  evicts the parent's map, which then reloads in full. → Key the LRU on the directories open on the
  enumeration stack.
- **[Low]** `importer.cpp:488`: each skipped directory records its gap as its own transaction. → Batch
  with `pending_directories_`.
- **[Nit]** Roots with any unfollowed link now always report `complete_coverage == false` (by design);
  the UI wording should not read as an error.

### 4.6 analysis: 9.5/10

All second-review findings fixed (`hypot` removed from the per-frame path, duplicate stages refused,
unused link removed, docs). Kept on purpose: pocketfft's internal scratch allocation (documented). Passes
39/39 under ASan on Windows.

- **[Low] (measured)** `src/analysis/byte_hash.cpp:25-30`: libc++ maps ERROR_BAD_NETPATH (server
  unreachable) to "not found" with no error code, so only ERROR_BAD_NET_NAME becomes SOURCE_OFFLINE.
  Measured with a scratch probe. It changes nothing in practice, because `read_file_stamp` already
  returns FILE_MISSING first and the catalog treats both the same. → Classify by `GetFileAttributesW`'s
  error code in both places, or drop the distinction.
- **[Nit]** `spectrum.cpp:75`: float cast of a double above FLT_MAX is formally undefined (gives inf on
  IEEE).

### 4.7 worker: 9/10

All Windows-relevant findings fixed with fault-injected tests:
- `kTaskStopDeadline` plus `_Exit` on every exit path;
- `waveform_segment` bins bounded by the task;
- handshake limits compared;
- CLI11 help to stderr;
- single progress throttle;
- a Win32 `cancel_blocked_read` test.

Passes 36/36 under ASan on Windows inside the 2 GiB job cap.

- **[Low]** `tests/worker_tests.cpp:744-761`: only the Shutdown path of the stuck-task exit is tested; the
  stdin-EOF and protocol-violation exits are not.
- **[Nit]** `kTaskStopDeadline` (2 s) equals the default `shutdown_grace` (2 s), so the worker's own exit
  races the supervisor's terminate (same outcome; only the exit code differs). → Use 1 s.

### 4.8 audio: 8/10

Every second-review finding fixed, each with a test:
- unconditional `cut_audio()` on epoch change;
- re-anchor at the consumed position for shape changes;
- epoch copied before adoption;
- `configure()` drops other-rate blocks;
- loop minimum after inexact EOF;
- generation bookkeeping;
- the deviations listed;
- `Overloaded` deduplicated.

The callback path was re-traced: no allocation, lock, I/O, logging, exception or unbounded loop. The new
output supervisor runs every backend call on one thread (COM balanced), tears down in the right order and
never runs two callbacks at once.

- **[Medium] (verified by reading miniaudio 0.11.25)** `output_device.cpp:184-189`, `output_supervisor.cpp:181-183`:
  see Fix first #4.
- **[Medium] (possible)** `output_device.cpp:184-189`: loss detection relies only on miniaudio's "stopped"
  notification, which is skipped when the backend stop fails (`miniaudio.h:42842-42849`), as after an
  unplug of an explicitly chosen endpoint. *The engine stays "Playing" in silence with no Retry.* → A
  watchdog on the output thread: a callback counter that has not advanced for about 1 s while started
  means lost.
- **[Low]** `playback_controller.cpp:404, 407-414`: the consumed re-anchor can still repeat or skip one
  callback period. The gate is published before the first new-epoch block is committed, and a callback
  can run in between. `IMPLEMENTATION_STATUS.md:206` ("nothing is replayed") overclaims. → Commit the
  first block before publishing the gate.
- **[Low]** `playback_renderer.cpp:22-41`: a same-rate device switch first plays old-epoch blocks from the
  old consumed position, then jumps back. → Mark every current epoch superseded in `configure()`.
- **[Nit]** A spurious underrun after a rate switch; sub-millisecond spin in `time_until_next_wake`
  (moodycamel truncates to ms); `guarded_device_request` lacks `catch (...)`; `requests_` deque is
  bounded only by blocking callers.

### 4.9 app: 8/10

Second-review Highs fixed:
- **Memory:** cap, compact rows, deviation recorded.
- **Cursors:** keyset, per-stream resume.

Mediums fixed:
- reload no longer holds the sync lock while streaming;
- close gives the job back and reopen waits, then returns a retryable DB_BUSY;
- read-only libraries are retried every 2 s and upgraded.

Every Low fixed: workspace reload, backoff predicate, lease-error throttle, flushed export, generation-safe
id cache, `remember_library`, thread docs, shared JSON helpers. Dropped as security-only: lock-file
tampering.

Partly fixed:
- An `all_samples` edit (more than 10,000 samples) still returns before the index reflects it.
- The memory figure comes from best-case names.

Not fixed:
- `State` is about 380 lines.
- Supervisor deadline, abandon and backoff have no tests.
- The JSON export envelope is spliced.

- **[Medium] (verified by reading)** `src/app/application.cpp:300-306, 382-385, 454-457, 472-503`: see Fix
  first #5.
- **[Medium]** `src/app/sample_index.cpp:743-772`: see Fix first #6.
- **[Medium] (possible; estimate)** `src/cli/bench_commands.cpp:36-48`, `sample_index.cpp:191-193, 893-909`:
  the recorded 248-265 B/row uses 12-16-byte synthetic names that fit libc++'s small-string buffer. Real
  names (25-60 bytes, often non-ASCII) allocate, roughly +100-190 B/row, and `fresh.rows` is never
  reserved (1.5× during growth). *"About 520 MiB at the cap fits 768 MiB" may not hold on real libraries.*
  → Benchmark with realistic names, keep row text in one arena, reserve, consider a byte cap.
- **[Low] (possible)** `sample_index.cpp:46-48, 616-631, 514-523`: the id-to-slot vector and id bitmaps are
  sized by the largest sample id (up to 2^31), outside the row cap. A sparse or huge rowid commits
  gigabytes, and a `bad_alloc` on the `apply_edit` path escapes. → Tie the id limit to the row limit.
- **[Low]** `sample_index.cpp:896-897`: a cancelled load still streams every row (the `for_each_row` visitor
  cannot stop). → Let it return `bool`.
- **[Low]** `sample_index.cpp:847-856`: the previous index is freed while the sync mutex is held. → Destroy
  it after release.
- **[Low]** `application.cpp:104, 360-365, 482`: `index_disabled` is permanent; a library pushed over the
  budget stays on SQLite after the other library closes. → Re-check on close and on the poll.
- **[Low] (possible)** `sample_index.cpp:1237-1248, 1425, 1488`: a library incomplete on one page and
  complete on the next has no stored position and restarts at row 0. → Treat it as STALE.
- **[Low]** `docs/DEVELOPING.md:120-126`, `IMPLEMENTATION_STATUS.md:186-187`: the docs say *closing* waits
  10 s (it is *opening* that waits) and that cursors go stale "only when rows are added or removed"
  (taxonomy changes also do).
- **[Nit]** `SampleIndex::loading()` is `noexcept` but locks and is unused; the 300 vs 260 MiB comment
  mismatch; tag sets not counted in the cap.

### 4.10 cli: 8.5/10

The Critical is fixed at the root and every binding audited. Also fixed:
- `play` stops on Stopped or at the known length plus 2 s;
- the stall limit is the job cap plus 5 min;
- `analyze` stops only when the queue is paused;
- a second Ctrl+C reaches the default handler;
- `backup` takes the lock;
- new tests for interrupts, timeouts, exit 3 and `play`;
- `value_parsing` split out.

Not fixed: `current_runs` lists 1,000 runs; `analyze` with a query still collects every id.

- **[Low]** `src/cli/search_commands.cpp:96-97, 114, 122-123, 138-144`: `--json` builds the whole result as
  one JSON DOM, and `--limit` accepts 10,000,000 (gigabytes at 1M rows). → Stream the array.
- **[Low] (possible)** `media_commands.cpp:290-294`: the end limit trusts estimated lengths (VBR MP3 without
  a seek header), so a longer file is cut. → Apply it only to exact durations.
- **[Nit]** The coverage warning repeats on every page; the binding test's comment says it exercises every
  flag but skips five; `bench --rows` above 1M cannot measure the index.

### 4.11 ui (core): 7.5/10

Second-review High fixed (anchored refresh over every loaded row, layout change moving persistent
indexes by key). Mediums fixed:
- partial selections refused;
- writes queued per library;
- `rows()` returns `Result`;
- the re-select removed;
- history reloaded on demand;
- exports on their own pool.

Lows fixed: the pending-edit overlay on every install path, taxonomy through `Application`, obsolete
generations stopped, teardown disconnects, shortcut scope per key, per-event costs trimmed, preferences
read on the pool, drag `path_of` map. `AnalysisController` and `LayoutController` were extracted, so
MainWindow is now 828 lines. Partly fixed: a running backup still delays exit.

- **[High] (verified code; livelock possible)** `src/ui/sample_table_model.cpp:326-353` with
  `main_window.cpp:506-510, 522-524`: see Fix first #2.
- **[Medium]** `sample_table_model.cpp:362-364` with `334, 748`; `edit_controller.cpp:75-76`:
  `refresh_rows` deliveries are dropped when the generation moves, and a refresh moves it both at its
  start and at its commit. *An analysis result or an edit's settle read arriving during a walk is lost,
  so the old value stays or a saved rating flickers back.* → Apply row deliveries by key regardless of
  generation, or re-request the keys read during a refresh after the commit.
- **[Medium]** `sample_table_view.cpp:204-209`, `main_window.cpp:662-681`: the partial-selection refusal is
  avoidable. Pages keep keys for every covered row, but `selected_keys()` returns only hydrated rows, so a
  Ctrl+A over more than 2,000 rows ("search kick, select all, press 5") is always refused. → Build the
  selection's keys from `Page::keys`; keep the refusal only for drag and copy, which need paths.
- **[Medium] (possible; Qt internals)** `sample_table_model.cpp:780-797`: the layout change is emitted
  without `QAbstractItemModel::VerticalSortHint`, so a 20,000-row Shift-selection holds about 340,000
  persistent indexes (rows × 17 columns), rebuilt on every refresh. The commit also builds a `std::map`
  over every common row. → Pass the hint; look up only the persistent rows' keys.
- **[Low]** `sample_table_model.cpp:785, 792`: when the result grows, persistent indexes past the old row
  count become invalid (selected rows pushed down by an import drop out). → Insert the extra rows before
  the layout change.
- **[Low]** `sample_table_model.cpp:93, 853-856` vs `733-742`: between stale retries a visible evicted page
  can hydrate with a stale cursor and refresh immediately, skipping back-off.
- **[Low]** `edit_controller.cpp:75-82`: if the reconcile read fails after a failed write, rows keep the
  failed prediction although the message says it "was undone on screen".
- **[Low]** `sample_table_model.cpp:271-274` vs `347-348`: appended pages and `seen_keys_` are unbounded
  while a refresh cuts at 500,000 rows. → Stop `canFetchMore` at the cap.
- **[Low]** `main_window.cpp:547-550`: a linear `library_info()` search plus a copy per selected sample on
  every selection update.
- **[Low]** No test drives MainWindow's selection refusal, `refresh()` while refreshing, or the 500 ms
  window path.
- **[Nit]** `IMPLEMENTATION_STATUS.md:199-200` says partial selections "ask the user to wait" (the code
  refuses); `layout_controller.cpp:46-47` skips the header restore when the dock restore fails; the
  "Select the samples to analyze first" message also shows when the session is not ready; `restart()`
  keeps `staged_keys_`.

### 4.12 ui (panels): 8.5/10

Every second-review High and Medium fixed:
- **Navigator:** use-after-free fixed by value capture, reads coalesced (one in flight per library, a
  1 s interval and a generation).
- **History and inspector:** history read on demand; the inspector Format line kept.
- **Preview:** device recovery with Retry and an Output menu, and the single-thread engine contract.

Every Low fixed:
- **Preview:** the "Opening…" status cleared; waveform-only generation.
- **Inspector:** drafts per field, focus-independent reset, Clear notes, stars, multi-arg tag text,
  builders split.
- **Jobs and history:** closed libraries' rows removed; Undo enabled only with something to undo.
- **Other panels:** locale formatting, asynchronous file manager, plain-text labels, menus split.

Deferred (Linux): `QFile::decodeName`.

- **[Medium] (verified)** `src/ui/preview_panel.cpp:249, 258, 310, 437, 451, 474, 490`: see Fix first #7.
- **[Low]** `preview_panel.cpp:248-251`: Space after a device loss resumes and then pauses at once. Restore
  re-plays and the queued `TogglePlayPause` then pauses. → Post `Play` when the snapshot is not Playing.
- **[Low] (possible)** `navigator_panel.cpp:134-153, 183, 194`: a full reload's older outline can overwrite
  a newer per-library one, and the tree stays stale until the next change. → Re-mark dirty libraries
  whose read started after the full read.
- **[Low] (possible)** `inspector_panel.cpp:725-736`: a focused collection combo keeps the previous
  library's ids when the selection moves to another library, so "Add to" can target the wrong collection.
  → Always refill on a library change.
- **[Low]** `inspector_panel.cpp:556-559`: a multi-selection with different manual BPMs shows "Value" with
  the previous sample's number and Apply enabled. → Show Mixed.
- **[Low] (perf)** `inspector_panel.cpp:571-588`: `reload_library_choices()` runs on every structure change
  of any library (about 6.7/s during import). → Ignore other libraries; coalesce like the navigator.
- **[Low] (possible)** `navigator_panel.cpp:298-303, 323-327`: a parent cycle in a damaged catalog builds
  items that are never attached (a leak on every rebuild; no crash).
- **[Low]** Device loss and engine-creation failure are untested (the null device is never lost).
- **[Nit]** Rich-text tooltips from user text; one shared library timer; `library_reloads_` keeps closed
  libraries; run states untranslated; chained `.arg()` in `provenance`; a conditional emoji CHECK; a long
  comment line; a redundant clip region.

---

## 5. Infrastructure

### 5.1 Build & CMake: 9/10

Fixed:
- WiX `perMachine` (`Packaging.cmake:39`);
- third-party licenses installed (`InstallThirdPartyLicenses.cmake`, `THIRD_PARTY_NOTICES.txt` with source
  links);
- the GUI-target guard;
- stale paths, the version define, doctest marked test-only, the README image;
- the include-graph check now rejects other modules' `detail/` headers.

Not fixed: no IPO/LTO option.

- **[Low] (possible)** `Packaging.cmake:39` with `CMakeLists.txt:1`: if `CPACK_WIX_INSTALL_SCOPE` needs CMake
  3.29, a 3.28 configure silently ignores it. → Raise the minimum or fail on older versions with WiX on.
- **[Low]** `cmake/DependencyNotices.cmake:77` embeds a literal CR byte, so git treats the file as binary,
  and an editor that saves it as LF would silently strip newlines from every license text. → Use `"\r"`.
- **[Nit]** `ASAN_SYMBOLIZER_PATH` decided at configure time (harmless; PATH covers it); pacman output
  parsed in English (`LC_ALL=C`); a long header line in `tests/fuzz/CMakeLists.txt`.

### 5.2 CI: 7.5/10

The design now matches the Windows-first plan:
- a `ci`, `ci-debug` and `ci-sanitize` matrix;
- a fuzz job time-boxed at 60 s per target, with crash artifacts uploaded;
- an MSI built with WiX 3.14.1 and uploaded;
- a pinned `windows-2025` runner;
- caches saved on failure.

The Linux job is parked with a truthful comment. **None of the new legs has run**: the last run is still
39233493989 on `6b5f1d4`.

- **[Medium] (possible)** `ci.yml:64-70, 151`: `update: false` with an `install:` list may install the
  package versions from setup-msys2's pinned snapshot (older clang, Qt and FFmpeg than the verified
  23.1.3, 6.11.2 and 9.0.2), or 404 once mirrors drop them, against `-Werror` legs. → Check the "Toolchain
  versions" step on the first run; switch to `update: true` or add `pacman -Sy` if they are old.
- **[Low]** `ci.yml:178`: fuzzing restarts from the checked-in seeds every run and the grown corpus is
  discarded (`stb_vorbis` reached 784 edges in 60 s locally). → Cache the corpus; add a longer scheduled
  run.
- **[Nit]** The comment says "five cached legs" (four are active); every run writes about 1.6 GB of new
  ccache entries.

### 5.3 Fuzzing & sanitizers: 8/10 (new)

- **Targets:** eleven libFuzzer targets.
  - seven probe families through `media::probe_memory`, which uses the same reader as `probe_file`;
  - `stb_vorbis`;
  - the worker protocol, with a decode→encode invariant;
  - the query parser plus RowMatcher;
  - the SQL and index cursor decoders, run on a real library.
- **Build:** ASan, UBSan and libFuzzer coverage on Windows, with flags spelled out because clang refuses
  `-fsanitize=fuzzer` for MinGW. `-fno-sanitize-recover=all` is set.
- **Runner:** time-boxed with an RSS limit and a hard kill. Crashes go to `fuzz-artifacts/` and are
  uploaded from CI; replay is documented.
- **Results:** four stb_vorbis defects found and fixed, with regression seeds; 11/11 clean in `run3.log`.
- **Windows sanitize preset:** 17/17 green in `build/integ-asan`. Its first run found the CLI
  use-after-free.

- **[Medium]** `tests/fuzz/`: the decoders the desktop process runs on every preview and waveform are not
  fuzzed. That covers dr_wav, dr_flac and dr_mp3 through `ma_decoder` (`decoder_miniaudio.cpp:186-189`),
  the FFmpeg decoder glue, and stb_vorbis's FILE path, which production uses
  (`decoder_vorbis.cpp:104` vs the target's `open_memory`). *A crash on a damaged WAV, FLAC or MP3
  during preview takes down the UI.* → A `fuzz_decoder` target over `ma_decoder_init_memory` per
  encoding, decoding a bounded number of frames, plus a FILE-based stb_vorbis variant.
- **[Low]** The seeds contain no `smpl`, `acid`, `cue `, `ds64`/RF64 or AIFF `MARK`/`INST` chunks, the very
  parsers the rule-1 deviation exists for. → One small seed per chunk type.
- **[Low]** `tools/run_fuzzers.py:12` says a leak fails the run; LeakSanitizer does not exist on Windows
  (the stb_vorbis leak was caught by the RSS limit). → Say so.
- **[Low]** Regression seeds are replayed only by the fuzz job. → A `-runs=0` CTest entry in fuzz builds.
- **[Nit]** `fuzz_cursor.cpp:50-78` leaves its temporary library in `%TEMP%` on every crash.

### 5.4 Dependencies & patches: 9/10

The stb_vorbis patch is recorded in the manifest and applied by the script. Applied to the LF baseline,
it reproduces the working-tree file byte for byte. 55/55 digests match. The CRLF refresh is done (44
files to LF; one upstream-CRLF file remains). `ref` is required; the non-atomic staging copy is
documented.

- **[Low]** Commit hygiene: the 44-file line-ending refresh (about 170k changed lines under `deps/`, which
  is `-text`) and the patch touch the same file. → Commit the refresh first, then the patch, so the
  patch stays reviewable.
- **[Low] (possible)** `stb_vorbis.c:953`: the comment cites GHSL-2023-165 to 172, part of which concern
  decode-time reads rather than setup. → Check the list and narrow the comment.
- (The push-mode Low is in §4.4.)

**Line endings:** the "LF will be replaced by CRLF" warnings come from the system `core.autocrlf=true`
and are harmless. Every tracked text file is LF in the index, and outside `deps/` there is no
line-ending diff. The only file with mixed endings is `cmake/DependencyNotices.cmake` (§5.1).

### 5.5 Tests & coverage: 8/10

The four UI controllers are now tested, and so are the output supervisor, `value_parsing`, domain
`json_fields` and `native_file`. ASan + UBSan pass 17/17 on Windows. Silent skips are now real skips;
`std::system` was replaced by `_wsystem`.

| Module | Component | Tested? |
|---|---|---|
| domain | all, incl. `json_fields` | yes |
| catalog | lifecycle, migrations 1-6 (pinned), history, scans, outbox, analysis queue, backups, reader pool, durable file, SQLite | yes |
| catalog | patch operations, edit command, taxonomy lookup | partial (through the Library API) |
| search | parser, compiler, service, paging, federation, RowMatcher parity, sample rows | yes, plus fuzz |
| media | every probe, bounded reader, `probe_memory`, tags, file handle, `native_file` | yes, plus 7 fuzz targets |
| media | miniaudio, Vorbis and FFmpeg decoders | yes (fixtures); **not fuzzed** |
| files | importer, file identity, gaps, skips, link cycles | yes (dedup and WOF untested) |
| analysis | every stage | yes |
| audio | engine, building blocks, output supervisor, wakeup signal | yes |
| audio | real output device, device loss | partial (null and offline backends) |
| worker | protocol, supervision, Win32 platform | yes, plus fuzz |
| app | application, workspace, sample index, file lock, export | yes, plus cursor fuzz |
| app | analysis supervisor | partial (through `Application`) |
| cli | every command, `value_parsing` | yes |
| ui | model, delegate, view, edits, undo, inspector, jobs, history, search bar, preferences, layout, waveform, preview, dialogs, session, five controllers | yes |
| ui | navigator, drag path resolver, main window | partial (end to end) |

- **[Low]** No unit tests for `AnalysisSupervisor` (deadline, abandon, backoff) or `DragPathResolver`.
- **[Low]** `worker_tests.cpp:608` still has a generous wall-clock upper bound.

### 5.6 Documentation: 8.5/10

Fixed: the Linux CI narrative, the status header (names the real commits, says the round is
uncommitted), the MSI claims ("Verified so far…"), the cursor and memory deviation, "fuzz-tested" (now
true), the search SKILLS. Not fixed: the `docs/COMMAND_LINE.md` synopsis still omits `--move-to-top`,
`--limit`, `--raw`, `--absolute-paths`, `--clear-category`, `--operation`, `--restore` and others.

- **[Low]** `IMPLEMENTATION_STATUS.md:58-59` ("CI repeats the 60 s runs on every push") and
  `docs/DEVELOPING.md:20-31` describe the new CI legs in the present tense, but none has run. → Add
  "(configured; not yet run)".
- **[Low] (possible)** `docs/BUILDING.md:61`: "point `WIX` at" the unzipped folder. CPack looks for
  `$WIX/bin/candle`, and the zip has no `bin/`. → Say `WIX` is the parent of `bin/`, or drop that route.
- **[Low]** The stale statements already listed: close vs open waiting (§4.9), "nothing is replayed"
  (§4.8), "ask the user to wait" (§4.11), `folder:Loops` 86 ms (§4.3), 46 vs 44 CRLF files.
- **[Nit]** "`[[nodiscard]]` on every Result" misses two private `Workspace` members; a SKILL tells the
  reader to set `ASAN_SYMBOLIZER_PATH` by hand (CTest does it); `THIRD_PARTY_NOTICES.txt` points at
  `share/licenses/` even when nothing was installed there.

### 5.7 Codebase-wide scans: 9.5/10

| Pattern (over `include/` and `src/`) | Count | Notes |
|---|---|---|
| TODO / FIXME / HACK / XXX | 0 | |
| Files with `_WIN32` | 24 | logic blocks all have POSIX branches |
| `<windows.h>` | 13 | all in `.cpp`, all guarded |
| raw `new` outside UI | 9 + 7 `new (std::nothrow)` | all into `unique_ptr` except the miniaudio VFS handle |
| raw `new` in UI | ~215 | Qt parent ownership |
| `delete` | 4 | VFS handle (2), `QTreeWidgetItem` (2) |
| `catch (...)` | 5 | all commented thread barriers after typed catches (new: `output_supervisor.cpp:108`, `playback_controller.cpp:72`) |
| typed `catch` | 62 | |
| regex / sprintf / strcpy / rand | 0 | |
| C-style casts | 0 | 623 `static_cast`, 21 `reinterpret_cast`, 1 `const_cast` |
| `(void)` discards | 11 | each commented |
| `[[nodiscard]]` on Result-returning header declarations | 250 of 252 | missing: `workspace.hpp:110, 113` (private) |
| headers without `#pragma once` / with `using namespace` | 0 / 0 | 133 headers |
| duplicate helpers | 1 pair left | `media::UniqueWin32Handle` vs `worker::UniqueHandle` (the latter also wraps fds; justified) |

---

## 6. Cross-cutting themes

1. **Partial-index predicates.** The new High (§4.2) is a query that cannot use the partial index it was
   written for. Check every statement on `locations` and `jobs` with `EXPLAIN QUERY PLAN` in a test,
   just as `search_tests.cpp:1130-1175` already does for the folder filter.
2. **Refresh, reload and patch loops on busy sessions.** Three of the Mediums share one shape: work
   restarted or repeated while the previous run is still going.
   - the anchored table refresh (§4.11);
   - index reloads queued without dedup (§4.9);
   - `patch_orders` re-merging every order (§4.9).

   One rule fixes them: at most one in flight plus a dirty flag, and incremental updates.
3. **Blocking the UI thread on device work.** The output supervisor made audio robust, but its
   synchronous API is now called from UI handlers (§4.12). Keep the supervisor API async for UI callers.
4. **Fuzz what the UI decodes.** Parsers are fuzzed; decoders run in the UI process are not (§5.3).
   stb_vorbis showed that this is where crashes hide (§4.4).
5. **Present-tense claims about configured but never-run CI.** These are small now (§5.6), but under
   rule 16 they should say "configured; not yet run" until the first green run.

---

## 7. Suggested order of work

1. **Stability on Windows:**
   - add the `submap` bounds to the stb_vorbis patch, plus a seed (§4.4);
   - make default-device changes and failed stops reliable in loss detection (§4.8);
   - make the preview's device calls asynchronous (§4.12);
   - treat WOF-compressed files as ordinary files (§4.5).
2. **Hot paths:**
   - `AND state<>'retired'` on the revive statement, with a plan test (§4.2);
   - filter skips by extension (§4.5);
   - refresh dedup with a dirty flag in the table (§4.11);
   - reload dedup and incremental `patch_orders` in the index (§4.9);
   - `VerticalSortHint` (§4.11).
3. **Correctness:**
   - row deliveries applied by key (§4.11);
   - selection keys from `Page::keys` (§4.11);
   - collision-safe rekey (§4.2);
   - endless paging under a persistent hydration failure (§4.3);
   - the chunked no-op receipt (§4.2).
4. **Bug catching:**
   - a decoder fuzz target and chunk-type seeds (§5.3);
   - unit tests for `AnalysisSupervisor` and device loss (§5.5);
   - confirm the first CI run's toolchain versions (§5.2).
5. **Commit and docs:**
   - commit the deps line-ending refresh separately from the stb_vorbis patch (§5.4);
   - fix the literal CR (§5.1);
   - mark CI claims "not yet run", and fix the stale statements and the CLI synopsis (§5.6).

---

## 8. Deferred (Linux)

Out of scope by the owner's decision (Windows first):
- **Linux CI:** the parked job and its symbolizer.
- **Worker:** sanitizer-aware POSIX memory-cap tests, the PDEATHSIG header contract, tests for the POSIX
  error pipe, PDEATHSIG and fds 0-2, and `ScopedSigpipeBlock`.
- **CLI:** the POSIX process group for Ctrl+C.
- **Build:** Linux `INSTALL_RPATH` and the libc++ probe.
- **Other POSIX-only items:**
  - `QFile::decodeName` round-trip;
  - the POSIX `durable_file` report after rename;
  - POSIX `classify_entry` following the link target.
