# 03 — Persistence, search and metadata history

## 1. Authority, identifiers and revisions

`contracts/schema.sql` is an executable end-state reference schema for the features specified here, not permission to build all features in phase P1. Implement numbered migrations as each feature arrives. The reference schema uses SQLite STRICT tables, foreign keys, JSON checks and FTS5. Production startup must assert required compile features and a pinned tested SQLite build.

Each library UUID is globally unique; a sample has a library-local integer row ID for joins/FTS/ANN mapping plus a stable UUID for export and external references. A `SampleRef` always includes library UUID and sample UUID; bare local IDs never escape library scope. UUIDs are generated locally using secure OS randomness. Cross-library duplicate UUIDs are not collapsed.

Three independent versions exist: `catalog_revision` (monotonic committed transaction sequence), `edit_revision` (sample user-metadata concurrency), and `content_revision` (audio-generation identity). Derived artifacts include content revision, algorithm version and configuration/model fingerprint. A relocation does not itself change content revision if identity proves bytes unchanged; a potentially changed file invalidates content-dependent results until verified. Preserve previous observations/history for inspection without treating stale values as current.

## 2. Main entities

| Entity | Responsibility |
| --- | --- |
| `library_state`, `schema_migrations` | Identity, current schema, revision and migration checksums |
| `roots`, `folders` | Portable source intent, mode, hierarchical navigation and root availability |
| `samples`, `locations` | Logical record, user annotations, file occurrences and last observations |
| `metadata_observations`, `effective_metadata` | Provenanced extracted/inferred facts and indexed effective values |
| `tags`, `sample_tags`, `tag_aliases` | Hierarchy, membership and reversible merge redirects |
| `categories` | Separate user-editable primary category tree |
| `collections`, `collection_items` | Smart query AST or ordered manual membership |
| `analysis_profiles`, `analysis_runs`, `jobs`, `analysis_claims` | Immutable profiles/run snapshots, durable queue and cancellation |
| `artifacts`, `artifact_segments` | Content-addressed derived data references and segment storage |
| `vector_spaces`, `vector_entries`, `index_generations` | Model/preprocessing compatibility, embedding positions and ANN publication |
| `duplicate_edges` | Evidence for exact/equivalent/near matches without destructive merge |
| `history_actions`, `history_chunks`, `history_state`, `command_receipts` | Durable grouped undo/redo and expected-value patches |
| `file_operations`, `review_items`, `scan_sessions`, `scan_frontier` | Recoverable external work and reconciliation |
| `catalog_outbox`, `consumer_cursors` | Replayable committed changes, mirror/derived-index progress |
| `sample_search`, `sample_fts` | Rebuildable lexical projection; one document per sample |
| `library_settings` | Portable versioned settings, not workstation bindings |

Machine-local root bindings map `(library_uuid, root_uuid)` to verified absolute local paths and observed volume/file IDs. Their schema belongs to workspace settings, not the portable catalog. `roots.availability` is a last-observed catalog projection, overridden by live runtime root status for enablement and filtering. A current session's root-disconnect event invalidates queries even before a database write can complete.

## 3. Core invariants

1. `samples` is never automatically hard-deleted because a source disappears. Tombstones remove a sample from ordinary results but preserve metadata/history for restore.
2. Only live locations with a reachable root and validated content can be used for playback/drag. All locations may be unavailable without deleting a sample.
3. Every committed mutation increments `library_state.catalog_revision` once and writes its history where applicable and outbox envelope in the same transaction. Read-model rebuilds are internally versioned; they do not fabricate user-edit history.
4. Each connection executes `PRAGMA foreign_keys=ON`; application tests also run `foreign_key_check`. Foreign-key declarations alone are insufficient.
5. User override values, automatic observations and effective indexed values must remain consistent in a transaction. Selecting **Automatic** clears an override; selecting **Unknown** deliberately suppresses automatic inference. Use explicit override-state fields, not a single ambiguous NULL.
6. BPM is represented as milli-BPM integers. Pitch class is 0=C through 11=B; mode is major, minor or unknown. Enharmonic spelling is display-only. Duration is an integer number of source frames plus source rate when exact, and integer microseconds plus a certainty flag for filtering. Float codec bit depth may differ from container width and is labeled.
7. Ratings are integers 0–5; favorite is 0/1. Confidence is NULL or [0,1]. Do not invent a key for a noisy kick or a tempo for a one-shot.
8. Paths remain relative and bounded (32,767 UTF-16 code units at the Windows boundary), with no NUL or `..` escape. Store UTF-8 strings in SQLite; validate conversion failures explicitly. String limits: tag/category 128 code points, notes 64 KiB UTF-8, display name 1,024 code points.
9. Parent graphs for tags/categories/folders must be acyclic, depth ≤16 for taxonomies, and use normalized sibling keys. Folder parent and location folder must belong to the same root; application validation enforces these cross-row constraints. Manual collection reorder uses a transaction with a temporary disjoint ordinal range before assigning final ordinals so uniqueness is never violated mid-swap. Application transactions enforce graph invariants that a CHECK constraint cannot express. The reference SQL does not pretend to enforce recursive cycle rules.
10. Sidecars are never trusted solely because their filenames look right. Validate header, size, checksum, version, expected vector dimension and bounded offsets before publication/use.

## 4. Metadata precedence and provenance

For each field: explicit user value → explicit user unknown → accepted embedded/container observation → accepted inferred analysis → filename suggestion → unknown. This order is field-specific: duration/rate/channel count are technical facts and are not user editable; BPM/key/category can be overridden. A user-accepted suggestion becomes an explicit user value with source lineage. Classification suggestions do not set categories automatically. Effective projection materializes supported sortable/filterable fields; observations retain method, source content revision, confidence, model/profile digest and timestamp.

Rerunning analysis must never erase notes/tags/ratings, override user values, or silently change a chosen category. If source content changes, technical facts are reprobed; user fields persist but display a content-changed badge and may be reviewed. Stale automatic BPM/key/embedding/waveform are excluded from current computations; UI may expose them as historical values.

## 5. SQLite configuration and transaction policy

Baseline: page size 4096 before creating WAL, `journal_mode=WAL`, `synchronous=FULL` for authoritative catalog, `foreign_keys=ON`, `busy_timeout=2000`, `temp_store=FILE`, explicit negative KiB cache limits from the global budget. Disable SQLite mmap initially to make catalog memory accounting simple; consider bounded mmap only with measured benefit. Register the versioned Unicode normalization/collation implementation explicitly. Do not globally rely on SQLite `NOCASE` for Unicode.

Writers use `BEGIN IMMEDIATE`, bind parameters, keep statements reusable and commit at most 500 ordinary row changes per batch. Never perform filesystem/model work inside transactions. Foreground edits take priority over import/analysis commits. Bulk actions operate in visible chunks; their grouping is a user undo unit, not an unbounded SQL transaction. A busy writer retries with jitter up to 2 seconds and then returns a typed retryable error; UI stays responsive. SQLite errors are never silently downgraded to success.

Schedule passive WAL checkpoints off the audio path. At 256 MiB WAL, diagnose long readers and pause background write admission; above 512 MiB, cancel/restart expendable search snapshots and finish a controlled checkpoint. Do not delete WAL files to fix size. Limit ordinary search read transactions to 2 seconds and export snapshots to the export quota; large exports can pause writers per-library in an explicit export session to avoid unbounded WAL growth.

## 6. Lexical and structured search

Use a typed parser, not text concatenation. Supported initial syntax:

```text
kick
"metallic impact" tag:Drums/Kick
bpm:140..155 key:F#minor duration:<2s available:true
format:flac rating:>=4 favorite:true
(tag:Drums OR tag:Percussion) AND NOT tag:Acoustic
folder:"Packs/Goa" category:Bass analyzed:embedding
```

Whitespace is AND, `OR` has lower precedence than AND, parentheses group expressions, NOT is unary. Quoted strings support backslash-escaped quote/backslash. Maximum query 4,096 Unicode code points, AST depth 16 and 256 nodes. Ranges are inclusive; duration accepts ms/s/min; unqualified numeric BPM is exact milli-BPM after decimal parsing. Invalid fields/units/quotes produce a positioned parse error and do not execute a modified query. Normal free text is token search; substring/fuzzy matching is not silently promised by FTS5.

Compile plain text to escaped FTS5 terms and explicit prefix terms only when user writes `*` after a token of at least two normalized characters. Never forward raw user MATCH syntax. Store searchable name, relative path, embedded textual tags, user notes and expanded canonical tag/category labels in `sample_search`. External-content `sample_fts` is synchronized by insert/update/delete triggers; the reference schema includes them. A trigger on `samples` alone cannot know all related tags, so mutation handlers must update the projection in the same transaction for directly changed samples. Large taxonomy renames/merges enqueue resumable projection updates and expose `Search index updating`; structured tag ID filters stay correct immediately. FTS rebuild uses a staged maintenance transaction and verifies counts/content [S04].

Search index content excludes deleted samples by removing their search projection. Known unknown values use `IS NULL`; zero duration is not unknown. Tag subtree predicates use bounded descendant IDs/temporary tables, not gigantic interpolated IN lists. Folder filters use folder-tree membership and location EXISTS. Combine matches using EXISTS to avoid duplicate rows from multi-tag or multi-location joins.

> **Amendment (implementation, 2026-10-19).** The implemented query semantics refine this section;
> `IMPLEMENTATION_STATUS.md` ("Search semantics") is authoritative: equality at typed precision
> (`bpm:120` = 119.5 ≤ x < 120.5), two-valued `NOT` (X and NOT X partition the library), primary-location
> semantics for `folder:` and root/folder scopes, `analyzed:none`/`false`, a 1,024-node cap on
> smart-collection expansion, and relevance ranking that keeps non-text OR matches after the text hits.

## 7. Query plans, sorting and bounded pagination

Page size defaults to 200, maximum 500. Sort whitelist: name, duration, size, rate, BPM, key, rating, added date, format, availability and relevance (where applicable). Stable total ordering includes library UUID and sample UUID as final tie-breakers. Nulls sort last in both directions using a two-part cursor (non-null partition, then null partition), or a matching expression index; do not add an unindexed CASE sort and assume an existing simple index remains sufficient. Sort strings use pinned normalized keys; original text remains for display.

Use keyset pagination for indexed metadata orders. Cursor contains query hash, ordered sort values, per-library revision/generation and last sample key; reject a cursor after incompatible query/mutation revision with `STALE_CURSOR`, preserving visible selected IDs and offering a refreshed view. No unbounded OFFSET scan. An arbitrary jump far into results uses a cancellable rank/bookmark job; the scrollbar does not imply constant-time exact random indexing.

Indexed sorts use composite indexes ending in sample ID. Relevance and complex computed sorts use a bounded top-K query initially; for stable deep browsing, materialize a disk-backed result spool with `(ordinal, library_uuid, sample_id, score, sort_keys)` under a quota, then page it. It is a background operation with progress, not millions of resident DTOs. Do not promise sub-100 ms sort of every possible five-million-row query. Unsupported fast plans remain functional through cancellable disk-backed processing, labeled `Preparing results`.

Qt stores at most 2,000 fully hydrated rows (10 pages) with LRU eviction. The model can represent a larger logical result range using placeholders and a disk-backed ordinal map without retaining row DTOs. Counts are separately asynchronous and labeled exact/estimated/updating. Never call `COUNT(*)` on every keypress. Selection uses stable IDs; `Select all results` creates a persistent selection snapshot on disk, not a vector of every match in RAM.

## 8. Multi-library federation

Coordinator fans out the same AST to independently leased readers, up to four concurrent queries across eight libraries. Each library returns compatible sorted pages; use a k-way merge with a small page heap and per-library cursors. No writable `ATTACH` federation and no assumption of cross-database transactions. Library offline/failure/timeouts produce a visible partial-results status naming the omitted scope; never report complete results silently.

For lexical relevance, raw per-database BM25 values are not directly comparable. Use deterministic reciprocal rank fusion (RRF) across per-library result ranks with parameter k=60 and an explicit per-library candidate depth (1,000 initially). Disclose ranked-result depth; a deeper request expands it or uses the disk-backed spool. This is a ranked window, not exhaustive relevance sorting of every match. Metadata sorts remain exact. Semantic similarity scores can be compared only when vector-space digests match; otherwise show separately labeled result groups or require a single model space. Do not sum scores across unrelated embeddings.

Workspace selections/saved searches include a frozen set of library UUIDs or explicit `all currently open`. Opening another library does not silently change a frozen selection. A combined export records the revision of each participating library, not one invented global snapshot timestamp.

## 9. Undo/redo and bulk operations

Every user edit is a `history_action` with durable command UUID, label, state, and ordered `history_chunks`. A chunk contains schema-versioned per-entity/per-field before and after values, expected values/revisions, and associated membership changes. Apply forward changes, inverse payload and catalog outbox in one SQLite transaction. Keep history in the library across restarts; default no automatic pruning of user history. Display disk use and offer explicit history archival/reset later. Automated scans and analysis log system changes separately and do not consume the user undo cursor.

Undo applies inverse chunks in reverse order. Redo applies forward chunks in forward order. After crash, action/chunk state resumes from its committed boundary. A cancelled bulk action keeps already committed chunks and is one partially-applied undoable action. Undo cancellation similarly leaves a visible partial undo with Resume Undo; do not allow a different user metadata action in that library while a partial undo/redo remains unresolved. This simplifies correctness without locking the UI.

Use value-level compare-and-set for the fields affected by the command, not blindly overwriting an entire old row. A conflict with a later edit or external repair yields `HISTORY_CONFLICT` and a review item, with no silent clobber. Unrelated fields can change without blocking undo. A new user action after a completed undo marks the redo tail superseded but retains its historical log. IDs needed for undo stay tombstoned, not reused. For huge patches, store compressed per-chunk JSON with a bounded decoded size and checksum; the reference SQL uses JSON text for readability. Any binary migration must preserve its versioned decoder.

Tag rename changes taxonomy plus direct structured queries immediately and rebuilds affected text projections in chunks. Tag merge creates a reversible redirect from source to canonical target; reject cycles and ancestor-into-descendant conflicts. Query membership expands aliases immediately. Rewrite membership rows in chunks with unique `(sample_id, tag_id)` and remember pre-existing target memberships so undo does not remove them. Source is retained as a tombstone/alias. Suggestions, smart queries and history refer to stable IDs, not mutable names. Undo merge restores exact original memberships and redirect state. Do not claim full atomic UI visibility for millions of rewritten membership rows; canonical lookup makes result membership stable throughout.

Filesystem operations and metadata undo are different: undoing an import removes/tombstones its catalog additions, never deletes the original or silently removes managed copies. A restoration/relink action restores metadata associations only when the file identity still matches. UI explains when physical source bytes cannot be restored. There is no general promise to undo external filesystem edits.

## 10. Export

CSV: UTF-8 with optional BOM, CRLF, RFC-style double-quote escaping, one row per sample, stable documented column order. Tags are JSON arrays encoded inside a CSV cell, not a lossy separator. Null fields are empty and explicit `*_source`/`*_status` columns distinguish unknown/stale. Spreadsheet-safe mode is the default: prefix cells beginning with formula-triggering characters with an apostrophe and record this in the export manifest; a clearly labeled raw mode preserves text exactly.

JSON: schema-versioned top-level object with library revision manifest and streamed `samples` array; typed numbers/booleans/null, tags as arrays, nested locations and provenance. Write to an operation-owned temporary file, flush, atomically replace the chosen destination only after successful completion. Cancellation never leaves a completed-looking truncated file. Metadata export does not copy audio, vector bytes, absolute machine paths by default, or the entire undo log. Include stable UUIDs, relative paths and root names; absolute paths are a separate explicit export option. Reimport of export is a future operation, not an implicit round-trip guarantee for every internal table.
