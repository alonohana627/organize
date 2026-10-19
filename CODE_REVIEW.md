# Code review: Music Sample Organizer

**Reviewed:** 2026-10-19, working tree of branch `phase/p1-browser` (the code is almost entirely uncommitted;
another session was editing it during the review, so line numbers can drift by a few lines).
**Scope:** every file under `include/`, `src/`, `tests/`, plus `CMakeLists.txt`, `CMakePresets.json`,
`cmake/`, `deps/` (manifest, `update_deps.py`, CMake), `patches/`, `.github/workflows/ci.yml`, and the docs
(`README.md`, `docs/BUILDING.md`, `deps/README.md`, `IMPLEMENTATION_STATUS.md`, `SKILLS/`). About 35,000
lines of project code and tests.
**Method:** read-only. Nothing was built or run. Each module was read in full by a dedicated reviewer.
Findings were checked against the code, and every Critical and High finding marked ✔ was re-checked by
hand against the source before it went into this report. "Possible" means the reasoning is sound but the
behaviour was not reproduced.
**Yardstick:** the owner's binding rules in `AGENTS.md` (libraries over hand-rolled code, patches for
vendored changes, clear names, `///` docs, header layout, tests for every component, Linux-ready platform
isolation, docs kept current) and the product invariants 9–16, plus general correctness, concurrency, error
handling, SQLite usage, performance, CMake/CI quality and UI code.

---

## 1. Summary

**Overall: 6.5 / 10.** The architecture is good and in places excellent: a clean `Result<>`-based error
model, bound parameters everywhere (no SQL or FTS5 injection path found), an RAII SQLite layer with a
statement cache and `BEGIN IMMEDIATE` retry, a real-time-safe audio callback, a hardened worker process
(job object, `close_range`, strict protocol), bounded probes with checked arithmetic, and a properly paged
Qt table model. The codebase-wide hygiene scan is close to spotless: no TODOs, no C-style casts, no
`catch (...)`, no `sprintf`, no `std::regex`, every header has `#pragma once`, and the header layout rule
holds everywhere.

What keeps it from 10/10 is a set of real bugs on the paths users hit most (closing a library, refreshing
the table during import and analysis, an offline drive, re-opening a library), a Linux build that cannot
compile today, CI that has never run and would fail on both platforms, and uneven doc comments and tests
(the CLI has no tests at all, and about 12 UI components have none).

### Fix first (the ten that matter most)

| # | Severity | Where | Problem |
|---|---|---|---|
| 1 | Critical ✔ | `src/app/application.cpp:294-296`, UI pool lambdas | `Application::library()` returns a raw pointer from a temporary `shared_ptr`; UI background tasks use it while `close_library` can destroy the library. Use-after-free. |
| 2 | High ✔ | `src/files/importer.cpp:254-255` | A `known_stats` error returns from `commit_batch` while submitted probe tasks still reference `scanned` and `stop` (BS futures do not wait). Use-after-free. |
| 3 | High ✔ | `src/media/decoder_ffmpeg.cpp:113-116, 219` | A mid-stream channel-count change rewrites `channels_`, and `read()` then `memcpy`s more floats than the caller's buffer holds. Heap overflow from an untrusted file. |
| 4 | High ✔ | `src/catalog/library_analysis.cpp:356-373` | `lease_next_job` takes `LIMIT 32` including `blocked` jobs, so one offline drive starves the whole analysis queue. |
| 5 | High ✔ | `src/app/application.cpp:257-262` | `open_library` runs `Library::open` (with crash recovery) before checking that the library is already open; re-opening resets the live session's jobs, scans and history. |
| 6 | High ✔ | `src/app/application.cpp:139-152` | `~Application` destroys the supervisor before joining import threads that call `supervisor->notify()`. Shutdown race. |
| 7 | High | `src/worker/main.cpp:303-318`, `src/app/analysis_supervisor.cpp:147-209` | Heartbeats come from an independent thread and a job has no deadline, so one file that hangs a decoder stalls analysis forever. |
| 8 | High ✔ | `src/worker/main.cpp:13-15, 115` | `<picosha2.h>` is included only under `#ifdef _WIN32` but used unconditionally: **mso-worker does not compile on Linux**. |
| 9 | High ✔ | `src/ui/sample_table_model.cpp:307-322` | `refresh()` during a pending `restart()` never clears `replacing_`, so paging stops at about 400 rows. Very likely during import (refresh every 750 ms). |
| 10 | High ✔ | `.github/workflows/ci.yml:17-29, 44-45` | Windows CI calls `python3` but installs no Python; Linux installs Qt 6.4.2 while CMake needs 6.5 and silently drops the GUI. README claims CI runs every suite on both platforms. |

### Findings by severity

| Severity | Count |
|---|---|
| Critical | 1 |
| High | 22 |
| Medium | ~85 |
| Low / Nit | ~150 |

---

## 2. Scorecard

| Module / area | Score | One-line verdict |
|---|---|---|
| domain | 7 | Sound utilities on utf8proc and stduuid; path keys always fold case and `\` (wrong on POSIX); `error.hpp` undocumented. |
| catalog | 6.5 | Excellent SQLite and history design; analysis-queue starvation, partial chunked edits replayed as success, exceptions escaping `Result<>`. |
| search | 7 | No injection paths, keyset paging done right; relevance sort drops valid OR matches, exact BPM equality useless. |
| media | 7 | Bounded, overflow-checked parsers with correct format details; FFmpeg heap overflow, MP4 `moov` budget, files locked during preview. |
| files | 6 | Right structure (bounded queue, pool, gaps); use-after-free on catalog error, POSIX identity has no inode, reparse/symlink handling fragile. |
| analysis (DSP) | 8 | Careful and well commented; pocketfft rebuilds its plan every frame, possible double-tempo bias, building blocks untested. |
| worker | 7 | Mature supervision and confinement; Linux compile break, hung-task blind spot, relative-path spawn bug on POSIX. |
| audio | 7 | Real-time path is clean; three ordering races, and unconditional declick fades that the spec forbids. |
| app | 5 | Clean facade, but re-open recovery, shutdown race, wrong-encoding workspace path, workspace lost updates, unbounded threads. |
| cli | 5 | CLI11 used well; `analyze` can hang forever, exceptions uncaught, silent usage errors, `std::stod`, no tests. |
| ui | 6 | Good async gateway and paged model; use-after-free, paging stall, global single-key shortcuts, refresh storm, 1,200-line MainWindow. |
| Build & CMake | 7 | Target-based, per-module libraries, SYSTEM includes, sound SQLite options; AUTOMOC leaks, hard-coded test list, no sanitizers or install. |
| CI | 4 | Right platforms and `-Werror`, but never run, and both jobs would fail or skip the GUI. |
| Dependencies & patches | 7 | Manifest, README and `deps/` agree (14/14), patch applied; tags not commit-pinned, `--check` cannot see hand edits, About box misses MIT notices. |
| Layout | 9 | Rule holds everywhere; internal headers sit in the public tree. |
| Tests & coverage | 6 | Core well covered with honest tolerances; CLI untested, half the UI untested, ffmpeg-dependent tests silently pass, sleep-based timing. |
| Documentation | 7 | Paths and symbols check out; CI claim false, stale test count. |
| **Overall** | **6.5** | |

### Owner-rule compliance

| Rule | domain | catalog | search | media | files | analysis | worker | audio | app | cli | ui |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 Libraries, never hand-roll | Pass | Pass | Pass | Partial | Pass | Pass | Partial | Partial | Partial | Pass | Partial |
| 2 Clear names | Partial | Partial | Pass | Pass | Pass | Partial | Partial | Pass | Pass | Pass | Pass |
| 3 `///` docs | Fail | Partial | Partial | Partial | Partial | Partial | Partial | Partial | Partial | Partial | Partial |
| 4 Layout | Pass | Pass | Pass | Pass | Pass | Pass | Pass* | Pass | Pass | Pass | Pass |
| 5 Tests | Partial | Partial | Partial | Partial | Partial | Partial | Partial | Partial | Partial | **Fail** | Partial |
| 7 Windows + Linux | Partial | Partial | Pass | Partial | Partial | Pass | **Fail** | Pass | Partial | Partial | Partial |

\* Worker headers (`mso::worker` namespace) live in `include/mso/analysis/`.
Rule 2 (vendored code via patches only): **Pass**: the one patch (`patches/csv_parser/0001-crlf-record-terminator.patch`)
is listed in the manifest and applied by `update_deps.py`; `.gitattributes` keeps `deps/**` byte-exact.
`update_deps.py --check` cannot detect a hand edit, though (see §5.3).
Rule 6 (docs move with code): **Partial**: see §5.6.

---

## 3. Cross-cutting themes

These recur across modules; fixing each once removes a dozen findings.

1. **Borrowed raw pointers across threads.** `Application::library()` and `open_libraries()` hand out
   `catalog::Library*` that dies on `close_library`. `~ReaderLease` calls back through a raw `Library*`.
   Fix once: a `with_library(uuid, function)` that holds the `shared_ptr` for the call, and leases that hold
   shared ownership of the pool.
2. **Hang detection is missing end to end.** The worker's heartbeat thread proves the process is alive,
   not that the task is progressing (`src/worker/main.cpp:303-318`). The supervisor renews leases forever
   (`analysis_supervisor.cpp:147-209`). The CLI `analyze` waits forever (`src/cli/media_commands.cpp:18-35`).
   docs/04_CONTRACTS.md:111 requires "cooperative cancel, then worker termination". Fix once: a task
   progress counter in the heartbeat, plus a per-job no-progress deadline that cancels, then kills.
3. **Event storms instead of coalescing.** Every analysis tick emits two progress and two catalog-changed
   events (`application.cpp:174-179`, `analysis_supervisor.cpp:89,139`). The UI then re-reads up to 10
   pages, rebuilds the navigator, re-reads the inspector detail and re-checks 1,000 files on disk, and the
   inspector overwrites what the user is typing. Fix once: throttle and coalesce per library at the
   Application layer, and make each UI consumer diff before writing.
4. **Exceptions escaping a `Result<>` API.** Throwing `nlohmann::json::parse`, const `operator[]` (UB on a
   missing key), `.get<T>()`, `value()` on wrong types, `std::filesystem` throwing overloads, and jthreads
   without a try barrier. Seen in catalog (history, observations, checkpoints), search (cursor decode),
   app (workspace load, export `dump()`), audio (`PlaybackController::run`), files (`is_symlink`), ui
   (`takeResult()`), cli (`main` catches only `CLI::ParseError`). Fix once: a project helper for
   non-throwing JSON field reads, `error_code` overloads everywhere, and a `catch (const std::exception&)`
   barrier at the top of every thread body.
5. **Discarded errors.** 32 `(void)` casts on `Result`s; about 16 of them drop catalog job-state
   transitions (`analysis_supervisor.cpp:106,114,123,141,150,165,171,203,206`;
   `application.cpp:167,188,193,273,323,468,488`). SQLite `bind*` return codes are ignored
   (`sqlite.cpp:115-138`). Fix: log or surface them; keep `(void)` only with a comment saying why.
6. **POSIX correctness has never been exercised.** Case-folded path keys, `\`→`/` on POSIX, missing inode
   identity, `is_symlink` that throws, `RLIMIT_AS` against sanitizers, a relative-path spawn bug,
   `__clang_major__`, a SIGINT handler that is not async-signal-safe, and the worker compile break. Linux
   is "prepared" but would not build or behave correctly.
7. **Unbounded growth despite invariant 13.** `catalog_outbox` never pruned, backups never pruned, history
   never pruned, operations map and finished jthreads kept until exit, one jthread plus one probe pool per
   import, sample-id vectors for whole roots, jobs panel rows, the event bridge's queued lambdas.
8. **Duplicated constants and helpers.** Analysis flag bits in three places (catalog, search parser, UI),
   `kMaximum*` vs `kMax*` limits duplicated between analysis and worker, three little-endian serializers,
   two `Overloaded` helpers, two `escape_like`, two Win32 handle RAII classes, four test temp-directory
   and four WAV-writer helpers.
9. **Internal headers in the public tree.** `catalog_internal.hpp`, `patch_operations.hpp`,
   `worker_platform.hpp` and four audio headers that include PRIVATE deps (`<miniaudio.h>`,
   readerwriterqueue) sit next to the public API. Adopt `include/mso/<module>/detail/`.
10. **Doc comments are strong on file headers and public classes, thin on internals.** The worst gaps are
    the most-read headers: `include/mso/domain/error.hpp`, `include/mso/catalog/sqlite.hpp`,
    `migrations.hpp`, the `Library` class with ~25 public methods, and the whole of
    `include/mso/media/media.hpp`, all of which use `//` or nothing.

---

## 4. Findings by module

Format: **[Severity]** `file:line`: problem. *Why it matters.* → suggested fix.

### 4.1 domain (`include/mso/domain`, `src/domain`): 7/10

Strengths: `fold_key` and `fold_path_key` on utf8proc (NFC plus simple lowercase, so "ﬁ" and "fi" never
merge); UUIDs from stduuid with `random_device`; `path_to_utf8`/`path_from_utf8` centralise conversions.

- **[High] ✔** `src/domain/core.cpp:62-93` with `src/catalog/library_scan.cpp:65-68, 370`: location identity
  is always the case-folded `path_key`, and `roots.case_sensitive` is always written as 0 and never read.
  `fold_path_key` also turns `\` into `/`. *On a case-sensitive Linux volume, `Kick.wav` and `kick.wav`
  become one location; every scan then reports a content change, bumps the revision and invalidates
  analysis. `a\b.wav` collides with `a/b.wav` (legal on POSIX).* → Detect case sensitivity per root, store
  it, use an exact key on case-sensitive roots; convert separators only under `#ifdef _WIN32`.
- **[Medium]** `include/mso/domain/error.hpp` (whole file): `ErrorCode`, `Error`, `Error::message` and
  `fail` use `//` or nothing. *Rule 3, on the most-included header in the project.* → `///` on each.
- **[Low]** `src/domain/core.cpp:129-144`: `is_safe_relative_path` does not reject `:` (alternate data
  streams), reserved device names (`CON`, `NUL`, `COM1`) or segments with trailing dots or spaces.
  *Possible path confusion on Windows.* → Reject them.
- **[Low]** `src/domain/core.cpp:37-41`: on malformed UTF-8, `fold_key` keeps raw bytes without folding,
  so `"KICK\xFF"` and `"kick\xFF"` differ. `parse_key` (191-218) does not accept `♯`/`♭`.
- **[Low]** `tests/domain_tests.cpp`: no tests for `error.cpp`, `count_code_points`, `is_valid_utf8`, the
  `path_to_utf8`/`path_from_utf8` round-trip, `trim`, `parse_key_mode`.
- **[Nit]** Empty leftover directory `src/domain/cli/`. `trim`, `to_lower_ascii` and `parse_key_mode` lack
  `///`.

### 4.2 catalog (`include/mso/catalog`, `src/catalog`): 6.5/10

Strengths: per-connection statement cache with `SQLITE_PREPARE_PERSISTENT`; `WriteTransaction` rolls back
by default; `BEGIN IMMEDIATE` with jittered bounded retry; WAL plus `synchronous=FULL`; reversible,
compare-and-set history patches that record only real changes; chunked bulk edits with a durable cursor;
lease tokens, cancel epochs and content-revision fences on analysis; invariant 14 honoured (one writer
mutex, one transaction, revision plus outbox plus projection together).

**High**
- **[High] ✔** `src/catalog/library_analysis.cpp:356-373`: `lease_next_job` selects
  `state IN ('queued','interrupted','blocked') … LIMIT 32`, then skips unreachable rows in C++. *If the first
  32 candidates are on an offline root, every call returns nothing and every later job, in every run,
  starves. The desktop queues analysis automatically, so one unplugged drive stops the whole queue.* →
  Move the reachability `EXISTS` into the `WHERE`, exclude `blocked`, and unblock jobs in
  `set_root_availability`/`finish_scan`.
- **[High] ✔** `src/catalog/library_history.cpp:580-583, 627-631` with `find_receipt` (78-92): each chunk
  commits a receipt in state `running`. If chunk N>1 fails, earlier chunks stay committed, the action stays
  `applying` (so `undo()` refuses it until a restart), and a retry with the same operation UUID gets the
  partial receipt back with `replayed=true`. *The caller is told the edit succeeded; the remaining samples
  are never edited.* → When the stored receipt is `running`/`partial`, resume from the next chunk or report
  `partial`; mark the action `partial` on error, not only after a crash.

**Medium**
- `src/catalog/library_history.cpp:753-761, 719-726`: an undo that hits `HISTORY_CONFLICT` leaves the action
  `applied` at the cursor. *Nothing older can ever be undone, and each retry adds another identical review
  item.* The schema's `'conflict'` state (`migrations.cpp:532`) is never set. → Mark the action `conflict`,
  move the cursor past it, add the review item once.
- `src/catalog/patch_operations.cpp:173, 314, 334-335`; `library_history.cpp:690`;
  `catalog_internal.cpp:325-326`; `library_analysis.cpp:382`; `library_queries.cpp:121`: stored JSON is
  read with const `operator[]` (UB on a missing key), throwing `.get<T>()` and throwing `json::parse`.
  `kHistoryCommandVersion` is stored but never checked. *A corrupt or older row throws out of a `Result<>`
  API or invokes UB.* → `contains`/`value()`, `parse(text, nullptr, false)`, check the version, return
  `DB_CORRUPT`.
- `src/catalog/library_analysis.cpp:28-101, 347-352, 383`: analysis facts are not range-checked (BPM,
  confidence 0..1, pitch class 0..11, sha256 length and hex, waveform segment path). *A bad value fails a
  `CHECK`, the job stays `running` until reclaimed, and reclaim never applies `kMaxAttempts`, so it can loop
  forever (possible).* → Validate facts, fail permanently on a bad value, cap attempts on reclaim.
- `src/catalog/library_analysis.cpp:427-428`: `complete_job` writes `facts.sample_id` and
  `facts.content_revision` from the caller instead of checking them against the lease, contrary to the
  header. → Reject a mismatch with `INPUT_CHANGED`, or take them from the lease.
- `src/catalog/library_analysis.cpp:451`: in `fail_job`, `if (!fenced) return {};` turns every
  `verify_lease` error (busy, I/O, not found) into success. → Swallow only `CANCELLED`.
- `include/mso/catalog/library.hpp:217-219`: `writer_connection()` and `writer_mutex()` are public. *Anyone
  can write without the mutex, revision or outbox, which breaks invariant 14.* Only tests use them. → Make
  them private with a test-only friend.
- `src/catalog/library_lifecycle.cpp:156-158, 300-303`: `~ReaderLease` calls `owner_->release_reader`
  through a raw pointer; nothing stops a lease outliving the `Library`. → Shared ownership of the pool, or
  assert in `~Library`.
- `src/catalog/library_queries.cpp:150-285`: `sample_detail` runs five statements with no read transaction.
  *Under WAL each gets its own snapshot, so a concurrent commit gives a torn view.* → A `ReadTransaction`
  RAII (`BEGIN`/`COMMIT`) for multi-statement reads.
- `src/catalog/catalog_internal.cpp:124-169`; `library_scan.cpp:453-462`; `patch_operations.cpp:356-360`:
  `refresh_search_projection` is ~6 statements per sample plus a recursive CTE per tag, run inside the
  writer transaction for a whole root or subtree rename, unchunked. `merge_tag` (`library_history.cpp:490-507`)
  builds one unbounded chunk. *At 100k samples, around a million statements while holding the only
  writer.* → Set-based SQL (`group_concat` over joins), and chunk taxonomy actions like edits.
- `src/catalog/sqlite.cpp:115-138`: every `bind*` ignores SQLite's return code. *A wrong index silently runs
  with NULL.* → Record the first bind error and return it from `next_row`/`run`.
- `include/mso/catalog/library.hpp:118-120` vs `src/catalog/library_scan.cpp:590-605`: the header says
  unseen locations are marked missing only with complete coverage; the code marks them missing without it,
  excluding only gap subtrees. → Decide which is intended; fix code or doc.
- `src/catalog/migrations.cpp:667-680` with `tests/catalog_tests.cpp:71-83`: checksums are computed from the
  current source, and the only test tampers with the database. *An accidental edit to a released migration
  passes every test and only breaks users' existing libraries (invariant 15).* → A doctest that pins the
  released SHA-256 values.
- `src/catalog/catalog_internal.cpp:109-122`: `catalog_outbox` is written on every mutation but never read
  or pruned; `consumer_cursors` is unused. → Prune below the minimum cursor, or cap.
- `src/catalog/library_lifecycle.cpp:24-40`: `write_file_atomically` never fsyncs the temp file or the
  directory before `rename`, and leaves `.tmp` behind on failure. *After a power loss `library.json` can be
  empty and the library unopenable.* → `FlushFileBuffers`/`fsync` behind `#ifdef _WIN32` with a POSIX
  branch.
- Tests: no direct tests for the SQLite wrapper (cache, moves, busy path, `backup_to`), the reader pool,
  upgrading an older schema, opening a newer schema read-only, `finish_scan` with gaps, `rename_root`,
  `set_root_availability`, lease expiry and reclaim, blocked jobs, `fail_job` backoff, `renew_lease`,
  category-rename undo. The bulk-edit test assumes sample IDs are `1..N`.

**Low**
- `library_history.cpp:439-464`: `move_tag` does not check that the new parent exists or is live, and the
  depth check ignores the moved subtree's own depth (the depth-16 CTEs then truncate paths silently).
  `patch_operations.cpp:281-284` maps every `InvalidArgument` to `HISTORY_CONFLICT`. `merge_tag`, `rename`
  and `rename_collection` do not check the target is live.
- `patch_operations.cpp:217-220`: `write_collection_link` reports `DB_BUSY`/`DISK_FULL` as
  `HISTORY_CONFLICT`. → Map only `SQLITE_CONSTRAINT_UNIQUE`.
- `library_scan.cpp:600-601, 251-252`: `LIKE … || '/%'` without escaping `_` and `%` (common in sample
  names); possible-move search ignores the root. → Prefix compare with `substr()` or `ESCAPE`.
- `library_scan.cpp:134`: `try_relink` probes the filesystem while holding the writer inside
  `BEGIN IMMEDIATE`; an offline network volume blocks all writers.
- `library_scan.cpp:43-44, 533`; `types.hpp:109`: unsafe or invalid-UTF-8 paths are dropped without a count;
  the 500-file batch limit is not enforced; `probe_error` is never persisted.
- `library_lifecycle.cpp:162-205`: library creation is not atomic (each migration commits separately; DB
  created before `library.json`); subdirectory creation errors are ignored.
- `library_lifecycle.cpp:249`: a read-only open of an older schema does not migrate, so later queries can
  fail at prepare time (possible). `verify_migration_checksums` (100-112) does not detect version gaps.
- `library_analysis.cpp:269-303`: `control_run` splits SQL on `;` by hand, never returns `NOT_FOUND`, bumps
  the revision when nothing changed.
- `library_analysis.cpp:226, 246-249, 312, 329`: duplicate sample IDs count as queued; `skipped` means two
  different things in two calls; stage list stored under a dummy `'bundle'` stage.
- `library_history.cpp:734, 788`: `undo`/`redo` mint a fresh operation UUID and store no receipt, so a
  retried undo reverts two actions; redo marks a `partial` action `applied`.
- `library_history.cpp:719-726, 745-746`; `library_lifecycle.cpp:305-317`; `library_scan.cpp:123-129`:
  swallowed errors (`record_history_conflict` returns `void`; a SQLite error becomes `NOT_FOUND`;
  `identity()` returns an empty name; `uuid::from_string(…).value_or(nil)` hides corrupt UUIDs).
- `library_queries.cpp:64-91`: `ensure_tag`/`ensure_category` bump the revision when the node already
  existed; implicit creations in `apply_edit` (`library_history.cpp:183-201`) are not undone.
- `sqlite.cpp:379-383`: `backup_to` retries `BUSY` forever. `sqlite.cpp:248, 310-323`: `busy_timeout` (2 s)
  inside a 2 s retry loop doubles the real wait.
- `library_lifecycle.cpp:326-344`: `acquire_reader` waits without a timeout; a thread that already holds a
  lease and asks for another deadlocks when the pool is exhausted. → `wait_for` plus `RESOURCE_LIMIT`.
- `library_lifecycle.cpp:355-372`: backups never pruned; `reason` unsanitized in the file name; two backups
  in the same millisecond collide.
- `edit_command.cpp:476-481`: add/remove overlap compares whole-path fold keys, missing `"Drums / Kick"`
  vs `"Drums/Kick"`.
- `library_history.cpp:117-120`: every new action runs an unindexed `UPDATE … WHERE state='undone'`;
  history is never pruned.

**Nit**
- Performance: `StatementCache::take` allocates a `std::string` per lookup (`sqlite.cpp:56-62`; use
  transparent hashing); `load_tree` folds keys inside the sort comparator (`library_queries.cpp:57-58`);
  `advance_revision` re-prepares via `execute_script` (`catalog_internal.cpp:110-115`; use a cached
  `UPDATE … RETURNING`); integers formatted into SQL with `std::format` bypass the cache
  (`library_scan.cpp:192-196, 590-591`).
- Dead or odd code: `inverse_json` stored but never read (`library_history.cpp:138-151`); unused
  `std::string sql;` (`patch_operations.cpp:114`); `Connection` move constructor calls `make_shared` inside
  `noexcept` (`sqlite.hpp:107-110`); `describe()` names only the first tag and does not match its
  documented example (`edit_command.cpp:502-527`).
- Rule 3 gaps: `sqlite.hpp` (`PreparedStatement`, `ConnectionOptions`, `Connection`, `WriteTransaction`,
  `make_sqlite_error`), all of `migrations.hpp`, the `Library` class and ~25 public methods in
  `library.hpp`, `types.hpp` `to_string`/`parse_*`, `patch::` builders, `ActionRecorder::finish/attach`.
- Rule 2: `v`, `m` (`migrations.cpp:262-268, 669, 675`), `c` (`library_history.cpp:263`), `a`/`b`
  (`library_queries.cpp:58`); one-letter SQL aliases throughout.

### 4.3 search (`include/mso/search`, `src/search`): 7/10

Strengths: every value bound; sort columns and FTS columns from whitelisted enums; FTS5 terms always
emitted as quoted strings with doubled quotes (`query_compiler.cpp:262-272`), so `NEAR`, `^`, `*`, `:` are
literal; LIKE escaped with `ESCAPE '\'`; locale-free `std::from_chars`; bounded parser (4,096 code points,
depth 16, 256 nodes); `(value, id)` keyset paging with a null partition, federated merge; cursor
fingerprinted with picosha2.

- **[High] ✔** `src/search/search_service.cpp:446-470`: relevance sort ranks only rows matching the OR of the
  positive text terms, then applies the full filter. *`kick OR tag:Drums` drops every row matching only
  `tag:Drums`; `kick OR NOT snare` drops every non-snare row without "kick"; `relevance_window_limited`
  stays false.* → LEFT JOIN the bm25 subquery over the filtered set (NULL score ranks last) when the query
  has a non-text OR branch, or fall back to the metadata sort and say so.
- **[Medium]** `query_compiler.cpp:72-74`, `query_parser.cpp:483-485`: `bpm:120` compiles to
  `bpm_milli = 120000` and `duration:2s` to exact microseconds. *Analyzed values like 120.18 never match.*
  → Equal means "within the precision typed" (BETWEEN).
- **[Medium]** `search_service.cpp:478-490, 325-385, 525`: rows are hydrated (location, tags, category per
  row) before they are discarded: up to 1,000 rows × libraries per relevance page, `page_size` × libraries
  per metadata page. → Fetch base columns, merge or slice, hydrate the final page once.
- **[Medium]** `query_compiler.cpp:204-205, 215`: `lower(fr.name)` and `lower(name)=lower(?1)` are
  ASCII-only, while the user value goes through utf8proc. *`folder:` and `collection:` with non-ASCII case
  differences do not match.* → Compare stored folded keys.
- **[Medium]** `query_compiler.cpp:55-59, 70-94, 146-153`: SQL three-valued logic leaks; `NOT bpm:>120`
  excludes unknown BPM, so `X` and `NOT X` do not cover the library. → `COALESCE(pred, 0)` or document it.
- **[Medium]** `search_service.cpp:118-131, 516-524`: the cursor fingerprint omits the set of libraries; a
  library opened mid-scroll starts from its first row. → Include sorted library UUIDs.
- **[Medium]** `tests/search_tests.cpp`: the injection test (95-100) accepts "error or empty"; untested are
  key/rating/format/added/sample-rate sorts, cross-page ordering (only uniqueness checked), relevance
  pagination, stale and malformed cursors, every `Scope` field, smart collections, column searches,
  embedded quotes, LIKE `_`/`%`, unit suffixes, and the OR-plus-relevance case.
- **[Low]** `search_service.cpp:405-418`: `decode_cursor` uses const `operator[]` and throwing `get`; a
  malformed token can crash instead of returning `STALE_CURSOR`.
- **[Low]** `search_service.cpp:472, 593-595` vs 526-531: inconsistent error policy between relevance,
  metadata and count paths.
- **[Low]** `search_service.cpp:137-156`: `Scope::folder` silently ignored without `root_id`.
- **[Low]** `search_service.cpp:143-148`: duplicate of `escape_like` (`query_compiler.cpp:20-27`).
- **[Low]** `query_compiler.cpp:233-244`: smart-collection expansion depth-capped but not size-capped
  (exponential SQL growth possible).
- **[Low]** `query_compiler.cpp:235-236`, `search_service.cpp:170-171`: `value("text", …)` throws on a
  non-string; parsing duplicated.
- **[Low]** `query_parser.cpp:320-321`: a quoted phrase containing `*` is rejected.
- **[Low]** `query_parser.cpp:503`: integer fields round fractions (`rating:3.5` → 4). → Reject.
- **[Low]** `query_parser.cpp:433-436`: `analyzed:false` is an error; flag constants (60-64) duplicate the
  catalog's bits.
- **[Low]** `query_parser.cpp:276-288`: depth limit not applied to `NOT` chains (~2,000-frame recursion
  possible in debug).
- **[Low]** `search_service.cpp:395-397, 495-496`: `uuids::to_string` twice per comparison inside
  `stable_sort`; compare UUIDs directly.
- **[Nit]** `kick AND AND snare` accepted; lone `)` gives a vague error; `"!!!"` gives an empty phrase;
  `relevance_window_limited` set at exactly 1,000 rows; unused `<variant>` in `query.hpp:21`; misplaced
  trailing comments at `query.hpp:33/38/49`.
- **[Nit]** Missing `///`: `query.hpp:52, 94-96`; `search_service.hpp:20-21, 74, 91, 111-112`; the
  `Parser` and `QueryCompiler` internals; `query_fingerprint`, `decode_cursor`.

### 4.4 media (`include/mso/media`, `src/media`): 7/10

Strengths: `ByteCursor` with a sticky failure flag, `checked_add`/`checked_multiply`, 128-bit
`units_to_microseconds`, and `BoundedFileReader` that charges the budget before allocating; every chunk walk
is monotonic and capped, with no recursion. Format details checked and correct: RIFF pad byte, RF64 ds64,
Wave64 alignment, AIFF 80-bit rate, ID3v2 syncsafe and unsynchronisation (v2.3 and v2.4), extended header,
footer, UTF-16 BOMs and surrogates, MP4 64-bit and size-0 boxes, QuickTime v1/v2 entries. FFmpeg locked
down: custom AVIO, `io_open` refused, audio-only demuxers, correct cleanup on every path. Source files are
opened read-only.

- **[High] ✔** `src/media/decoder_ffmpeg.cpp:104-122, 219-220`: `read()` recomputes the channel count from
  `channels_` on every pass, and `configure_resampler()` overwrites `channels_`/`sample_rate_` when a frame's
  layout changes. *A stream whose channel count rises mid-stream (AAC PCE change, joined streams, crafted
  file) makes `memcpy` write past the caller's buffer: a heap overflow from untrusted input.* → Fix the
  output layout and rate after the first frame and let swr remix to it, or fail with `CorruptAudio`.
- **[Medium]** `probe_ogg.cpp:106-110`: the tail scan accepts any `OggS\0` without a CRC or size check, so a
  stray pattern in packet data yields a bogus granule reported as an exact duration. → Verify CRC32 and
  that the page fits.
- **[Medium]** `probe_mp4.cpp:353-357`: the whole `moov` (up to 16 MiB) is read in one call against a 4 MiB
  budget. *Any M4A with large cover art in `udta` gets `ProbeLimit`, no codec, and then cannot be
  previewed.* → Walk `moov` children with header reads; load only `trak/mdia/…/stsd` and `ilst`; skip `covr`.
- **[Medium]** `decoder_miniaudio.cpp:64`, `decoder_vorbis.cpp:27`: preview opens files via
  `_wfopen`/`_wfsopen`, without `FILE_SHARE_DELETE`. *While a file previews, Explorer or a DAW cannot rename,
  move or delete it, which contradicts `file_handle.hpp`; the comment at `decoder_vorbis.cpp:24` is wrong.*
  → `ma_decoder_init` with callbacks over `FileHandle`; for stb_vorbis, `CreateFileW` →
  `_open_osfhandle` → `_fdopen`.
- **[Medium]** `probe_wav.cpp`, `probe_aiff.cpp`, `probe_flac.cpp`, `probe_mpeg.cpp`, `id3.cpp`,
  `probe_mp4.cpp`, `probe_asf.cpp`, `probe_caf.cpp`: rule 1 judgement call. All container and tag parsing is
  hand-rolled, while vendored miniaudio already has dr_wav metadata (smpl, acid, inst, LIST/INFO), dr_flac
  metadata and dr_mp3 Xing/LAME parsing, and TagLib (LGPL/MPL, GPL-compatible) could cover ID3, MP4, ASF,
  APE and Vorbis tags through a custom stream over `BoundedFileReader`. Keeping probes bounded is a
  defensible reason, but it is undocumented. → Record the decision in `IMPLEMENTATION_STATUS.md` and the
  `add-audio-format` skill, or adopt TagLib.
- **[Medium]** `include/mso/media/media.hpp:19-86`: the whole public API (`ProbeLimits`, `EmbeddedTags`,
  `LoopPoint`, `ProbeResult`, `probe_file`, `Decoder` and its virtuals, `open_decoder`, `filename_hints`, …)
  uses `//` or nothing. → `///` with contracts (`read` buffer size, seek past end, thread affinity).
- **[Medium]** `probe.cpp:70`, `probe_wav.cpp:267`, `probe_aiff.cpp:104`, `probe_mp4.cpp:93`: tags are read
  in full up to 1 MiB, so cover art is read just to get a few text frames (tens of GB over 100k files); MP4
  text-decodes binary `covr` items. → Walk frame headers with small reads; skip APIC/GEOB/PRIV; decode only
  `ilst` types 1 and 2.
- **[Medium]** `tests/media_tests.cpp:734-876`: every FLAC/MP3/Vorbis/Opus/AAC/ALAC/WMA/WavPack/CAF test
  `return`s with a MESSAGE when `ffmpeg` is not on PATH and still passes. No tests for Wave64, Ogg
  FLAC/Speex, ADTS, VBRI, ID3v2.2, v2.4 extended header/footer, UTF-16BE, ID3v1, ASF DRM, MP4 v1/v2 and
  64-bit boxes, or hostile inputs outside WAV; helpers tested only indirectly; nothing asserts invariant 11
  (source bytes and mtime unchanged).
- **[Low]** `ffmpeg_input.cpp:103-112`, `probe_ffmpeg.cpp:40-60`: the read-callback error is checked only
  when open fails, so a starved probe returns as complete; no `avformat_find_stream_info`, so duration is
  never filled (the else branch is dead); FFmpeg metadata tags ignored.
- **[Low]** `decoder_ffmpeg.cpp:245-257` vs 278-283: `AVERROR_INVALIDDATA` skipped on send but fatal on
  receive; one corrupt frame ends preview.
- **[Low]** `decoder_ffmpeg.cpp:308`: a frame without a timestamp after seek is assumed to start exactly at
  the target.
- **[Low]** `id3.cpp:123`: v2.4 frame sizes always read as syncsafe; old iTunes files use plain sizes. → The
  TagLib/mutagen fallback heuristic.
- **[Low]** `probe_wavpack.cpp:45`: possibly wrong total for files over 2^32 samples (WavPack's
  `low + (high << 32) - high`).
- **[Low]** `probe_mp4.cpp:203`: rate from the 16.16 sample-entry field cannot exceed 65535 and shows the
  core rate for HE-AAC (possible). → Read `esds`/`srat`.
- **[Low]** `probe_asf.cpp:58, 132`: unknown format tags labelled `"wma"`; DWORD `WM/BeatsPerMinute`
  ignored. APEv2 tags not parsed anywhere. `probe_wav.cpp:221` misses Wave64 LIST/INFO;
  `probe_aiff.cpp:44` passes `in24`/`in32`/`23ni` through as opaque codecs.
- **[Low]** `decoder_vorbis.cpp:98`: stb_vorbis has 32-bit lengths and a fuzz-crash history, and runs in
  the UI process even when FFmpeg is available. → Prefer FFmpeg for Vorbis under `MSO_HAVE_FFMPEG`.
- **[Low]** `file_handle.cpp:114-124`: POSIX `open()` without `O_NONBLOCK` blocks forever on a FIFO; the
  directory check is by path after open (TOCTOU). → `fstat` and require `S_ISREG`.
- **[Low]** `file_handle.cpp:57-60`: no `\\?\` prefix, and only the GUI exe is `longPathAware`; long paths
  fail as `FileMissing` in `mso.exe` and `mso-worker.exe` (possible).
- **[Low]** API: `Decoder::read(float*, size_t)` → `std::span<float>`; seek past end behaves differently per
  backend.
- **[Nit]** `id3.cpp:256` reads the ID3v1.0 comment as 28 bytes, not 30; `id3.cpp:89` keeps the `((`
  escape; `tag_mapping.cpp:64` has only 80 genres; `parse_tag_key` lacks Open Key ("8m"); `probe_mpeg.cpp:156`
  variable `bits` holds microseconds × kbps; missing `<limits>` (`probe_caf.cpp`), `<string>`/`<utility>`
  (`ffmpeg_input.hpp`).
- **[Nit]** Missing `///` on most parser internals (`WaveParser::*`, `walk_chunks`, `find_box`,
  `parse_sample_entry`, `read_page`, `handle_frame`, `open_codec`, …). Names: `ext_lower` (`media.hpp:59`),
  lambda `c` (`filename_hints.cpp:88, 129`), `mtime_ns`.

### 4.5 files (`include/mso/files`, `src/files`): 6/10

Strengths: enumeration on its own thread into a bounded moodycamel
`BlockingReaderWriterCircularBuffer` with timed waits; probes in a bounded BS thread pool; nothing past the
cancellation fence is committed; directory listing failures become gaps, so files are never marked missing
due to permissions; Windows identity opens with `FILE_READ_ATTRIBUTES` and full sharing; unchanged files are
not re-probed. `scanned.reserve()` keeps task references stable while the batch grows (checked).

- **[High] ✔** `src/files/importer.cpp:254-255` (in the loop that submits tasks at 260): if
  `known_stats()` fails partway through a batch, `commit_batch` returns while already-submitted tasks hold
  `&file` (into the local `scanned`) and `&stop`. BS `submit_task` futures do not block on destruction.
  *Running tasks write into freed memory.* → Load the directory stats before submitting anything, or wait
  for all futures in a scope guard before every return.
- **[Medium]** `file_identity.cpp:85-93`: POSIX identity sets no `volume_id`/`file_id` (no rename tracking
  on Linux), ignores the `last_write_time` error, maps EACCES to `FileMissing`, and converts
  `file_clock` directly (not the Unix epoch on libstdc++). → `stat`: `st_dev`, `st_ino`, `st_mtim`.
- **[Medium]** `file_identity.cpp:97` with `importer.cpp:93`: POSIX `is_reparse_point` calls throwing
  `std::filesystem::is_symlink`; an `lstat` failure throws on the enumeration jthread and terminates the app.
  → `entry.is_symlink(error_code)`.
- **[Medium]** `importer.cpp:93`: every `FILE_ATTRIBUTE_REPARSE_POINT` entry is skipped on Windows, including
  OneDrive and dedup files. *Whole synced folders import as empty, with no gap and no count.* → Check the
  reparse tag; handle cloud placeholders explicitly; count skips.
- **[Medium]** `importer.cpp:95`, `file_identity.cpp:54`: with `follow_reparse_points`, no cycle
  detection, and `FILE_FLAG_OPEN_REPARSE_POINT` stats the link rather than its target. → Track visited
  (volume, file id) for directories; drop the flag when following.
- **[Medium]** `importer.cpp:330-335`: the enumerator watches only `enumeration_stop`. If the committer
  constructor or `run` throws, the jthread destructor joins a thread blocked on a full queue: deadlock. →
  Pass the jthread's token, or request stop in a scope guard.
- **[Medium]** `importer.cpp`, `file_identity.cpp:52`: long paths without `\\?\` in `mso.exe` become
  `FileMissing` (possible).
- **[Medium]** `tests/import_tests.cpp`: untested are permission-denied gaps, symlinks and junctions,
  cancellation mid-scan with resume, small batch sizes, probe-failure counting, catalog errors mid-batch
  (which would have caught the High), and `read_file_identity` itself.
- **[Low]** `importer.cpp:270`: `probe.wait()` drops task exceptions; the file is committed with no metadata
  and no error. → `.get()` and convert to `probe_error`.
- **[Low]** `file_identity.cpp:42`: `filename().string()` goes through the ANSI code page; Win32 error
  mapping disagrees with `media::detail::FileHandle`.
- **[Low]** `file_identity.cpp:36`: `(ticks - epoch) * 100` can overflow (UB) for crafted timestamps.
- **[Low]** `importer.cpp:68`: pending-directories `deque` unbounded for wide trees.
- **[Low]** API: `file_id` as `std::string` → `std::array<std::uint8_t,16>`; cancellation drops progress and
  summary; `ProbeLimits` not settable via `ImportOptions`; `volume_id` 16 vs 8 hex digits.
- **[Nit]** Duplicate Win32 RAII handle (`file_identity.cpp:17-31` vs media); `relative_utf8` is a no-op
  wrapper; missing `<chrono>`, `<cstddef>`, `<map>`; missing `///` on `Enumerator::*`,
  `ImportCommitter::*`, `ImportOptions`, `error_from_last_error`.

### 4.6 analysis DSP (`include/mso/analysis`, `src/analysis` minus worker files): 8/10

Strengths: one streaming decode feeds every stage, NaN/Inf cleaned once and reported; memory bounded by
block size and the musical window; `WaveformBuilder` coarsens adaptively; the waveform codec validates size,
flags, SHA-256 and ranges (tests flip every bit); DSP checked correct (Krumhansl-Kessler profiles, parabolic
peak interpolation, periodic Hann normalised by 2/Σw, unbiased autocorrelation via zero-padded FFT,
circular-mean tuning); canonical PCM hash; no exception crosses `analyze_file`.

- **[Medium] ✔** `src/analysis/spectrum.cpp:38-39`: `pocketfft::r2c` free function on every frame with
  `POCKETFFT_CACHE_SIZE` 0 (only `POCKETFFT_NO_MULTITHREADING` is set, `deps/CMakeLists.txt:41`). *The plan
  and twiddles are rebuilt and scratch allocated per frame (~86/s of audio), contradicting
  `spectrum.hpp:15-16`.* → Keep a `pocketfft_r<float>` plan as a member, or define `POCKETFFT_CACHE_SIZE`.
- **[Medium]** `tempo_estimator.cpp:166-185` (possible): `climb_octaves` accepts 2× and 3× tempi at 85% of
  the slower comb with no prior; dense hi-hat grooves at 85-95 BPM can read as 170-190. Tests cover only
  kick patterns. → Weigh climbs by `tempo_prior`; add 16th-hat and half-time regression fixtures.
- **[Medium]** `tests/analysis_tests.cpp`: no direct tests for `MagnitudeSpectrum`, `autocorrelation`,
  `nearest_power_of_two`, `SpectrogramStream`, `MusicalFeatureExtractor`; tempo and key never tested at
  48/96 kHz; only three keys tested.
- **[Low]** `key_estimator.cpp:26, 92-93` (possible): the 0.03 margin includes relative and parallel keys,
  so expect many "unknown key" results on real music.
- **[Low]** `spectrogram_stream.hpp:43-44`: `erase(begin, begin+hop)` memmoves per hop. → Ring buffer.
- **[Low]** `analyze_file.cpp:60-75`: byte hash and decoder each read the whole file (double I/O on slow
  shares).
- **[Low]** `analyze_file.cpp:31-34`: the `status` error is ignored, so EACCES reports as `FILE_MISSING`.
- **[Low]** `waveform_builder.cpp:38-41` (possible): bin width set from an inexact frame-count hint can stay
  too coarse for VBR files.
- **[Low]** Duplicated limits: `kMaximumMusicalSeconds` vs `kMaxMusicalSeconds`, `kMaximumWaveformBins`
  (1<<20) vs `kMaxWaveformBins` (65536), `kMaximumSampleRate` twice. Three little-endian serializers
  (`pcm_hasher.cpp:18-24`, `waveform_codec.cpp:44-87`, `worker_protocol.cpp:25-39`).
- **[Low]** `pcm_hasher.hpp:9`: public header includes `<picosha2.h>`. → Pimpl.
- **[Nit]** `waveform_builder.cpp:42` reserves up to 24 MiB for tiny files; no `catch (...)` in
  `analyze_file.cpp:97-103`; missing `///` on `parse_stage`, `LoudnessResult`, `AnalysisRequest`,
  `AnalysisReport`, `read_file_stamp`, `analyze_file_or_throw`, `StageSelection::from`; `kMax*` vs
  `kMaximum*`, `mtime_ns`.

### 4.7 worker (protocol, supervision, platform files, `src/worker/main.cpp`): 7/10

Strengths: robust framing (partial reads, length checked before allocation, garbage becomes
`PARSE_ERROR`, no paths in error text); strict schema decoding with `std::in_range` and finite floats;
handshake PID check, watchdog for handshake, lease and cancel deadlines, bounded event queue, late results
after cancel become `CANCELLED`. Win32: `CREATE_SUSPENDED` → job → resume, `KILL_ON_JOB_CLOSE`, memory and
verified CPU caps, explicit handle list. POSIX: `pipe2(O_CLOEXEC)`, everything prepared before fork, only
async-signal-safe calls after it, `close_range`, EINTR loops, SIGPIPE handled, zombies reaped.

- **[High] ✔** `src/worker/main.cpp:13-15, 115`: `#include <picosha2.h>` is inside `#ifdef _WIN32`, but
  `picosha2::hash256_hex_string` is used unconditionally, and none of the included `mso/` headers pull it in
  (checked). **mso-worker does not compile on Linux.** → Move the include out of the `#ifdef`.
- **[High]** `src/worker/main.cpp:303-318` with `src/analysis/worker_process.cpp:470-474`: heartbeats come
  from an independent `HeartbeatPump` thread and any frame renews the lease. *A task thread stuck in a
  decoder or on an offline share keeps the lease alive forever; only a frozen whole process is detected
  (the tests use SIGSTOP/SuspendThread). docs/04_CONTRACTS.md:111 requires cancel, then termination.* →
  Heartbeat carries a task progress counter; add a per-task no-progress deadline in
  `Session::overdue_deadline`.
- **[Medium]** `worker_platform_posix.cpp:317, 323, 338-339`: the child `chdir`s to the executable's parent,
  then `execv`s the original (possibly relative) path, so `build/mso-worker` resolves to
  `build/build/mso-worker`; a bare `mso-worker` (the fallback at :276) makes `chdir("")` fail. → Make the
  path absolute first.
- **[Medium]** `worker_protocol.cpp:205-237, 592-594`: `TaskResult` values are schema-checked but not
  range-checked (confidence, BPM, sha256 format, counts, waveform fields), unlike Progress. → A
  `validate_result()` symmetric to `validate_task`.
- **[Medium]** `worker_process.cpp:414-428`: the handshake ignores `algorithm_versions` and `self_test`; a
  stale binary gets its results stamped with current algorithm versions. → Require equality; refuse
  self-test unless asked.
- **[Medium]** `worker_platform_posix.cpp:324-325, 336, 218`: `RLIMIT_AS` breaks sanitizer builds and
  counts mappings; setting the hard limit fails with EPERM under a lower `ulimit -v` and surfaces as a crash,
  not `RESOURCE_LIMIT`; `memory_limit_applied = true` unconfirmed. → Clamp, set only the soft limit, report
  setup errors through a CLOEXEC error pipe, disable for sanitizer presets.
- **[Medium]** `src/worker/main.cpp:410-418`: command line parsed by hand while CLI11 is vendored (rule 1).
- **[Medium]** `worker_process.cpp:68-120, 137-167`: `FrameSender` and `EventQueue` are hand-written
  mutex+deque queues; the SPSC `readerwriterqueue` does not fit (multiple producers, drop-oldest), which is
  a legitimate reason but undocumented. → Vendor `moodycamel::ConcurrentQueue`, or record the exception.
- **[Medium]** `tests/worker_tests.cpp`: untested are a live worker sending garbage, a message before the
  handshake, a second or wrong-PID handshake, the handshake timeout, a hung task thread with heartbeats
  running, `write_file_atomically`, and the memory-cap path; `sleep_for(4500ms)` at :499; the file uses
  `TEST_SUITE("analysis")` (:245).
- **[Low]** `worker_platform_win32.cpp:161-174`: `CreatePipe` makes both ends inheritable before the
  supervisor end is cleared; a concurrent `CreateProcess(bInheritHandles=TRUE)` (QProcess) can inherit
  them. → Create non-inheritable and mark only the child end.
- **[Low]** `worker_platform_posix.cpp:328-341`: no signal-mask reset before exec; no `PR_SET_PDEATHSIG`
  on Linux, so a hung task survives a supervisor crash.
- **[Low]** `worker_platform_posix.cpp:331-332` (possible): if fds 0-2 were closed, `dup2` can clobber a
  pipe end. → `F_DUPFD_CLOEXEC` above 2 first.
- **[Low]** `worker_platform_posix.cpp:134-142`: blocking `waitpid` after SIGKILL can hang on a D-state child.
- **[Low]** Subprocess management hand-written on both platforms; reproc (MIT) would cover spawn, pipes and
  kill but not the confinement. → Record the decision.
- **[Low]** `worker_process.cpp:48-64`: `validate_options` does not check durations (a lease deadline under
  the 2 s heartbeat kills healthy workers).
- **[Low]** `worker_protocol.cpp:480-497`: no catch for `bad_alloc` or `format_error`, despite "no exception
  escapes". `worker_protocol.cpp:559-563`: `request_sequence` never validated.
- **[Low]** `src/worker/main.cpp:194`: `TaskRunner::start` joins the previous task thread on the protocol
  thread; a blocked final write stops Cancel/Shutdown handling. `main.cpp:344-345`: `__clang_major__` fails
  with GCC.
- **[Low]** `include/mso/analysis/worker_*.hpp` use namespace `mso::worker` in the analysis folder;
  `worker_platform.hpp` is internal but public. → `include/mso/worker/`.
- **[Nit]** Progress matched on job id, not `cancel_epoch` (`worker_process.cpp:396-399`);
  `parse_error_code` assumes `Internal` is the last enumerator (`worker_protocol.cpp:639-645`);
  `GetExitCodeProcess` unchecked (`win32:261`); `Overloaded`/`error_of` duplicated; no `@file` in
  `worker_protocol.cpp`/`worker_process.cpp`; undocumented `to_json`/`from_json` layer; `kMax*` names.
- **Observation (invariant 10):** `src/ui/preview_panel.cpp:340` calls `analysis::analyze_file` in the GUI
  for the waveform stage. Allowed for preview, but it links the full analysis entry point into the UI and
  lets a malformed file crash the UI. → A narrower preview-only waveform API.

### 4.8 audio (`include/mso/audio`, `src/audio`): 7/10

Strengths: the callback path (`OutputDevice::on_audio` → `PlaybackRenderer::render` →
`PcmRing::front/pop_front`) uses only preallocated moodycamel operations, atomics and arithmetic; no
allocation, locks, I/O, logging, virtual decoder calls or exceptions (invariant 12 holds). Exact integer
source clock (`source_position.hpp:30-64`); epochs invalidate stale audio; correct shutdown order; resampling
and folding through `ma_data_converter`, time-stretch through signalsmith; an offline backend gives 18
hardware-free end-to-end tests.

- **[High] ✔** `src/audio/source_stream.cpp:264-291` (with :260, :266, `playback_renderer.cpp:33`): every
  segment start gets a 5 ms fade-in, including play from frame 0 of a one-shot, and every loop pass gets a
  fade-out and fade-in at the seam, plus a transport fade on Play. docs/05_AUDIO_INTELLIGENCE.md §8: "Use
  short crossfades only as a selected preview option and show that they alter audition"; not listed as a
  deviation. *Drum attacks are softened in a sample auditioner, seamless loops dip every pass, and loops
  under ~10 ms play nearly silent.* → Ramp only when a segment starts mid-signal; make seam fades an opt-in
  `PlaybackShape` option reported in the snapshot; enforce a minimum loop length.
- **[Medium] ✔** `src/audio/command_mailbox.cpp:117-118`: `take()` loads `stop_sequence_` before
  `queue_.peek()`; the producer can store Stop (s) and enqueue Play (s+1) between the two loads. *"Stop then
  Play" ends Stopped; "Stop then Seek" lands at 0; `tests/audio_tests.cpp:440-444` can flake.* → Peek first,
  then load with acquire.
- **[Medium] ✔** `src/audio/playback_renderer.cpp:31-32`: `output_enabled` is loaded before `pcm_epoch`,
  while the decoder thread stores them in the opposite order. *A callback can play the new epoch (file start
  or a new file) at full gain after Stop.* → Load the epoch first, or pack both in one 64-bit atomic.
- **[Medium]** `playback_renderer.cpp:106-108`: mid-callback epoch adoption reuses the `output_enabled`
  captured at callback start and skips the gain cut and declick decay (click; new audio leaks under a pause
  fade). → Route through `follow_current_epoch`.
- **[Medium]** `preview_engine.cpp:86-97` / `preview_engine.hpp:83`: `select_output_device` destroys the old
  device before starting the new one, contrary to the doc. *One failed switch kills output.* → Stop the old
  device without destroying it; restart it on failure.
- **[Medium]** `preview_engine.cpp:150-165`: after a device switch, blocks rendered for the old rate play at
  the new rate until re-anchor. → Bump the epoch or store the rate in `PcmBlockHeader`.
- **[Medium]** `playback_controller.cpp:48-60`: `run()` has no exception barrier; `open_stream` catches only
  `bad_alloc`. *One bad file or OOM terminates the desktop app.* → try/catch per pass → `fail_source`.
- **[Low]** `source_stream.cpp:255-262`: `wrap_to_loop_start` ignores the decoder's real landing position;
  each wrap is a full seek.
- **[Low]** `source_stream.cpp:66-75`: loop end not clamped to the real end when frames are inexact.
- **[Low]** `source_stream.cpp:167-169` with `playback_controller.cpp:295-308`: a persistent converter error
  can become a busy loop of zero-frame blocks (possible).
- **[Low]** `preview_engine.cpp:28, 37`; `playback_controller.cpp:364-369`: ring sized for 192 kHz while
  384 kHz is accepted; prebuffer silently capped at 125 ms.
- **[Low]** `source_stream.cpp:202-213`: stretch-mode tail approximate; position can exceed `total_frames`.
- **[Low]** `shared_playback_state.hpp:36-91`: no `static_assert(is_always_lock_free)`; callback and decoder
  writes share cache lines. → `alignas(std::hardware_destructive_interference_size)`.
- **[Low]** No FTZ/DAZ anywhere: denormals on decaying stretch tails cost decoder-thread throughput.
- **[Low]** `playback_controller.cpp:143-151`: after device-loss recovery every source becomes `Paused`.
- **[Low]** `playback_renderer.cpp:157-161`, `preview_engine.cpp:227-230`: the playhead ignores device
  latency (leads by 10-40 ms); snapshot reads epoch before frame.
- **[Low]** `command_mailbox.cpp:73-114`: custom lock-free coalescing on top of moodycamel between two
  non-real-time threads, which is where the ordering bug lives. → A mutex-protected latest-values struct
  plus moodycamel, or direct unit tests.
- **[Low]** `fixed_text.hpp:72-75`: `AtomicText::load` spins without pause or yield (the seqlock ordering is
  correct).
- **[Low]** `output_device.hpp:13`, `stereo_converter.hpp:10`, `command_mailbox.hpp:10`, `pcm_ring.hpp:11`:
  public headers include PRIVATE deps. → `detail/`.
- **[Low]** `tests/audio_tests.cpp:197-219, 410-412`: wall-clock sleeps.
- **[Nit]** Discarded `try_enqueue` results (`pcm_ring.cpp:32, 52, 69`); `set_loop` re-anchors when
  unchanged (`playback_controller.cpp:225-232`); superseded decoder destroyed under `request_mutex_`
  (40-44); `Overloaded` defined twice; wrong "768 KiB" comment (`preview_engine.cpp:26-27`); no fade-in
  after underrun (`playback_renderer.cpp:45-47`); missing `///` on many private members of
  `CommandMailbox`, `PlaybackController`, `PlaybackRenderer`, `SourceStream`, `OutputDevice`, `PcmRing`.
- **Tests:** no direct tests for `CommandMailbox`, `PcmRing`, `SourceClock`/`LoopFold`, `AtomicText` UTF-8
  truncation, `StereoDownmix`, `LinearRamp`, `WakeupSignal`, `select_output_device`, device-loss recovery,
  loop or seek in stretch mode, underrun counting, non-48 kHz output.

### 4.9 app (`include/mso/app`, `src/app`): 5/10

Strengths: export streams into `<destination>.<uuid>.partial` and renames into place, with CSV quoting via
csv-parser and JSON via nlohmann; `search`/`count` hold a `shared_ptr` snapshot of libraries; the event
handler is invoked outside its mutex; the supervisor fences results and verifies waveform segments before
referencing them; workspace writes via temp plus rename, sets aside corrupt files, and has
`#ifdef _WIN32` LOCALAPPDATA and POSIX XDG branches.

- **[Critical] ✔** `src/app/application.cpp:294-296`, `include/mso/app/application.hpp:137`:
  `Application::library()` returns `.get()` of a temporary `shared_ptr` ("valid until close_library()").
  UI pool lambdas (`main_window.cpp:716, 862, 892, 947`; `inspector_panel.cpp:418, 521`;
  `history_panel.cpp:70`; `navigator_panel.cpp:103, 248`) call `apply_edit`, `undo`, `history`,
  `list_folders` and `create_backup` through it while `close_library` (279-286) can run on another of the
  four pool threads. *Use-after-free; the navigator reload during import makes it reachable.* → A
  `with_library(uuid, function)` that holds the `shared_ptr` for the call; route every UI catalog access
  through it; remove the raw-pointer accessors.
- **[High] ✔** `application.cpp:257-272`: `open_library` calls `catalog::Library::open` before the
  duplicate-UUID and 8-library checks; a writable open always runs `recover_after_crash()`
  (`library_lifecycle.cpp:225-226, 265-293`). *Opening an already-open folder (UI twice, or the CLI while
  the desktop runs, since there is no lock file) marks live jobs interrupted, pauses runs and scans, marks
  in-flight history partial. A 9th library is migrated and backed up before being refused.* → Read the
  manifest UUID first; add a per-library exclusive lock (`LockFileEx`/`flock`); recover only when the lock is
  acquired cleanly.
- **[High] ✔** `src/app/workspace.cpp:14-17, 24`: on Windows `std::getenv` returns ANSI-code-page bytes
  that are decoded as UTF-8 by `path_from_utf8`. *A profile like `C:\Users\אלון\…` gives a wrong or
  unwritable workspace path.* → `SHGetKnownFolderPath(FOLDERID_LocalAppData)` or `_wgetenv` inside the
  existing `#ifdef`; same for `MSO_WORKSPACE_DIR`.
- **[High] ✔** `application.cpp:139-152` with 508: `~Application` destroys the supervisor before stopping
  and joining imports; an import finishing in automatic mode reads `state_->supervisor` unsynchronized.
  *Data race and possible use-after-free at shutdown.* → Join operations first, then the supervisor.
- **[High]** `src/app/analysis_supervisor.cpp:147-209`: no wall-clock timeout per job; the lease is renewed
  every 5 s forever, also after a failed renewal (155-158). See theme 2. → Per-job deadline scaled by
  duration, then kill and restart the worker, failing the job as retryable.
- **[Medium]** `workspace.cpp:89-99` with `src/cli/main.cpp:61-73`: the desktop and CLI share
  `workspace.json`, each rewriting the whole file from its in-memory copy, with the same fixed `.tmp` name
  and no fsync. *Lost updates and possible corruption (which then sets aside every binding).* → Unique temp
  name, inter-process lock, re-read and merge, fsync.
- **[Medium]** `workspace.cpp:57-69`: `value("name", …)` throws on wrong-typed entries, escaping `load()`
  and blocking startup, contrary to `workspace.hpp:44`.
- **[Medium]** `application.cpp:377-406`: the operations map is pruned only by `wait_for_operation`, which
  the UI never calls; finished jthreads accumulate; the thread starts before it is inserted, so an early
  cancel is lost.
- **[Medium]** `application.cpp:187-195, 370-406`: one jthread plus one probe pool per import, and start-up
  scans every root at once. → One bounded queue on the vendored thread pool.
- **[Medium]** `application.cpp:471-489`: `queue_automatic_analysis` pages a full hydrating search to collect
  every sample id into an unbounded vector; errors dropped (482, 488). → Catalog-side `INSERT … SELECT` run
  creation.
- **[Medium]** `application.cpp:180-184`: the supervisor can lease a job before `set_paused(true)` is
  applied after an unclean shutdown; libraries opened later are never checked.
- **[Medium]** `application.cpp:174-179`, `analysis_supervisor.cpp:89, 139`: unthrottled analysis events
  (see theme 3).
- **[Medium]** `application.cpp:121, 201-204`: `set_event_handler` gives no guarantee the old handler is not
  still running. → Document plus a quiescence mechanism.
- **[Medium]** `analysis_supervisor.cpp:140-144`: a failed `submit` retries immediately, spawning workers
  back to back. → Backoff.
- **[Medium]** Tests: `AnalysisSupervisor` has none; `Workspace` has none for wrong types, newer versions,
  rebinds; export lacks `--raw`, absolute paths, filters, multi-page (>500 rows, so the cursor path is never
  exercised), replacing a destination; application tests sit in `TEST_SUITE("export")`.
- **[Low]** `metadata_export.cpp:173-185`: the CSV manifest is written after the rename, directly, with no
  error check; the rename error is overwritten (168-171).
- **[Low]** `metadata_export.cpp:102, 131-159`: the recorded revision does not describe the exported data
  under concurrent edits.
- **[Low]** `metadata_export.cpp:127, 152`, `workspace.cpp:94`: `dump()` throws on invalid UTF-8. →
  `error_handler_t::replace`.
- **[Low]** `metadata_export.cpp:127`: JSON envelope built by splicing dump text. `:165`: stream failure
  checked only after all pages.
- **[Low]** `application.cpp:541`: absolute-path resolver ignores managed roots (latent until P2.1).
  `application.cpp:95-96`: `resolve` joins catalog paths without `is_safe_relative_path` (possible).
- **[Low]** `application.cpp:233-249`: folder-name sanitizing skips Windows reserved names.
  `application.cpp:308-326`: `add_root` stores `absolute()` not canonical, allows duplicates, leaves an
  unbound root on failure, emits no availability event.
- **[Low]** `workspace.cpp:25, 30, 46`: temp-directory fallback for persistent state; an `exists()` error
  yields an empty workspace that the next save writes over the real one.
- **[Low]** `application.cpp:42, 186`: unused `State::owner` and a misleading comment;
  `analysis_supervisor.hpp:6` contradicts automatic mode.
- **[Low]** `analysis_supervisor.cpp:198-206`: failed waveform files left on disk; results `(void)`-dropped;
  `profile_digest = "default"` hard-coded (133); a malformed lease token falls back to `new_uuid()` (130).
- **[Nit]** `RunningOperation` in a named namespace in a .cpp (ODR hazard); the jthread's own token ignored
  for a second `stop_source`; "scope" reused for a different meaning; missing `///` on
  `AnalysisSupervisor`, `create_library`, `close_library`, `libraries`, `cancel_operation`, `count`,
  `start_analysis`, `export_metadata`, `ExportOptions`, `Workspace` binding methods.

### 4.10 cli (`include/mso/cli`, `src/cli`): 5/10

Strengths: CLI11 throughout with `ensure_utf8(argv)` and `SetConsoleOutputCP(CP_UTF8)` behind
`#ifdef _WIN32`; validators (`IsMember`, `Range(0,5)`, stduuid parsing); partial coverage reported as
warnings; `--json` built with nlohmann; Ctrl+C maps to cooperative import cancellation.

- **[High]** `src/cli/media_commands.cpp:18-35` with `analysis_supervisor.cpp:109-117`: `mso analyze` polls
  until queued+running is 0, with no timeout. *If mso-worker cannot start, the supervisor pauses itself and
  the CLI loops forever; a run older than the 100 most recent ends the wait early with false success.* →
  Detect "paused/worker unavailable" and exit non-zero; overall timeout; Ctrl+C; query by run id.
- **[Medium]** `src/cli/main.cpp:64-68`: only `CLI::ParseError` is caught; `json::type_error`,
  `filesystem_error` (`bench_commands.cpp:130-131`) or `bad_alloc` abort with no message. → Catch
  `std::exception`.
- **[Medium]** `edit_commands.cpp:12-18`: `std::stod` is locale-dependent and accepts trailing garbage,
  negatives, `nan`. → `std::from_chars` with full-consumption and range checks, shared with the query parser.
- **[Medium]** `src/cli/main.cpp:61-73` with `application.cpp:273`: the CLI uses the desktop's workspace and
  records every opened library as open. *Every folder touched by `mso info`, `mso bench` or tests is
  auto-reopened by the desktop and counts against the 8-library limit.* → Do not persist "open" from the CLI.
- **[Medium]** `edit_commands.cpp:134, 172`: `mso tag`/`mso collection` with no action exit 2 and print
  nothing; conflicting options (99-100, 85-86) silently pick one. → `->excludes()`, `require_option(1)`.
- **[Medium]** `library_commands.cpp:13-16, 96`: the SIGINT handler calls `stop_source::request_stop()`,
  which is not async-signal-safe on POSIX (can deadlock); never restored; only `import` installs it. →
  Atomic flag plus watcher thread, or `SetConsoleCtrlHandler`/`sigaction` behind `#ifdef`.
- **[Medium]** No automated test of any CLI command (no `cli` suite, no CTest invocation of `mso`). Rule 5
  **Fail**. → A doctest suite that drives `CLI::App` in-process against a scratch workspace.
- **[Low]** `main.cpp:67`, `command_context.hpp:18-20`: usage errors exit with CLI11's 100-114 codes despite
  `kExitUsage = 2`; every runtime error is 1; partial-coverage searches exit 0.
- **[Low]** `search_commands.cpp:101-105`: `--json --count` appends plain text after the JSON; count errors
  ignored.
- **[Low]** `bench_commands.cpp:54-64`: `--repetitions 0` indexes an empty vector (UB); benchmark errors
  ignored (114, 128).
- **[Low]** `library_commands.cpp:98`: `import --root 99` does nothing and exits 0; `\r` progress written
  when not a terminal and with `--json`; `info` swallows a count error (54).
- **[Low]** `media_commands.cpp:137-147`: same unbounded id collection as the app.
- **[Nit]** `register_analysis_commands` lives in `media_commands.cpp`; `play --volume` clamps instead of
  `CLI::Range`; magic 3600 s cap (104); missing `<functional>` in `bench_commands.cpp`; `"mso 0.1.0"`
  hard-coded (`main.cpp:49`) while the GUI uses `PROJECT_VERSION`; `g_interrupt` Hungarian prefix; missing
  `///` on file-local helpers and the six `register_*` functions.

### 4.11 ui (`include/mso/ui`, `src/ui`): 6/10

Strengths: one async gateway (`background_tasks.hpp`, `ApplicationSession::run`) delivering through
`QFuture::then(context, …)`, so destroyed panels never get callbacks; generation counters drop stale
results; all `this`-capturing connects pass a context object; `SampleTableModel` is a proper paged model
(native `canFetchMore`/`fetchMore`, keyset cursors, LRU of 10 pages, old rows kept until the new page
arrives) with tests that interleave deliveries; accessibility considered throughout; palette-only painting;
careful UTF-8/UTF-16 handling; refusing partial drags.

- **[Critical] ✔** UI use of `Application::library()`: see §4.9.
- **[High] ✔** `src/ui/sample_table_model.cpp:307-322, 516-539` (with 256, 78): `refresh()` during a pending
  `restart()` never resets `replacing_` (only `replace_rows`/`clear` do). *`canFetchMore()` stays false: the
  user cannot scroll past ~400 rows and sees "Loading…" until the next query. Very likely, since catalog
  refreshes fire every 750 ms during import and analysis.* → Reset `replacing_`/`append_pending_` in
  `refresh()`, or defer refresh to the pending restart; add a test.
- **[High] ✔** `src/ui/main_window.cpp:257, 280`; `src/ui/shortcut_catalog.cpp:11-23`: Space, Escape,
  Delete, F and 0–5 are `Qt::WindowShortcut`s. *Delete pressed in the Navigator, Jobs or History removes the
  table selection from the library; digits and F in the tree change the table selection's rating or
  favourite and kill type-ahead.* → `Qt::WidgetWithChildrenShortcut` on the table for single-key commands;
  only modifier shortcuts window-wide.
- **[High]** `application.cpp:176-178` → `main_window.cpp:416-426` → `sample_table_model.cpp:313-321, 538`:
  every analysis tick triggers a refresh (up to 10 sequential pages plus a count every 750 ms), a navigator
  rebuild, an inspector re-read and a drag re-check of up to 1,000 files. *A view scrolled past row 2,000 is
  cut back every 750 ms; the refresh may never commit if 10 pages take longer than the throttle.* → See
  theme 3; re-read loaded pages from stored cursors; recount less often.
- **[High]** `inspector_panel.cpp:565-567, 486-489, 449-453` (via `main_window.cpp:178-181, 444`): every
  refresh calls `show_selection`, resetting the name field, BPM spin box and tag list even when the selection
  is unchanged. *A rename or BPM edit in progress is overwritten during import or analysis.* → Update only
  unfocused, unmodified fields, or diff first.
- **[Medium]** `application_session.cpp:88-95`; `main_window.cpp:1099-1103, 1123-1126`: `set_preference`
  updates memory only when the queued writer runs, so the presets menu can show stale data and two quick
  saves can lose one; `preference()` on the UI thread waits on the mutex held during file write. → A
  session-side cache updated synchronously, coalesced writes.
- **[Medium]** `sample_table_view.cpp:223-226`; `main_window.cpp:443`: a drag starts only if async
  resolution for exactly this selection finished. *Click-and-drag in one motion is often refused.* → Build
  URLs at drag start from catalog data; check availability in the background.
- **[Medium]** `drag_path_resolver.cpp:9-29`; `application_session.cpp:17`: every selection change checks
  up to 1,000 files on the shared 4-thread pool, uncancellable. → A dedicated one-thread pool with a
  `stop_token`; debounce.
- **[Medium]** `navigator_panel.cpp:96-128, 137-195`: every reload reads every folder of every root and
  rebuilds a `QTreeWidget` (every 2 s during import); the header (`navigator_panel.hpp:25-26`) still says
  folders load on expand. Invariant 13. → A lazy `QAbstractItemModel`; fix the comment.
- **[Medium]** `main_window.cpp` (~1,200 lines, ~60 functions): business logic in the window (multi-library
  edit fan-out 849-885, undo routing 922-929, export 661-705, backup 711-727, presets 1093-1129, direct
  `catalog::Library` calls). → Extract `EditController`/`UndoRouter` behind the Application API,
  `ExportController`, `LayoutPresetStore`, `SampleContextMenu`.
- **[Medium]** `main_window.cpp:1131-1150`: the About box hard-codes dependencies and omits
  signalsmith-stretch, signalsmith-linear and csv-parser (MIT notices required). → Generate from
  `deps/manifest.json` at configure time.
- **[Medium]** `main_window.cpp:1165-1175`: `choose_library` maps the choice back by name; duplicate names
  ("My Samples") act on the wrong library. → Choose by index.
- **[Medium]** `inspector_panel.cpp:384, 394-402`: `same_library_set` compares only first rows; combos stay
  empty after a multi-library selection; never reloaded after taxonomy edits.
- **[Medium]** `inspector_panel.cpp:38-41, 198, 594-605`: tags rebuilt by splitting the display string on
  ", "; a tag containing ", " breaks counts and removes the wrong tag. → Carry tag ids in `SampleRow`.
- **[Medium]** `jobs_panel.cpp:192-194, 91-105`: the `present` set is filled but never read, so cleared
  runs come back and rows grow unbounded; `worker_status_` keyed by library name (`jobs_panel.hpp:66`).
- **[Medium]** `waveform_view.cpp:131-158, 66-71`; `preview_panel.cpp:29`: one logical pixel per column
  with no device-pixel-ratio handling (blocky on high DPI); full redraw every 30 ms. → Cached `QPixmap` at
  `devicePixelRatioF()`; repaint the playhead strip only.
- **[Medium]** `preview_panel.cpp:110-117`: every volume-slider step queues a full workspace file write. →
  Persist on release or debounce.
- **[Medium]** `tests/ui_tests.cpp`: no tests for MainWindow, InspectorPanel, JobsPanel, HistoryPanel,
  SearchBar, PreferencesDialog, LayoutStore, DisplayFormat, DragPathResolver, WaveformView, AddFolderDialog,
  EmptyStateWidget, FileManager, `select_keys`; refresh-while-pending untested.
- **[Low]** `sample_row_delegate.cpp:130-134` vs `main_window.cpp:138`: Qt emits `doubleClicked` before
  `edit()`, so double-clicking a star probably still plays the sample.
- **[Low]** `display_format.cpp:15-19`: analysis flag bits copied from the catalog's private `stage_flag()`.
- **[Low]** `background_tasks.hpp:28`: `takeResult()` rethrows on the UI thread → `std::terminate`. →
  Convert to `Result` in `run_in_background`, or `onFailed`.
- **[Low]** `display_format.cpp:32-34`, `sample_table_view.cpp:234`, `main_window.cpp:832`: three separate
  `generic_u16string()` conversions, lossy or throwing for non-UTF-8 Linux names.
- **[Low]** `file_manager.cpp:13`, `main_window.cpp:753`, `navigator_panel.cpp:415, 423`: `Q_OS_WIN`
  instead of `_WIN32`; "Show in Explorer" label on Linux.
- **[Low]** Qt already provides: clock formatting (`preview_panel.cpp:37-43`), `QLocale::formattedDataSize`
  (`display_format.cpp:80-83`), plural `tr("%n …", nullptr, n)` (`main_window.cpp:474`), no concatenated
  translations (`main_window.cpp:476-479`, `navigator_panel.cpp:149`), `QKeySequence::Undo/Redo/Find/…`
  (`shortcut_catalog.cpp:13-28`; Ctrl+Shift+Z is not Windows' Redo).
- **[Low]** `search_bar.cpp:107-108, 115`: hard-coded `#c42b1c` stylesheet; unused "invalid" property.
- **[Low]** `sample_table_model.cpp:78, 550-553, 456-458, 313, 487-491`: failed loads retried with no
  back-off; a failed refresh can leave pages `hydrating` forever.
- **[Low]** `main_window.cpp:880, 953-956, 1183`: edit and undo failures shown only as a 6 s status message.
- **[Low]** `add_folder_dialog.cpp:36-38`: says analysis runs "only when you ask", contradicting automatic
  mode.
- **[Low]** UI-thread blocking: engine created on first play (`preview_panel.cpp:244-263`);
  `is_library_folder` disk check (`main_window.cpp:517`).
- **[Low]** Layout restored after `show()` (window jumps); initial size set twice (`main_window.cpp:79`,
  `main.cpp:84`); `restoreState()` result ignored (`layout_store.cpp:41-42`).
- **[Low]** Model nits: `commit_staged_pages` swaps rows under persistent indexes without
  `layoutAboutToBeChanged` (523-536); unused `SampleKeyRole`; `queryFailed` used for info (572); first-page
  failure clears rows contrary to the header (417-419); `select_keys` is O(keys × rows).
- **[Low]** Accessibility: `StarRatingWidget` and `WaveformView` expose no role or value; the context-menu
  key uses the pointer position; `poll_engine` resets the accessible name every 30 ms.
- **[Low]** `navigator_panel.cpp:295, 354-361, 436-439`: root name parsed from display text; tag rename loses
  expansion; "Move tag" offers the tag's own descendants.
- **[Low]** `main_window.cpp:146-161`: modal `QMessageBox` from inside `startDrag`. → `QTimer::singleShot(0)`.
- **[Low]** `application_session.cpp:48-51`: one queued lambda per event, no coalescing.
- **[Low]** `sample_table_model.cpp:545`: `const_cast<SampleTableModel*>(this)`. → `mutable` cache.
- **[Nit]** Missing `///` on private members across ~18 headers (largest: `main_window.hpp:64-123`,
  `inspector_panel.hpp:53-59` `update_summary`, `navigator_panel.hpp:85` `show_context_menu`, 140 lines);
  `a`/`b` in `same_scope` (`sample_table_model.cpp:20-21`); custom JSON preference store instead of
  QSettings should be justified in a comment.

---

## 5. Infrastructure

### 5.1 Build & CMake: 7/10

Strengths: one static library per module with `mso::` aliases for vendored targets; SYSTEM includes and
`-w` for C deps; `compile_commands.json`; `-Werror` in `release`; SQLite with FTS5, `THREADSAFE=2`,
`DQS=0`, `OMIT_LOAD_EXTENSION`, `DEFAULT_FOREIGN_KEYS`; the include graph matches the documented layering
with no upward includes.

- **[Medium]** `CMakeLists.txt:78`: `set(CMAKE_AUTOMOC ON)` before `add_subdirectory(tests)` gives the
  Qt-free `mso_tests` an autogen step (`build/release/tests/mso_tests_autogen` exists). → Set `AUTOMOC` per
  Qt target.
- **[Medium]** `CMakeLists.txt:31`: every module exposes the whole `include/` PUBLIC, so layering is only a
  convention. → Per-module include roots, or a CI include-graph check.
- **[Medium]** `tests/CMakeLists.txt:8`: hard-coded suite list; a new `TEST_SUITE("files")` or `("cli")`
  compiles but never runs under ctest, and doctest exits 0 on an empty filter. → A catch-all entry or
  `doctest_discover_tests`.
- **[Medium]** `CMakeLists.txt:76`: `find_package(Qt6 6.5 QUIET …)` silently drops the GUI and UI tests. →
  `MSO_REQUIRE_GUI` (ON in CI).
- **[Low]** `cmake/ProjectOptions.cmake:2-12`: no ASan/UBSan option, no `_LIBCPP_HARDENING_MODE` in Debug,
  no `-Wold-style-cast`/`-Wconversion`/`-Wsign-conversion` (the code already has 0 C-style casts).
- **[Low]** No `install()` rules or windeployqt; double-click launch does not work (README admits it).
- **[Low]** `CMakeLists.txt:62`: `mso_app` links `mso_audio` PUBLIC but uses no audio header.
- **[Low]** `tests/worker_tests.cpp:115-119`: worker binary found by guessing. → `$<TARGET_FILE:mso-worker>`
  as a compile definition.
- **[Nit]** `deps/CMakeLists.txt:4`: claims compile features are asserted at startup; only FTS5 is.
  `SQLITE_*` definitions PUBLIC; consider `OMIT_DEPRECATED`, `OMIT_SHARED_CACHE`,
  `LIKE_DOESNT_MATCH_BLOBS`. `file(GLOB CONFIGURE_DEPENDS)` sources; `ui_tests.cpp` globbed into
  `mso_tests` as an empty TU; `release` preset is RelWithDebInfo.

### 5.2 CI (`.github/workflows/ci.yml`): 4/10

- **[High] ✔** `ci.yml:17-29`: the MSYS2 job runs `python3 … || python …` but installs no Python, and
  `path-type: minimal` hides the runner's. *The first real step fails.* → Add
  `mingw-w64-clang-x86_64-python`.
- **[High] ✔** `ci.yml:44-45` with `CMakeLists.txt:76`: Ubuntu 24.04's `qt6-base-dev` is 6.4.2, below 6.5,
  so the GUI and `ui` suite are silently skipped on Linux; distro Qt is also built against libstdc++ (ABI
  risk with libc++). → Qt ≥ 6.5 for this toolchain, or document Linux as CLI-only, and make a missing GUI
  fatal in CI.
- **[Medium]** `ci.yml:43`: `wget … llvm.sh | sudo bash` runs an unpinned script as root. → Add the apt repo
  and key explicitly.
- **[Medium]** Only `release` is built: no Debug, no sanitizer job, no `clang-format --dry-run`.
- **[Low]** Actions pinned by major tag, not SHA; no `permissions: contents: read`; no `concurrency`.
- **[Low]** `update: true` with unpinned pacman packages plus `-Werror`: a toolchain bump can break CI with
  no code change.
- **[Low]** No ccache; nothing forces the ffmpeg-dependent tests to run (`MSO_REQUIRE_FFMPEG=1`).
- **Observation:** the repository has no git remote and the code is uncommitted, so CI has never run.

### 5.3 Dependencies & patches: 7/10

All 14 vendored directories match the 14 manifest entries and the 14 `deps/README.md` rows; every rule-1
library is present; the single patch is listed, applied and present in the vendored file; archives are
sha256-pinned; `zipfile.extractall` sanitizes `..`.

- **[Medium]** `deps/update_deps.py:356-365`: `--check` only checks files exist; it cannot detect a hand
  edit (rule 2) or an unapplied patch. → Record post-patch sha256 per file and verify offline.
- **[Medium]** `update_deps.py:270-284` with `manifest.json:94-95`: pocketfft is cloned `--depth 1 --branch
  cpp` then compared with the pinned commit; once the branch moves, the pin is unreproducible. → `git fetch
  --depth 1 origin <commit>`.
- **[Medium]** `manifest.json`: the other 10 git deps are pinned only by tag (movable). → Add `commit` for
  every git entry (the script already verifies it).
- **[Medium]** About box missing MIT notices (see §4.11).
- **[Low]** `update_deps.py:306-307`: sha256 optional; CLI11's LICENSE download has none.
- **[Low]** `update_deps.py:303-312, 328-335`: downloads land in place before verification; a failed
  `git apply` leaves an unpatched file. → Stage, verify and patch in a temp dir, then move.
- **[Low]** `update_deps.py:290, 304`: `urlopen` without timeout; files dropped from the manifest are never
  deleted.
- **[Low]** `deps/miniaudio_impl.c` (project code in the vendored tree, not in `deps/README.md`),
  `deps/.clang-format`, `deps/picosha2/README.md`, `deps/stduuid/README.md` are not produced by the
  manifest. → Document `miniaudio_impl.c` as the sanctioned glue unit or move it; list or drop the READMEs.
- **[Nit]** `deps/miniaudio/miniaudio.c` and `signalsmith_linear/platform/linear-xsimd-dispatch.cpp` are
  vendored but never compiled. FFmpeg comes from pkg-config (documented); its license variant is not stated.

### 5.4 Layout: 9/10

No `.h`/`.hpp` under `src/` or `tools/`; only `.cpp` in `src/`; nothing in `include/` outside
`include/mso/<module>/`; all 87 headers have `#pragma once`; none has `using namespace`.

- **[Low]** `tests/test_support.hpp` is the only header outside `include/`. → Add a one-line `tests/`
  exception to AGENTS.md rule 4.
- **[Low]** Internal headers in the public tree (theme 9).
- **[Nit]** `SKILLS/README.md:31-32` says platform code is never in public headers, but
  `worker_process.hpp:33` and `file_handle.hpp:36` contain `#ifdef _WIN32` (with POSIX branches).

### 5.5 Tests & coverage: 6/10

Tests use exact or principled tolerances, generate fixtures at runtime, and include real fault injection for
the worker; no relaxed acceptance criteria were found (rule 16 holds).

| Module | Component | Tested? |
|---|---|---|
| domain | core | yes |
| domain | error | partial |
| catalog | Library lifecycle, history/undo, scans, queries, analysis jobs, backups | yes |
| catalog | sqlite wrapper, migrations, patch_operations, edit_command, types | partial (indirect) |
| search | parser, compiler, service | yes (gaps in §4.3) |
| media | WAV/RF64, AIFF, MPEG+ID3, container detect, filename hints, miniaudio decoder | yes |
| media | FLAC, Ogg, Vorbis | only when `ffmpeg` is on PATH |
| media | MP4, ASF, CAF, WavPack, FFmpeg decoder/probe | partial, silently skipped without ffmpeg |
| media | text decoding, tag mapping, bounded reader, byte cursor, file handle | partial (indirect) |
| files | importer | yes |
| files | file_identity | partial |
| audio | preview engine end to end | yes (black-box) |
| audio | ring, mailbox, controller, renderer, source stream, converters, stretcher, device | partial (indirect) |
| analysis | tempo, key, chroma, onset, loudness, waveform builder+codec, hashers, analyze_file | yes |
| analysis | spectrum, musical features, spectrogram stream | partial |
| worker | protocol, process, main, win32 platform | yes |
| worker | posix platform | written, never run |
| app | application, workspace, export | yes (in `export_tests.cpp`) |
| app | analysis_supervisor | partial |
| cli | all six sources (~935 lines) | **no** |
| ui | table model, star rating, delegate, table view, shortcut catalog, session | yes |
| ui | navigator, preview, waveform, display format | partial |
| ui | main window, inspector, jobs, history, search bar, preferences, add folder, layout store, drag resolver, file manager, empty state, background tasks | **no** |

- **[High]** CLI and 12 UI components untested (rule 5). → `tests/cli_tests.cpp` driving `CLI::App`
  in-process; offscreen widget tests for inspector, jobs, history, search bar, layout store, drag resolver.
- **[Medium]** `media_tests.cpp:735-737, 789-791, 859-861`: tests `return` with a MESSAGE when `ffmpeg` is
  absent and count as passed. → `doctest::skip()`, and fail under `MSO_REQUIRE_FFMPEG`.
- **[Medium]** Sleep-based timing: `worker_tests.cpp:497-498` (4.5 s then `heartbeats >= 2`), :506 (cancel
  under 2 s), `audio_tests.cpp:215-218, 391`. → Poll with deadlines; derive bounds from configured intervals.
- **[Low]** Four temp-directory and four WAV-writer helpers despite `test_support`.
- **[Low]** Suite naming: `worker_tests.cpp:245` uses `"analysis"`; app tests under `"export"`.
- **[Low]** No `TIMEOUT` or `LABELS` on CTest entries.
- **[Nit]** `tests/ui_tests.cpp` changed during the review; re-check counts at hand-off.

### 5.6 Documentation: 7/10

Every file path cited in README, BUILDING, deps/README, IMPLEMENTATION_STATUS and SKILLS exists; preset
names and output paths are right; README's CLI list matches the subcommands; SKILLS symbols exist; the
124 `mso_tests` cases and 9/9 CTest suites match registration.

- **[High] ✔** `README.md:9-12`: says CI "runs every test suite on Windows … and Linux … on every push".
  CI has never run, Windows would fail at Python, and Linux skips Qt. → "CI is configured for …; not yet
  run", and correct the Linux Qt claim.
- **[Medium]** `IMPLEMENTATION_STATUS.md:31`: "`mso_ui_tests` 7 cases"; `tests/ui_tests.cpp` now has 11.
  → Re-run and report real output (rule 16).
- **[Medium]** Undocumented deviations: unconditional declick fades (§4.8), hand-rolled container/tag parsers
  (§4.4), hand-rolled worker queues (§4.7). Each belongs in `IMPLEMENTATION_STATUS.md`.
- **[Low]** `README.md:42-43`, `IMPLEMENTATION_STATUS.md:12`: "job object (memory and CPU caps)" is
  Windows-only; POSIX has `RLIMIT_AS` and nice, no CPU cap.
- **[Low]** `SKILLS/add-dependency/SKILL.md:100-101`: Qt is found in the top-level CMakeLists, not
  `deps/CMakeLists.txt`.
- **[Low]** `docs/BUILDING.md:56-57`: apt list omits `ffmpeg`; :64 says CI runs "the same steps" but CI
  uses `release`, not `debug`; :14-15 omits Python.
- **[Low]** Stale code comments: `navigator_panel.hpp:25-26` (lazy folder load), `analysis_supervisor.hpp:6`
  (never starts work on its own), `add_folder_dialog.cpp:36-38`, `decoder_vorbis.cpp:24`,
  `deps/CMakeLists.txt:4`.
- **[Nit]** `SKILLS/build-and-test/SKILL.md:51` mentions `REQUIRE_OK`, which does not exist;
  `deps/README.md:405` says readerwriterqueue is "available" (it is used); `BUILDING.md:72` omits
  `mso_ui_tests`; `SKILLS/add-dependency/SKILL.md:79` says `mso::<name>` but nlohmann is `mso::json`;
  `VALIDATION.md:22` reads as stale.

### 5.7 Codebase-wide scans: 9/10

| Pattern (over `include/` and `src/`) | Count | Notes |
|---|---|---|
| TODO / FIXME / HACK / XXX | 0 | |
| `#ifdef _WIN32` files | 12 | Every logic block has an `#else`; single-branch blocks are Windows-only additions. |
| `<windows.h>` | 6 | All inside guards in platform .cpp files. |
| raw `new` outside UI | 7 | All private-constructor factories into `unique_ptr`. |
| raw `new` in UI | ~152 | Qt parent ownership (idiomatic). |
| `delete` | 1 | `QTreeWidgetItem` removal. |
| `rand` | 0 | `random_device`, seeded `mt19937` in bench. |
| `sprintf`/`strcpy`/`strcat` | 0 | |
| `catch (...)` | 0 | 17 typed catches. |
| `std::regex` | 0 | |
| `std::endl` / iostream | 0 | `<print>` used. |
| C-style value casts | 0 | 447 `static_cast`, 18 `reinterpret_cast`, 2 `const_cast`. |
| `(void)` discards | 32 | ~16 drop catalog job-state errors (theme 5). |
| headers without `#pragma once` | 0 | |
| `using namespace` in headers | 0 | |
| abbreviated identifiers | ~6 | `v`, `m`, `c`, `a`/`b` (listed in §4.2); no `stmt`/`cfg`/`ctx`/`buf`/`idx`/`tmp`/`err`/`db`. |

---

## 6. Suggested order of work

1. **Memory safety:** the `Application::library()` use-after-free (§4.9), the importer use-after-free
   (§4.5), the FFmpeg channel-change overflow (§4.4).
2. **Data integrity:** re-open recovery plus a per-library lock (§4.9), partial chunked-edit receipts and
   undo conflicts (§4.2), workspace lost updates and the encoding of its path (§4.9), fsync on atomic writes
   (§4.2, §4.9).
3. **Stuck work:** the analysis-queue starvation (§4.2), hang detection end to end (theme 2), the CLI
   `analyze` wait (§4.10).
4. **Everyday UI:** paging stall, single-key shortcuts, refresh storm and inspector clobbering (§4.11),
   relevance OR drops (§4.3), declick fades (§4.8), the three audio ordering races (§4.8).
5. **Linux and CI:** the worker include, case-sensitive path keys, POSIX identity and symlinks, the POSIX
   spawn path, then make CI green on both platforms with Python, Qt ≥ 6.5, `MSO_REQUIRE_GUI`,
   `MSO_REQUIRE_FFMPEG`, plus a Debug+sanitizer job.
6. **Rules 3, 5 and 6:** `///` on the core headers named in theme 10; CLI tests and the untested UI
   components; pinned migration checksums; correct the README CI claim and record the three undocumented
   deviations.
7. **Hygiene:** `detail/` headers, deduplicated constants and helpers, commit-pinned deps with content
   hashes, generated About box, `(void)` discards logged.
