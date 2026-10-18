# 01 — Product and technical requirements

## 1. Authority and interpretation

Version 1.0, dated 2026-10-19. This is an implementation specification, not an assertion that the application exists. MUST is a release requirement; SHOULD allows a documented, evidence-backed exception; MAY is optional. All times in persistent records are UTC Unix milliseconds unless a field says otherwise. Binary sizes use KiB/MiB/GiB; latency uses milliseconds; sample rates use Hz.

The present brief and recovered decisions about this specific organizer are binding. Defaults below resolve unspecified details. They are not retroactively attributed to the user. A phase postpones a capability; it does not remove it. Do not reopen settled choices just to start implementation.

## 2. Confirmed scope

| ID | Binding decision | Verification |
| --- | --- | --- |
| R01 | Fully offline Windows desktop application; no accounts or cloud APIs | First-run and feature suite with network blocked |
| R02 | C++23 core, Qt 6 UI, SQLite database per portable library | Build and dependency inspection |
| R03 | Millions of samples; strict bounded memory and responsive browsing | 1M and 5M benchmark gates |
| R04 | Import ordinary metadata automatically; expensive audio analysis requires explicit user action | Import trace contains no full audio-analysis jobs |
| R05 | Referenced folders and managed library copies | End-to-end tests of both modes |
| R06 | Track additions, deletions, moves, renames; unresolved changes go to review; never auto-delete originals | Filesystem and ambiguity fixtures |
| R07 | Open and search multiple libraries together | Federated query and partial-failure tests |
| R08 | Unavailable samples retain searchable metadata; disable playback; no offline audio copies | Disconnect tests for source and catalog volumes |
| R09 | Persistent analysis queue with pause, resume, cancellation, resource limits, customizable profiles | Restart and worker-kill tests |
| R10 | Common audio formats, including AAC and Opus | Versioned decoder capability corpus |
| R11 | Detailed sortable table and visual waveform browser | Both views share selection and filters |
| R12 | Bulk tags, rename, merge, hierarchical tags, suggestions | History and taxonomy tests |
| R13 | Customizable panels, columns, saved layout presets | Restart and monitor-change tests |
| R14 | Folder and category navigation, with filters in either | Query-equivalence tests |
| R15 | Manual and dynamically updated smart collections | Membership and query invalidation tests |
| R16 | Persistent metadata history and undo/redo across restarts | Kill/restart at every mutation boundary |
| R17 | Customizable keyboard shortcuts | Conflict detection and persistence tests |
| R18 | Import progress and cancellation | Partial-commit and resume fixtures |
| R19 | Export metadata to CSV and JSON | Round-trip schema and escaping tests |
| R20 | Database migrations with backup, verification, failure recovery | Upgrade and interrupted-migration matrix |
| R21 | Local natural-language search and audio similarity | Retrieval evaluation and no-network tests |
| R22 | Local classification, BPM detection, key estimation | Held-out audio evaluation |
| R23 | Playback V1: play/pause/seek/waveform | Audible vertical slice and transport tests |
| R24 | Playback V2: loop, pitch shift, speed, volume | DSP and transport tests |
| R25 | Playback V3: BPM sync, beat-aligned loops, tempo-aware preview | Beat and stretch tests |
| R26 | Drag samples into DAWs in V1; VST3 integration later | Windows DAW interoperability tests |
| R27 | Exact, cross-format equivalent, and near-duplicate detection; no automatic deletion | Duplicate ground-truth corpus |
| R28 | Incremental delivery with an early useful application | Each milestone produces a runnable package |

## 3. Resolved defaults

| Decision | Chosen default and rationale |
| --- | --- |
| Platform | Windows 11 x64, including N editions; no dependence on OS media codec packs. Windows 10 and ARM64 are not release targets. |
| Toolchain | MSVC ABI, MSVC or clang-cl with required C++23 feature probes; CMake and Ninja; one pinned compiler/SDK/Qt set in P0. No requirement to use every C++23 feature. |
| UI | Qt Widgets, `QMainWindow`, dock panels, custom paged `QAbstractItemModel`; `QPainter` waveform rendering first. No embedded browser. |
| Audio output | WASAPI shared mode first, through a small audio adapter; miniaudio is the initial output backend candidate. Device sample rate conversion is explicit. Exclusive mode/ASIO are outside V1. |
| Codec adapter | Pinned FFmpeg libraries in worker processes; local file handles and bounded reads. Decode PCM/float, resample with libswresample. No OS codec dependence. |
| Query intelligence | Structured filter parser plus a local joint audio/text embedding model. No generative LLM is required and no model may generate SQL. |
| Model runtime | ONNX Runtime CPU baseline, one lazy-loaded session per model role; optional GPU execution only after CPU correctness. |
| Model selection | A CLAP-family checkpoint is a candidate, not an approved redistributable dependency. Select and validate an exact model pack during P5 under the gate in document 05. |
| Similarity index | USearch C++ adapter, immutable, memory-mapped shards, bounded mutable delta; native Windows smoke test in P0 and scale gate before P5. Exact search is the small-corpus fallback. |
| Sample identity | One logical sample per imported file occurrence; explicit alternate locations can be attached after verified relink. Duplicate detection groups records without collapsing them. |
| Metadata ownership | User tags, categories, ratings, notes and overrides live in SQLite. No embedded audio metadata writes in scope. Extracted, inferred and user values remain distinguishable. |
| Simple organizing fields | Favorite, rating 0–5 (0 means unrated), free-text notes, editable display name. Original filename stays visible. |
| Audio modification | Non-destructive preview processing only. No editor, stem separation, recorder, sequencer, or sample renderer in core scope. |
| Libraries | Up to 8 simultaneously active/searchable libraries in the supported configuration; disk-backed registry may remember more. Warn rather than silently dropping scope beyond the supported limit. |
| Cross-library edits | Explicit independent operations per library with partial-success reporting. No distributed atomicity. |
| Source filesystems | Local NTFS and exFAT source volumes; local NTFS writable catalog storage is the supported baseline. Read-only sources work. No network paths, URLs, cloud placeholders that require hydration, or archive traversal. |
| Portable catalogs | Closed catalog directories are movable. Managed audio is relative to the catalog; referenced roots are rebound by identity/path mapping. Safe-export prepares a consistent portable directory. |
| Offline state | Source disconnected with catalog online: full metadata remains editable. Catalog disconnected: read-only local metadata mirror with a displayed revision watermark. |
| Privacy | No telemetry, crash upload, automatic update checks, online help assets, or runtime downloads. Logs are local and bounded. |
| Distribution | Offline installer/ZIP plus separate offline model pack. Core search and playback work without a model. Developer dependency fetching is allowed during a deliberate bootstrap step. |
| Source license | Not selected by this document. Preserve third-party licenses; do not invent ownership or choose the project's public license. |

## 4. Definitions

A **library** is a catalog directory with a stable UUID, SQLite database and versioned sidecars. A **root** identifies a source directory, mode, and local volume binding. A **sample** is the stable catalog record to which user metadata belongs. A **location** is a file occurrence reachable through a root and relative path. A **content revision** changes when audio bytes may have changed. A **metadata revision** changes when catalog state changes. A **job** is one durable unit of work against a fixed input revision. An **analysis run** is a user-requested group of jobs with an immutable profile snapshot. An **artifact** is derived data indexed by content and algorithm/model versions. A **mirror** is a local read-only catalog replica, not an audio cache and not a replacement for backup.

Separate these concepts. A path is not a stable identity. A fingerprint is not proof of equality. A missing file is not a deleted catalog record. An embedding model's cosine score is not a probability. A waveform is not audio that can be replayed.

## 5. Primary workflows

### 5.1 First run and first useful result

1. Start with all networking unavailable and no model installed.
2. Create a library in a writable local folder or open an existing compatible one.
3. Add a source root; default to **Reference in place**. **Copy into library** is a separate explicit choice with destination and space estimate.
4. Enumerate and probe bounded metadata; results appear in committed pages while import continues.
5. Search by filename, path, metadata, tags and filters; select a file and press Space to preview.
6. Drag an available file to a DAW, add a tag, create a collection, close and reopen the app.

This is the P1 release objective. It must not require embeddings, an ANN index, full-library waveform generation, or a GPU.

### 5.2 Import and analysis boundary

Import may read file identity, length, timestamps, container headers, codec parameters, existing tags, duration when cheaply derivable, and loop markers already in the container. Filename heuristics can propose BPM/key tokens, tagged as `filename` evidence. Import MUST NOT decode the entire file, compute whole-file hashes, generate full waveform caches, estimate BPM/key, classify, generate embeddings, or find audio duplicates. A managed copy may hash bytes it is already copying for integrity; that does not authorize further analysis.

A metadata probe is bounded to 4 MiB total reads and 2 seconds wall time by default; it can seek to footer metadata within that total. Probe timeout yields a searchable partial record with unknown fields, not a failed whole import. Exact duration may remain unknown for VBR files. An explicit retry/deep probe is available.

Clicking Play/Preview or explicitly requesting a waveform authorizes bounded decoding of that selected file. Opening the waveform browser alone displays cached thumbnails or placeholders; it does not launch a library-wide analysis. A selected short file may finish its waveform during playback. Full waveforms for other rows require **Generate waveforms for selection** or an analysis profile. Labels must distinguish `not generated`, `generating`, `stale`, and `failed`.

### 5.3 Organize and retrieve

User searches, filters by folder/category/tags/BPM/key/duration/type/rating/availability, switches views, and preserves selected sample IDs. Tags may be nested to depth 16, with cycle rejection and normalized sibling-name uniqueness. Category navigation uses a separate exclusive primary category per sample; tags are many-to-many. Suggested tags and classes remain suggestions until accepted. Category trees can start with Drums, Bass, Melody, Vocals, FX, Ambience and Unclassified; these are editable defaults, not a universal taxonomy.

Manual collections store ordered sample membership. Smart collections store a versioned query AST and evaluate current committed metadata. V1 smart collections use deterministic filters/lexical terms; later semantic collections pin model and query-vector versions and visibly report incomplete analysis coverage. Collections are library-scoped. A federated saved search is a workspace preset, not a cross-library transactional collection.

### 5.4 External changes and disconnected media

A watcher records hints, and reconciliation checks actual state. Unambiguous same-volume rename with stable file identity preserves the sample and history. If a path now points to different content, retain the record, increment content revision, invalidate analysis, and record the change; never transfer annotations to a different independently discovered file by filename alone. Cross-volume move proposals require user acceptance unless an explicit app-managed operation established provenance.

When a root disconnects, mark its availability at root level immediately; do not rewrite millions of locations. Search still works and displays an unavailable badge. Playback/drag re-check the actual location. Reconnection starts reconciliation and re-enables validated files. Do not create offline proxy audio. Managed audio already copied by an explicit import is the real managed file, not an offline proxy.

### 5.5 Analyze, inspect, correct

User selects samples or a query, chooses a profile, sees count, estimated decode work, disk and memory costs, and starts a durable run. A selection snapshot freezes IDs and content revisions; subsequently matching samples are not silently added. Progress reports discovered/eligible/queued/running/completed/skipped/failed, with ETA only after enough observations. Pause, resume and cancel are available for run and global queue. Re-analysis skips matching valid artifacts unless Force is chosen. Manual values always take precedence over inferred values and remain intact after re-analysis.

### 5.6 Preview and DAW integration

Only one sample plays at a time initially. Selection change does not auto-play by default; an optional persistent audition-on-selection setting is explicit. Space toggles play/pause outside text-edit controls; Escape stops. A seek bar works without a completed waveform. V1 DAW drag exports existing local file paths and leaves bytes unchanged. Dragging unavailable samples fails clearly. For a multi-selection containing unavailable items, the default refuses the mixed operation and offers a visible available-only choice in the UI; it never silently omits files.

V2 adds non-destructive looping, ±24 semitones pitch, 0.25–4× speed, independent tempo/pitch mode and a preview volume control. Volume is available in V1 as a basic safety/usability default even though the confirmed expanded playback milestone is V2. V3 adds user-set internal master BPM (20–400), beat snapping only with valid/user-supplied beat data, and tempo-aware preview. Host tempo synchronization belongs to the later VST3 integration; the standalone application has no implicit knowledge of Ableton's transport.

## 6. UI contract

Main workspace: library/root/category/collection navigation dock; central table or waveform browser; inspector dock; preview transport and waveform dock; jobs dock; status area with catalog state and analysis coverage. Use resizable panels, saved column order/width/visibility/sort, and named layouts. Restore layouts with off-screen windows clamped to current monitors. Provide Reset layout. All persistent layouts and bindings have schema versions and safe fallback defaults.

Table fields: filename/display name, format, duration and certainty, sample rate, channels, bit depth when meaningful, size, BPM/source/confidence, key/source/confidence, category, tags, rating, favorite, location, availability, analysis state, date added. Compressed bit depth is blank if not meaningful. Selecting multiple rows shows mixed values and explicit Set/Add/Remove/Clear operations. No ambiguous blank that overwrites values on focus loss.

Every long action has progress, cancellation where meaningful, and a recoverable result summary. Loading, unknown, empty, unavailable and error are distinct states. Sorting applies to the whole query, not merely loaded rows. Accessible names, keyboard focus visibility, 100–200% DPI and screen-reader exposure are required. Hebrew/Unicode filenames and bidirectional text must display correctly; UI strings start in English and are externalizable.

## 7. Explicit exclusions and extension boundaries

No cloud sync, accounts, telemetry, social catalog, sample marketplace, generative audio, training service, DAW project parsing, destructive audio editing, or automatic original deletion. VST3 is a later desktop integration, not part of the first usable release. No arbitrary plug-in host, embedded Python, or general automation scripting runtime. Do not build a generic framework for hypothetical future products. Extension points are the decoder, analysis algorithm, model pack, search-index adapter and audio backend interfaces already justified by this scope.
