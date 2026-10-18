# 07 — Phased delivery and acceptance backlog

## 1. Execution policy

Build one useful vertical slice at a time, validate it, commit it and package it. The current specification repository contains no application implementation. P0 is the first implementation phase. P1 is the first useful release. Advanced tasks have explicit dependencies; no calendar estimates are asserted without execution data. Track person-days and observed throughput after P1 if scheduling becomes necessary.

Each task completion requires code, relevant tests, a usable behavior demonstration, updated status and a coherent Git commit. A milestone passes only when all its gates pass on Windows. Feature-disabled stubs do not count as satisfying a requirement. If a late research/quality gate fails, core releases may ship with that capability clearly unavailable; do not label the full roadmap complete.

## 2. P0 — Foundations and decisive spikes

Dependencies: specification accepted as working input. Output: buildable shell, core test harness, pinned toolchain and evidence for risky dependencies.

| Task | Deliverable | Acceptance |
| --- | --- | --- |
| P0.1 | CMake presets, pinned compiler/SDK/Qt/SQLite, dependency cache, test framework | Debug/release build from documented Windows shell |
| P0.2 | Empty Qt window plus domain IDs/errors, application command adapter | UI thread ownership test; clean shutdown |
| P0.3 | SQLite wrapper and first migration, WAL/backup smoke harness | Transaction rollback, FK, FTS5 and backup restore pass |
| P0.4 | Output/decoder spike: local WAV → worker → PCM ring → WASAPI | Audible playback with UI responsive, no callback allocation |
| P0.5 | Native Windows USearch disk-view build/load/search and memory measurement | No POSIX-only assumptions; bounded shard behavior recorded |
| P0.6 | Offline network test and Windows file identity/watcher spike | No runtime network attempts; overflow handled by rescan |
| P0.7 | Status/ADR/benchmark scaffolding and Git workflow | Build commands, actual results and unresolved failures recorded |

Do not block the P1 metadata/playback path on selecting a final semantic model. P0.5 informs later adapter risk; if it fails, keep the vector interface deferred and record a replacement decision before P5.

## 3. P1 — First useful sample browser (V1)

Dependencies: P0 core/codec/database spikes. Output: offline runnable app that catalogs a real folder, finds samples, previews them, and drags them to a DAW.

| Task | Deliverable | Acceptance |
| --- | --- | --- |
| P1.1 | Create/open one library; referenced root import, bounded probe, progress/cancel | First rows appear during import; no expensive jobs triggered |
| P1.2 | Paged table, lexical search, name/date/duration filters and sorts | D100K responsive; unknown values display correctly |
| P1.3 | Preview play/pause/stop/seek, volume, lazy selected waveform | WAV/FLAC/MP3 plus AAC/Opus smoke corpus; long-file streaming |
| P1.4 | Existing-file drag-and-drop to Windows DAWs | Ableton Live and one other chosen DAW tested; unsupported target formats surfaced |
| P1.5 | Favorites, ratings, notes, simple tags, basic manual collection | Data persists; per-library undo/redo survives restart |
| P1.6 | File/root availability and conservative reconcile | Unplug audio source keeps rows; playback disabled |
| P1.7 | Crash recovery, first verified backup/migration tests, packaged offline build | Kill before/after edits, source unchanged, no success before commit |

Gate: user can complete the first-run workflow from document 01 without a model or GPU. Record D100K memory/search/preview metrics; ship this slice before full-scale AI. Simple tag history and schema recovery are included now because retrofitting metadata durability later is expensive.

## 4. P2 — Library reliability, portability and organization

Dependencies: P1. Output: durable portable multi-library organizer.

| Task | Deliverable | Acceptance |
| --- | --- | --- |
| P2.1 | Managed copy import with operation journal and integrity verification | Crash at every publication boundary; no original deletion |
| P2.2 | Multiple libraries and federated queries/partial status | Eight libraries; no ID collision; correct sort/merge |
| P2.3 | Local metadata mirror, safe eject and offline-catalog read mode | Whole catalog drive removal works at displayed watermark |
| P2.4 | Rename/move tracking, unresolved-change review, root rebinding | Case/Unicode/cross-volume/reused-ID/denied-subtree corpus |
| P2.5 | Hierarchical tags/categories, bulk edit/rename/merge and grouped history | Million-row selection stays disk-backed; merge undo restores memberships |
| P2.6 | Smart collections, folder/category filter parity | Deterministic AST reevaluates on relevant commits |
| P2.7 | Metadata CSV/JSON export, configurable shortcuts, docks/columns/layout presets | Cancel-safe export; keyboard/layout recovery tests |
| P2.8 | Robust migration staging/restore and version compatibility | Interrupted upgrade matrix with verified original retained |

Gate: D1M metadata memory/search targets, real removable-drive scenarios, persistent undo/redo and managed import integrity all pass. Smart collections at this phase use lexical/structured queries; semantic collections are not faked.

## 5. P3 — Explicit analysis and waveform browsing

Dependencies: P2 durable operations. Output: controllable local analysis with useful musical metadata.

| Task | Deliverable | Acceptance |
| --- | --- | --- |
| P3.1 | Durable runs/profiles, scheduler, process limits, pause/resume/cancel | Leases/epochs/restarts and budget enforcement verified |
| P3.2 | Waveform segment cache and virtualized waveform browser | Open view does not trigger full-library analysis; LRU bounded |
| P3.3 | Peak/RMS, onset and spectral features, musical BPM/key algorithms | Deterministic fixtures and held-out corpus reports |
| P3.4 | Provenance inspector, manual overrides, stale-result invalidation | Reanalysis cannot overwrite user decisions |
| P3.5 | Full required decoder/container corpus and error matrix | Unsupported/corrupt samples isolated; N-edition behavior tested |
| P3.6 | Byte-exact and PCM-equal duplicate analysis/review | Zero false equality; no delete/merge side effects |

Gate: analysis control/resource targets, import-analysis separation and corpus quality gates pass; D1M indexing is measured separately from audio-analysis throughput. If BPM/key accuracy fails, mark the algorithms experimental/unavailable for release until corrected; do not hide confidence or invent valid outputs.

## 6. P4 — Advanced preview (V2)

Dependencies: P3 input/provenance/beat-feature infrastructure, P1 stable playback. Output: loops, independent pitch/speed and stable preview under load.

Tasks: P4.1 choose/pin stretch implementation and license manifest; P4.2 sample-frame loops and optional crossfades; P4.3 independent ±24-semitone pitch and 0.25–4× speed with quality/latency reporting; P4.4 transport generation/seek/end-of-file stress; P4.5 audio regression corpus and 30-minute load test. Gate: controls perform correctly and callback is still free of blocking work. Original files remain unchanged. Extreme settings may sound degraded and are labeled, but must not crash or overrun buffers.

## 7. P5 — Local semantic search and similarity

Dependencies: P3 pipeline, P0/P2 scale architecture. Output: validated offline model pack, natural-language and reference-audio retrieval, classification suggestions and near duplicates.

| Task | Deliverable | Acceptance |
| --- | --- | --- |
| P5.1 | Exact model choice, provenance/license records, ONNX export and fixtures | CPU Windows runtime, parity, memory and quality gates |
| P5.2 | Pack install/verify/unload, embeddings and vector-space versioning | No downloads or code execution from pack; stale vectors excluded |
| P5.3 | Sharded ANN + delta + exact fallback + atomic rebuild | 1M/5M memory and recall reports; corruption/recovery tests |
| P5.4 | Natural-language, similarity and hybrid modes with hard filters | Relevant corpus metrics; incomplete coverage visibly reported |
| P5.5 | Classification/tag suggestions with acceptance history | No automatic tag/category replacement |
| P5.6 | Near-duplicate candidates and pair verification | Precision gate, non-transitivity handling and A/B review |
| P5.7 | Optional semantic smart collections | Pin model/query version, explicit coverage and refresh behavior |

Gate: all supported CPU features work with networking blocked; inference/ANN performance and quality both pass. GPU is an optional later improvement, not a prerequisite for the product claim. A failure to find an approved model is an explicit P5 blocker, not permission to switch to cloud APIs.

## 8. P6 — Tempo-aware preview and scale hardening (V3)

Dependencies: P4 stretch, P3 beats, P5 index scale for full product. Output: internal BPM sync, beat-aligned preview, production-scale operations.

Tasks: P6.1 internal master tempo and ratio calculations; P6.2 beat-aligned loops and manual markers; P6.3 latency/phase tests with beat grids and ambiguous tempo; P6.4 D5M end-to-end soak/search/history/export/import tests; P6.5 accessibility and high-DPI pass; P6.6 offline packaging, notices, backup/restore drills and capability documentation. Gate: five-million-row budgets, data integrity and documented quality pass. No full feature-complete declaration before P0–P6 gates are met.

## 9. P7 — VST3 integration extension

Dependencies: P6 stable desktop release. Later confirmed requirement, not core-release blocker. Produce a separate VST3 companion that receives host tempo/transport and exchanges selected sample/preview controls through versioned local IPC. Do not put catalog, SQL, inference or filesystem work on a DAW audio thread. Decide plugin audio role and sample-transfer behavior in a focused ADR grounded in host testing. Test Ableton and at least one other VST3 host, process disconnect/reconnect and version mismatch. No internet dependency. VST3 SDK/license choice and exact host API behavior are verified when this phase starts. P7 is not fulfilled by an empty plugin shell.

## 10. Requirement traceability

| Requirements | Implementation phases | Mandatory evidence |
| --- | --- | --- |
| R01, R02, R28 | P0–P7 | Offline build/run, dependency manifest, incremental packages |
| R03 | P1 D100K, P2 D1M, P5/P6 D5M | Reproducible latency/memory reports |
| R04, R18 | P1, P3 | Import trace, cancellation/restart test |
| R05 | P1 reference, P2 managed | Both import modes and copy fault matrix |
| R06, R08 | P1 basic, P2 complete | Reconcile/unplug/mirror/ambiguity tests |
| R07 | P2 | Federated results/partial-scope tests |
| R09 | P3 | Queue state/resource/fault tests |
| R10 | P1 baseline, P3 complete | Format/container capability corpus |
| R11 | P1 table, P3 waveform | Shared filters/selection, frame-time metrics |
| R12 | P1 simple, P2 full management, P5 suggestions | Merge/history/provenance tests |
| R13, R14, R15, R17, R19 | P2; P5 semantic collections | UI/query/export tests |
| R16 | P1 simple, P2 full bulk | Durable undo/redo/conflict matrix |
| R20 | P0/P1 baseline, P2 full | Verified backups and interrupted upgrade tests |
| R21 | P5 | Semantic/ANN evaluation and offline run |
| R22 | P3 BPM/key, P5 classification | Frozen held-out quality metrics |
| R23 | P1 | Play/pause/seek/waveform tests |
| R24 | P4 | Loop/stretch/pitch/speed corpus |
| R25 | P6 | Internal tempo and beat-alignment tests |
| R26 | P1 drag/drop, P7 VST3 | Real DAW interoperability |
| R27 | P3 exact/equal, P5 near | Three-level duplicate corpus |

## 11. Git and ongoing status

Create `IMPLEMENTATION_STATUS.md` at P0 with current milestone, completed task IDs, exact passing/failing commands, measured performance, unresolved risks and next executable task. Maintain feature branches such as `phase/p1-browser`; commit coherent increments, typically one task or one verified sub-slice. Suggested commit subjects: `build: pin Windows toolchain and presets`, `feat(import): stream metadata into bounded catalog batches`, `test(recovery): exercise managed-copy publication failures`.

Before each commit: inspect diff/status, run relevant checks, stage specific files, write a factual commit message. Do not commit generated builds, personal samples, databases, model weights or secrets. Commit schemas/migrations/test generators/fixture manifests; small licensed fixtures may be committed with provenance. Tags `v0.1.0`, `v0.2.0` etc. follow actual packaged passing milestones, not this document's numbering. No remote publishing is authorized by merely asking to work with Git.
