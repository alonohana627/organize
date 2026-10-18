# 06 — Performance, validation, recovery and release operations

## 1. Measurement rules

All numbers below are engineering targets to verify, not measured results. A missed gate is a release failure or a documented scope/performance amendment; it must not be disguised by changing the benchmark. Record build commit, dependency/model digests, OS, CPU, RAM, storage, device driver, power mode, dataset hash and configuration in every report. Report median/p95/p99/max and sample count, not just an average.

Reference machine B1: Windows 11 x64, 8 physical CPU cores/16 logical threads, 32 GiB RAM, local NVMe SSD, no discrete GPU requirement, standard shared-mode audio endpoint at 48 kHz. Pin exact hardware/OS details in the first benchmark baseline. B2 stress configuration: same software with 16 GiB RAM and sample source on external USB SSD; B3 slow-source characterization: local rotating disk. Release latency targets apply to B1; B2 must remain functional and honor bounds, while B3 reports its own measured expectations. User-specific high-end hardware must not substitute for B1.

Datasets: D10K functional mixed audio; D100K early benchmark; D1M release scale; D5M full-scale gate. D1M/D5M metadata distribution: skewed filenames, 20% multilingual/Unicode cases, 5–30 tags/sample stress subsets, deep folders, 5% missing media, 20% missing analysis, mixed durations and formats. Use deterministic synthetic metadata for DB scale and a separately licensed/generated real audio corpus for decoder/audio/semantic quality. Do not claim five million physically decoded files based on five million synthetic rows.

Cold run: fresh process and documented cache state; a process restart alone does not clear the OS file cache. If cache eviction cannot be controlled, label it process-cold, not storage-cold. Warm: fixed preconditioning run, then 30+ repetitions; interactive search suite at least 1,000 queries, with 100 representative shapes and skewed rare/common terms. Measure latency from user command through first rendered results, separately from backend time. During-load tests run import or capped analysis plus playback.

## 2. Acceptance targets

| Metric | D1M target on B1 | D5M target on B1 | Conditions |
| --- | --- | --- | --- |
| Usable startup, warm | ≤2 s p95 | ≤3 s p95 | Existing healthy catalog; no full integrity scan/import/model load |
| Usable startup, process-cold | ≤5 s p95 | ≤8 s p95 | Initial page; background checks continue |
| Filename/tag + indexed filters, first 200 rows | ≤150 ms p95, ≤400 ms p99 | ≤300 ms p95, ≤800 ms p99 | Warm selective supported plans; no exact total count |
| Unfiltered indexed name/date page | ≤100 ms p95 | ≤200 ms p95 | Keyset traversal |
| Complex uncached sort/count | Show progress/cancel within 100 ms | Same | Completion measured, no invented constant-time promise |
| Federated indexed search | ≤500 ms p95 | ≤1 s p95 | 8 libraries, combined totals D1M/D5M, warm |
| Semantic ANN, model already loaded | ≤2 s p95 | ≤5 s p95 | Top 50, one shared model space, unfiltered, SSD, bounded shards |
| Semantic storage-cold | ≤5 s p95 | ≤12 s p95 | Measure real I/O; loading model reported separately |
| Text encoder inference, loaded | ≤500 ms p95 | Same | Approved CPU model on B1; part of end-to-end timing |
| Preview start | ≤150 ms p95 PCM; ≤300 ms compressed | Same | Local SSD, idle device; first audio, not only state change |
| Preview seek | ≤200 ms p95 PCM; ≤500 ms compressed | Same | Codec-specific exactness reported |
| UI event-to-feedback | ≤50 ms p95, no >100 ms avoidable UI block | Same | Import/analysis active within defaults |
| Scroll frame time | ≤16.7 ms p95, ≤33.3 ms p99 | Same | 60 Hz, cached thumbnails, bounded visible rows |
| Root disconnect → disabled actions | ≤2 s p95 | Same | Device signal/poll plus per-action validation |
| Import visible progress | ≤250 ms between updates | Same | Throttled; no dishonest total before enumeration |
| Import metadata throughput | ≥1,000 files/s simple PCM; ≥200 mixed files/s | Report same workloads | Local generated SSD fixtures; no copying/analysis; probe limits apply |
| Cancel acknowledgement | ≤2 s streaming, ≤5 s inference | Same | Worker kill fallback; partial results reported |

Memory gates are for the process tree and include child workers. No hidden unlimited memory in an index, decode process, temporary selection, database page cache or UI selection. Track private committed bytes and working set (including mapped pages) separately. Approximate steady-state envelopes:

| Mode | Private committed upper target | Process-tree working-set upper target |
| --- | --- | --- |
| Browse/search/preview, no model | 768 MiB | 1 GiB |
| Import plus preview, no model | 1 GiB | 1.5 GiB |
| Semantic search with approved CPU pack | 2 GiB | 3 GiB |
| Background analysis plus UI/preview | 3 GiB total, of which workers ≤2 GiB | 4 GiB total |

These are release gates measured over 30-minute scenarios, not a promise that Windows can always prevent transient residency overshoot. Admission reserves worst-case allocations before tasks start; worker job-object memory limits enforce the configured commit cap. At 90% budget, trim caches and reduce admission; at the hard limit, reject/pause work with `RESOURCE_LIMIT`. A model exceeding its declared peak fails approval. Do not kill the foreground app to meet a cap.

## 3. Budget allocation

Browsing-mode starting budgets: Qt/UI 192 MiB; catalog readers/writers aggregate 192 MiB; hydrated rows/query buffers 64 MiB; waveform tiles 96 MiB; audio decode/ring 64 MiB; watcher/import queues 32 MiB; miscellaneous reserve 128 MiB. Sum is 768 MiB. These include all connections, not 192 MiB per library. Each module reports live/reserved bytes to the coordinator. Per-library opening cannot multiply the global cap.

Search adds bounded vector reader mappings/scratch and the model session within its mode budget. Background analysis uses a distinct worker group limit but remains inside the total envelope. Disk temp space is separately quota-controlled. Selection/result spool quota starts at 2 GiB per operation/4 GiB total; if exceeded, abort preparation or request a deliberately narrower/configured scope through the UI, never spill indefinitely.

Disk planning must be measured, with initial estimates:

| Data | Estimate/budget |
| --- | --- |
| Catalog + indexes + history + FTS | Roughly 2–6 KiB/sample before unusually large notes/tags/history: about 10–30 GB at 5M; measured, not guaranteed |
| Local metadata mirror | Additional catalog projection footprint; show disk requirement before baseline creation |
| 512-D float16 embeddings, one/sample | 5.12 GB at 5M, plus segment metadata |
| int8 ANN vectors + graph | 2.56 GB vectors plus graph/IDs/alignment; measure adapter output |
| Waveform cache | User-configurable 2 GiB default LRU; full 5M coverage is not implied |
| Backups | At least one full verified pre-migration copy plus operation headroom |
| Logs | 100 MiB total rolling; last 10 sessions |

Before copy/index rebuild/migration, estimate final bytes + temporary bytes + old generation/backup retention. Refuse admission when estimated free space cannot cover this plus max(1 GiB, 10% of operation size) margin. Unknown estimates use conservative bounds and running quota checks. Disk full remains possible and must be recovered safely.

## 4. Correctness and quality gates

| Area | Required test |
| --- | --- |
| Domain | Metadata precedence, unknown vs auto, value bounds, revision transitions, Unicode normalization |
| SQL | Constraints, FTS insert/update/delete/rebuild, rollback, query plans on skewed data, foreign keys |
| History | Rename/merge/bulk undo and redo across restart; exact memberships preserved; compare-and-set conflict behavior |
| Import | Partial probe, malformed headers, duplicate requests, interrupted scan, denied subtree, changed-during-copy |
| Files | Case-only rename, long/Unicode paths, reused file ID, hard link, junction loop, unplug/replug, same UUID collision |
| Jobs | Pause in each stage, cancel epoch fence, lease expiry, repeated completion, crash/restart, input revision changes |
| Audio | Golden decoded frame counts, gain/rate/channel conversion, codec delay, sample-accurate PCM seek, nonfinite inputs |
| Real-time | No allocations/locks/I/O in callback; callback timing and underruns under capped workload |
| Search | Parser fuzzing, no SQL injection, hard filters always respected, cursor invalidation, federated partial scope |
| ANN | Recall and filtered recall against exact same-space reference; stale/tombstoned IDs never returned |
| UI | Keyboard-only main workflows, focus context, high DPI, screen reader labels, both views and layout recovery |
| Offline | Fresh install, every workflow and restart with networking denied; verify no application network attempts |
| Operations | Backup restore, failed migration, safe eject, disk-full atomic export, derived-index rebuild |

Audio stability gate: 30-minute playback with browsing plus default-limited analysis/import, zero application-induced underruns and zero callback deadline misses on B1 in the controlled harness. Separate external driver/OS disruptions with traces; do not automatically blame the environment without evidence. Callback workload target <25% of the device buffer duration p99 and bounded worst-case; benchmark actual device sizes.

Analysis evaluation uses held-out data not reused to choose thresholds. Before P5 release freeze a documented corpus and labels: at least 1,000 musical loops across tempo/genres; 500 tonal clips plus 300 non-tonal/ambiguous clips; 500 semantic queries with relevance judgments; and at least 1,000 duplicate transformations with negatives. Rights/provenance must be recorded. Generated click tracks and pure tones test mechanics but do not replace real-world music evaluation.

Provisional release gates to freeze before tuning: BPM within ±1% on ≥85% of suitable loops, with ≥70% coverage at the selected confidence threshold; separately report half/double-aware accuracy and octave mistakes. Key exact major/minor accuracy ≥75% on suitable tonal labeled clips at ≥60% coverage; non-tonal unknown detection ≥90%. Semantic nDCG@10 ≥0.60 and Recall@20 ≥0.75 on the frozen corpus, plus explicit improvement over lexical baseline on descriptive queries. Classification macro-F1 ≥0.70 over the frozen supported classes, with per-class coverage/confusion table. ANN Recall@10 ≥0.95 relative to exact same-space retrieval on 1,000 sampled queries; filtered recall measured across selectivity buckets (0.01%, 0.1%, 1%, 10%, 100%) with target ≥0.90 for ANN mode and 1.0 for exact mode. The approximate/candidate-limited status remains visible even when average recall passes.

Byte-exact confirmation: zero false positives in corpus and randomized mutation tests. PCM-equality: zero false positives under the defined canonicalizer, including channel/rate mismatch negatives. Near duplicates: precision ≥0.98 at displayed high-confidence threshold with recall and excluded transformation types reported. These are product gates, not assertions that an off-the-shelf model/algorithm already achieves them.

## 5. Fault injection and recovery matrix

| Failure point | Required durable outcome |
| --- | --- |
| Kill before catalog commit | No partial chunk, no success receipt |
| Kill after commit before UI event | Reopen shows committed state; duplicate command returns same receipt |
| Kill mid managed copy | Source untouched; `.partial` ignored/recovered by operation manifest |
| Kill after copy rename before DB registration | Published orphan matched to intent; recover registration/review |
| Kill during analysis output | Incomplete segment never referenced by ready artifact |
| Kill after artifact rename before DB commit | Orphan cleaned after grace/ownership checks |
| Drive removal during SQLite write | Typed I/O failure; stop writes; mirror fallback only at valid watermark |
| Root scan loses permission | No false bulk deletion/missing classification of unseen subtree |
| Watcher overflow | Schedule targeted full enumeration and retain metadata |
| ANN file truncated/checksum mismatch | Disable generation; use prior verified generation/fallback |
| Model pack corrupted | Reject before inference; core still works |
| Disk full during migration/export | Original catalog/export destination remains valid |
| History patch conflicts | No clobber; exact conflicting fields shown |
| Second writable process | Lock denied/read-only open; no concurrent app writer corruption |

Use deterministic failpoints before/after every publication and commit boundary, not only random process killing. Verify source hashes, committed revisions, history cursor, mirror watermark and artifact reachability after recovery. Run selected failpoints on real NTFS removable media in addition to mocked I/O.

## 6. Migrations and backup

Migration IDs are increasing integers with immutable SQL/code checksums; record application build and expected old/new versions. On open: validate library identity, inspect schema version, refuse newer writes, acquire per-library Windows named mutex plus lock file/handle, stop writers/jobs touching the catalog, finish/abort read snapshots, then prepare migration. A lock file's mere existence is not proof of a live owner; OS-held locks establish ownership.

For small transactional schema changes: first create a consistent backup via SQLite backup API into a separate file; check operation return codes, `quick_check`, `foreign_key_check`, required table counts and UUID/revision; record checksum and manifest, flush and publish backup. Run migration in a transaction and update both migration table and `user_version` consistently. `foreign_keys` changes cannot be assumed to take effect inside an active transaction; table rebuild procedures set/enforce them correctly outside/inside boundaries as appropriate. Run full integrity and semantic invariant checks before enabling writes.

For large/table-rewrite migrations: work on a backup-derived staging catalog while the original stays unchanged, run migration and checks there, flush and close, then switch through a journaled rename/manifest sequence. Keep a verified original and a migration recovery manifest with states `prepared`, `backup_verified`, `transformed`, `validated`, `switched`, `complete`. Windows cannot promise one atomic transaction over multiple file renames. Startup interprets the state, verifies checksums, and picks the last validated complete catalog; it never guesses based solely on timestamps. Old WAL/SHM handles must be closed/checkpointed through SQLite before replacement. Do not manually copy just a live `.sqlite` file and ignore its WAL [S01,S02].

On failure, preserve the failing candidate and logs, restore/use the verified original, and leave the library read-only until recovery succeeds. Do not automatically downgrade a newer schema. Default backups: retain last three successful scheduled/manual metadata backups and all pre-migration backups until a subsequent verified successful launch and explicit retention policy permits pruning; keep at least one verified recoverable copy. A metadata backup does not back up referenced audio. Backup UI lists what is included; a portable managed export may separately copy audio under explicit user action.

Safe export pauses catalog writes, creates a consistent catalog snapshot, flushes referenced immutable artifacts/manifests, records coverage and relative root bindings, and optionally copies managed media. Use operation-owned staging destination and a completion manifest. An incomplete directory is never labeled a finished portable library.

## 7. Build, CI and delivery

P0 must create `CMakePresets.json` with `windows-debug`, `windows-release`, `windows-asan` where supported, and test presets `unit`, `integration`, `smoke`; exact toolchain paths come from the developer environment, not user-specific hardcoding. Initial expected workflow from a configured Windows developer shell:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset unit --output-on-failure
ctest --preset integration --output-on-failure
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset smoke --output-on-failure
```

These presets/commands are deliverables of P0; they do not exist as an application build in this specification-only repository. P0 records exact working commands, compiler/SDK/Qt versions, dependency hashes and feature probes. Use a pinned package manifest/lock and explicit bootstrap that populates a local dependency cache; normal configure/build must not silently download dependencies. The installed application and model pack installation work without internet. Ship no Python runtime unless a later justified feature needs one; build/evaluation scripts may use Python.

CI tiers: every change builds core and tests relevant modules; Windows PR builds run unit/integration/smoke with fixtures; scheduled/release runners execute larger fault/corpus/performance suites. Linux core tests are useful for portable domain code but do not replace Windows gates. Sanitizers where supported, static analysis and fuzz harnesses focus on parser, file formats, IPC and migration payloads. Do not require flaky timing gates on arbitrary hosted shared runners; use a pinned performance machine with baseline comparisons and trend reports.

Packaging includes app/worker binaries, required runtime/Qt/codec DLLs, notices, offline help, capability manifest and build/dependency inventory. Model packs are separate and versioned. Build reproducibility means pinned inputs and documented outputs; bit-for-bit reproducibility is a later measured property, not assumed. Sign installers/releases when the project has an authorized certificate; no secrets enter the repo. No network update checker.

## 8. Operational diagnostics and supportability

Local diagnostics expose queue length, analysis CPU/memory, cache sizes, DB/WAL size, current catalog/mirror revisions, index coverage, worker crash count, source availability, audio endpoint/buffer, callback/underrun counters and build/model versions. Store bounded structured JSON logs with UTC timestamps, monotonic durations, correlation IDs and redacted paths. Crash dump generation is optional/local and explained; never upload automatically. Diagnostics export is a user action to a chosen local file.

Provide repair operations: validate catalog, rebuild lexical projection, rebuild vector index from existing vectors, clear waveform cache, reconcile roots, resume interrupted operation and restore verified backup. None may discard user metadata or remove original audio to make a check pass. User-visible wording must accurately distinguish “analysis missing,” “index missing,” “file offline,” “catalog offline,” “not supported,” and “failed.”
