# 09 — Decisions, risks and technical references

## 1. Architecture decision record index

| ADR | Decision | Rationale / rejected alternative | Revisit trigger |
| --- | --- | --- | --- |
| ADR-001 | C++23, Qt Widgets, SQLite | Confirmed stack; predictable native desktop/paged models | Explicit scope change only |
| ADR-002 | Metadata import separate from explicit audio analysis | Immediate usability, user control and bounded resources | Never remove consent boundary |
| ADR-003 | Logical sample + independent locations + content revisions | Paths move; duplicates are not automatically the same editorial record | Proven collision/invariant issue |
| ADR-004 | Per-library writer and local read-only mirror | Portable authority plus searchable catalog-drive disconnect fallback | Mirror lag/space evidence; preserve honest watermark |
| ADR-005 | Worker-process codec/analysis isolation | Codec/inference faults and hard limits cannot stall GUI/audio | Measured IPC cost; retain crash isolation |
| ADR-006 | Qt Widgets virtual models with disk-backed spools | Millions of samples cannot be materialized into Qt objects | Concrete rendering bottleneck |
| ADR-007 | Non-destructive preview, WASAPI shared baseline | Simultaneous DAW/system audio and no source edits | Later endpoint needs and measured host compatibility |
| ADR-008 | FTS5 + typed AST + keyset queries | Deterministic structured filtering without SQL generation | Proven query-plan limits; no arbitrary user SQL |
| ADR-009 | Local dual audio/text encoder, no generative LLM | Semantic retrieval is a shared-vector-space problem | Validated model improves corpus quality |
| ADR-010 | ONNX CPU pack with verified preprocessing | Portable offline inference, reproducible vectors | Provider/graph incompatibility or measured quality gap |
| ADR-011 | USearch immutable shards, bounded delta | Native Windows path and controllable residency; no full in-RAM HNSW assumption | Fails native build, memory, latency or recall gates |
| ADR-012 | Chunked history with resumable grouped undo | Million-row edits must not become unbounded RAM/long transactions | Proven unacceptable partial-action UX |
| ADR-013 | Byte equality, PCM equality and near similarity separated | Lossy formats cannot honestly be called bit/sample equal | New equivalence algorithm with explicit semantics |
| ADR-014 | Explicit phased releases | Useful browser before model/index/DSP complexity | Dependency graph changes, not impatience |
| ADR-015 | No remote Git publication in this task | User requested Git, not a hosting destination | User gives remote and publication instruction |

Alternatives not chosen by default: full-library in-memory HNSW; separate database server; Qt SQL calls directly from view callbacks; persistent offline PCM caches; automatic fuzzy relinking; generative query-to-SQL; a monolithic process with inference on the audio path; FAISS on-disk APIs assumed to work identically on Windows without testing. These are design judgments, not claims that the libraries are unsuitable in all projects.

## 2. Risk register and decision gates

| Risk | Impact | Mitigation and release decision |
| --- | --- | --- |
| Audio/text model poor on short musical samples | Natural language feels unrelated despite working embeddings | Freeze real producer query corpus, compare lexical baseline; P5 stays incomplete if quality fails |
| Model weights/export redistribution unresolved | Model cannot be shipped as assumed | Record exact code/weights provenance/licenses; separate pack approval; no invented permission |
| ONNX export differs from original preprocessing | Numerically plausible but wrong retrieval | Golden input/output parity and retrieval parity before indexing users' libraries |
| ANN disk paging at 5M | Latency/memory targets cannot both pass | Measure shard size, quantization and cold I/O; keep exact fallback; amend adapter via ADR if needed |
| Metadata/tag/history size exceeds estimate | Catalog/mirror/backup consumes far more disk | Distribution benchmarks, quotas and visible storage estimates; no silent history deletion |
| Portable drive removed during writes | Catalog I/O failure and stale cache | WAL-aware recovery, baseline/mirror revision display, safe eject |
| Watcher event loss or unstable file IDs | Wrong annotations or missing files | Reconciliation and evidence-based relinking; ambiguous review |
| Long readers retain WAL | Unbounded disk growth and writer contention | Query deadlines, spool quotas, export admission, checkpoint policy |
| Chunked bulk edits/merge undo complexity | Partial operation or target membership loss | Durable chunks, field-level preconditions, merge provenance and failpoint matrix |
| Codec/FFmpeg distribution configuration | Unexpected formats/licensing/security exposure | Pinned minimal build, capability corpus, notices/source process, disabled protocols |
| Stretch quality and latency | Pitch/tempo preview unusable or glitches | P4 dedicated adapter/quality gate; independent controls not fulfilled by resampling alone |
| Current environment lacks Windows/audio device | Can't prove native acceptance from a Linux container | Label unexecuted tests; Windows CI/hardware gates remain mandatory |
| Prior catalog unavailable before first mirror | Cannot browse uncached metadata | Display preparation status; never claim previously unknown content survives removal |

No unresolved product question prevents starting P0/P1. The remaining gates are empirical dependency/model/DSP validation, not requests for another product-design interview.

## 3. Primary technical references

These references establish specific API/library facts. Architectural choices, quotas, schemas and target metrics in this specification are project decisions and remain subject to implementation validation. URLs were checked during preparation on 2026-10-19. Exact released dependency revisions are pinned at P0/P5 rather than inferred from moving documentation pages.

| ID | Primary source | Supported use in this specification |
| --- | --- | --- |
| S01 | [SQLite WAL](https://www.sqlite.org/wal.html) | WAL/checkpoint/sidecar behavior and local shared-memory assumptions |
| S02 | [SQLite Online Backup API](https://sqlite.org/backup.html) | Consistent incremental backup instead of copying a live main DB alone |
| S03 | [Qt QAbstractItemModel](https://doc.qt.io/qt-6/qabstractitemmodel.html) and [Fetch More](https://doc.qt.io/qt-6/qtwidgets-itemviews-fetchmore-example.html) | UI-thread ownership and incremental data loading; not a claim that append-only fetchMore alone bounds memory |
| S04 | [SQLite FTS5](https://sqlite.org/fts5.html) | External-content index synchronization, tokenizer/query behavior and rebuilding |
| S05 | [Microsoft ReadDirectoryChangesW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw) | Asynchronous directory notifications and overflow reconciliation |
| S06 | [Microsoft job-object CPU rate control](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_cpu_rate_control_information) | Child process-group CPU caps, interpretation and limitations |
| S07 | [FFmpeg licensing/configuration guidance](https://ffmpeg.org/legal.html) | Configuration-dependent dependency obligations and DLL/source notices workflow |
| S08 | [ONNX Runtime thread management](https://onnxruntime.ai/docs/performance/tune-performance/threading.html) | Explicit inference thread-pool/spinning control |
| S09 | [FFmpeg protocols](https://ffmpeg.org/ffmpeg-protocols.html) | Protocol configuration; local-only runtime requires deliberate restriction |
| S10 | [CLAP original research](https://arxiv.org/abs/2206.04769) | Joint text/audio retrieval concept; does not certify any later checkpoint's license or quality |
| S11 | [USearch repository](https://github.com/unum-cloud/usearch) and [C++ interface](https://unum-cloud.github.io/USearch/cpp/) | Native C++ search and saved-index disk-view interfaces; must be benchmarked on Windows |
| S12 | [Qt LGPL documentation](https://doc.qt.io/qt-6/lgpl.html) | Module/build-dependent distribution review; no project-license choice implied |

Keep a dependency inventory with name, exact version/commit, SHA-256, origin URL, build flags, code license, linked modules and shipped notices/source instructions. For model weights add their separate license and preprocessing/export lineage. An architectural document is not legal clearance for a chosen binary, codec or checkpoint.

## 4. Definition of complete specification and complete implementation

This specification is complete enough to start implementation when: requirements are traceable to phases; default choices and empirical gates are distinguished; state/identity/transaction boundaries are explicit; schema and API contracts are available; failure behavior and benchmarks are defined; and the agent can start P0 without another product interview.

The application is complete through the core desktop scope only when P0–P6 pass on Windows with actual artifacts and evidence. P7 completes the later VST3 extension. Creating this repository, compiling the SQL, or writing a mock UI does not satisfy application acceptance. Do not conflate document validation with product validation.
