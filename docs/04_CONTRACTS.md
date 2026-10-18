# 04 — Service contracts, operation protocols and errors

## 1. API conventions

Public core APIs use value DTOs, RAII ownership, `std::expected<T, Error>`, `std::stop_token` and fixed-width integers. Synchronous interfaces below describe logical work on the appropriate executor; UI calls go through an asynchronous application adapter that returns a request handle and delivers completion/events on the UI thread. Do not invent a custom coroutine framework in P1. Interface sketches are contracts, not a complete compilable header.

No service exposes a raw `sqlite3*`, decoder pointer, Qt widget, unowned buffer, Windows handle or internal ANN ID. Request objects are immutable after submission. Every operation has a UUID, caller correlation ID, library scope and cancellation token. Read results include revision and coverage status. Errors use a stable enum plus context fields and a separately localizable user message.

```cpp
struct SampleRef { Uuid library; Uuid sample; };
struct RequestContext { Uuid operation; Uuid correlation; std::stop_token stop; };
struct Error {
    ErrorCode code;
    bool retryable;
    std::optional<Uuid> library;
    std::optional<Uuid> sample;
    std::string safe_detail;
};
template<class T> using Result = std::expected<T, Error>;

struct SearchPage {
    std::vector<SampleRow> rows;       // <= 500, bounded hydration
    std::optional<Cursor> next;
    std::vector<LibraryRevision> revisions;
    Coverage coverage;              // complete/partial/indexing/model-unavailable
    CountEstimate count;            // unknown/estimate/exact
};
class CatalogService {
public:
    Result<LibraryHandle> open(const OpenLibrary&, RequestContext);
    Result<MutationReceipt> edit(const MetadataCommand&, RequestContext);
    Result<MutationReceipt> undo(LibraryId, RequestContext);
    Result<MutationReceipt> redo(LibraryId, RequestContext);
};
class SearchService {
public:
    Result<SearchPage> search(const SearchRequest&, RequestContext);
};
class ImportService {
public:
    Result<OperationHandle> start(const ImportRequest&, RequestContext);
    Result<void> cancel(OperationId);
};
class AnalysisService {
public:
    Result<RunHandle> enqueue(const AnalysisRequest&, RequestContext);
    Result<void> set_state(RunId, RunControl); // pause/resume/cancel
};
class PreviewService {
public:
    Result<void> open(SampleRef, RequestContext);
    void post(TransportCommand) noexcept;    // bounded command mailbox
    PreviewSnapshot snapshot() const;       // no decoder/DB work
};
```

`MutationReceipt`: operation UUID, authoritative committed revision, history action UUID, applied/skipped/conflicted counts and mirror watermark. Success means the authoritative commit completed. It does not imply mirror persistence, a filesystem operation's completion or a cross-library commit. Reusing the same operation UUID and canonical command digest returns the stored receipt; reusing it with different content returns `IDEMPOTENCY_MISMATCH`. Persist command receipts in the action/operation payload or a dedicated feature migration; do not keep idempotency only in memory.

## 2. Command schemas

JSON examples are wire/fixture representations of typed DTOs. Reject unknown versions and invalid enum values; tolerate unknown optional fields only when the specified version says they are ignorable. Limit control messages to 1 MiB and per-chunk mutation patches to 4 MiB uncompressed. A request with more sample IDs uses a disk-backed selection handle.

```json
{
  "version": 1,
  "operation_id": "8c4c3110-99bb-42f9-963d-d11728e73a0f",
  "library_id": "2c3cfc37-1af5-4da3-b14a-6564bfd87312",
  "kind": "metadata.patch",
  "selection": {"snapshot_id": "a6a76399-588a-44c8-a6f1-789e9c9f82e9"},
  "changes": {"tags_add": ["Drums/Kick"], "rating": 4},
  "preconditions": {"conflict_policy": "skip_and_report"}
}
```

Human-entered tag paths are resolved to existing/new stable tag UUIDs in a preparation step; execution snapshots those UUIDs so concurrent renames cannot retarget edits. `tags_add`, `tags_remove`, `tags_replace`, `clear`, and `set` are distinct; incompatible operations on the same field are rejected. Exact-ID selections capture per-sample expected field values/revisions. Query-wide selections are enumerated at a pinned read revision into a local disk-backed selection DB with original content/edit revisions and a size quota; queue admission remains `preparing` until ready. Selection DBs are checksummed operation artifacts referenced by run/action manifests, survive restart, and are deleted only after no operation/history entry needs them. History itself contains the final per-chunk membership deltas; it must not depend indefinitely on a temporary selection DB.

```json
{
  "version": 1,
  "scope": {"libraries": ["2c3cfc37-1af5-4da3-b14a-6564bfd87312"]},
  "text": "warm plucked bass",
  "mode": "hybrid",
  "filter": {"and": [
    {"field": "duration_us", "op": "lt", "value": 3000000},
    {"field": "available", "op": "eq", "value": true}
  ]},
  "sort": [{"field": "relevance", "direction": "desc"}],
  "page_size": 200,
  "cursor": null
}
```

Hard filters apply to every returned result in every mode. `lexical`, `semantic`, `similarity` and `hybrid` are explicit modes. `similarity` requires a reference sample or an explicitly selected local file; decoding an external reference requires the action itself and never imports it without user choice. A semantic model missing does not produce an empty successful result; return a capability error with lexical search still available as an explicit alternative.

## 3. Event contract

Events carry `event_version`, library/operation IDs, sequence/revision and payload. UI event classes: `ImportProgress`, `CatalogChanged`, `RootAvailabilityChanged`, `ReviewItemCreated`, `RunProgress`, `JobFailed`, `PreviewStateChanged`, `IndexCoverageChanged`, `MirrorWatermarkChanged`, `OperationCompleted`. Catalog events are emitted only after commit; duplicate delivery is permitted, with subscriber deduplication by sequence. Preview events are transient and never routed through a database transaction.

Progress counts are monotonic except when a deliberate scope change starts a new run. Throttle UI progress to 10 Hz; coalesce repeated sample updates into ranges/ID batches. When event queue exceeds 1,024 messages or 4 MiB, replace fine-grained change events with `CatalogInvalidated(revision)` and refresh the visible page. Never drop a stop/cancel acknowledgement or terminal error silently.

Cancellation is a request, not proof that work ceased. UI shows `Cancelling` until admission has stopped and in-flight output is fenced. A result arriving after cancellation must match the accepted run epoch and lease; otherwise discard it. Already committed chunks remain committed and are reported. A source file copy is cancellable before publication; a final DB commit is short and completes/rolls back as one transaction.

## 4. Worker IPC

Use a private Windows named pipe for control and an explicitly inherited/shared mapping for streaming PCM or large feature chunks. Pipe name includes a cryptographic random token and user/session identity; ACL permits only the current user and required system principals. Validate the child process identity and handshake nonce. No TCP/HTTP listener is needed, including for later VST integration.

Frame header: 4-byte magic `MSOW`, protocol major/minor, little-endian uint32 payload length, uint64 request sequence; payload is UTF-8 JSON control data. Reject lengths >1 MiB before allocation. Binary blocks use a separately described mapping ID, byte length, content type and checksum. Never deserialize raw pointers or C++ object layout. Same protocol major required; negotiate supported minor capabilities at startup. Malformed input closes the channel and fails the job.

Handshake returns build ID, protocol, codec list, algorithm versions, model-space digests, process limits and accepted media constraints. Worker task includes job UUID, lease UUID, cancel epoch, content revision, normalized profile digest, read-only file handle capability and bounded output destination capability. Paths are not executable commands. Workers never invoke shell commands on filenames.

Heartbeat every 2 seconds, lease deadline 15 seconds with supervisor renewal. A hung job gets cooperative cancel, then worker termination after 5 seconds. Lack of heartbeat during a blocked IPC write cannot block the supervisor. At most three attempts for transient crashes/I/O; deterministic unsupported/corrupt/model-validation errors are terminal. Backoff 1/4/16 seconds with jitter; root-missing is `blocked` awaiting availability, not a retry loop.

Worker outputs include actual input stat/hash evidence, frames/bytes processed, algorithm/model IDs, warnings, completion/partial status and temporary artifact descriptors. Supervisor verifies lease, cancellation epoch, content revision and profile before publication. Stale input returns `INPUT_CHANGED`; output is not attached to the new revision. Worker success alone is not a committed catalog result.

## 5. Publication protocol for derived artifacts

1. Worker writes an operation-owned temporary segment with header, payload and checksum; flushes and reports completion.
2. Supervisor validates bounded structure/content and current input/lease/cancellation fences.
3. Rename the complete segment within the target filesystem to its immutable final name. Windows open-handle sharing flags must permit the chosen rename semantics; otherwise close first.
4. Writer transaction registers the segment/artifact, updates effective fields/vector entries, history if a user acceptance occurred, revision and outbox.
5. Publish UI/index events after commit. A crash between 3 and 4 leaves an unreferenced immutable artifact; orphan sweeper deletes it only after verifying no active operation/manifest references it and a 24-hour grace period.

For index generations, publication is a manifest switch only after complete checksum validation and DB generation registration. Keep the previous generation until all reader leases release. On startup reconcile manifest vs DB state and pick the last fully verified generation; never modify mapped index bytes in place.

## 6. Error taxonomy

| Code | Meaning | Recovery / UI |
| --- | --- | --- |
| `SOURCE_OFFLINE` | Root or volume unavailable | Preserve rows; block playback/jobs; resume after validation |
| `FILE_MISSING` | Root online, specific path absent | Mark location missing; reconciliation/relink review |
| `ACCESS_DENIED` | File/root permission prevents access | Keep metadata; show path and retry action |
| `INPUT_CHANGED` | Revision/stat changed during work | Discard stale output; new explicit/continued run may requeue |
| `UNSUPPORTED_CODEC` | Build lacks decoder or protected content | Metadata remains; report actual capability |
| `CORRUPT_AUDIO` | Decoder rejects malformed data | Fail only sample/job; preserve evidence |
| `PROBE_LIMIT` | Bounded import cannot determine fields | Partial metadata; explicit deep probe |
| `DEVICE_LOST` | Audio endpoint disappeared | Stop/flush; choose/reinitialize device |
| `MODEL_MISSING` | Required local pack absent | Core works; semantic capability unavailable |
| `MODEL_INVALID` | Hash/runtime/shape/preprocessing mismatch | Quarantine pack; no fallback download |
| `INDEX_UNAVAILABLE` | Derived vector index missing/stale/corrupt | Exact bounded fallback or rebuild status |
| `RESOURCE_LIMIT` | RAM/CPU/disk admission cannot fit | Queue blocked; show required versus available budget |
| `DISK_FULL` | Write/backup/export cannot finish | Roll back DB; preserve valid files; release owned temps |
| `DB_BUSY` | Writer/reader lock deadline exceeded | Retryable, never UI stall |
| `DB_CORRUPT` | Integrity or I/O evidence of corruption | Read-only/recovery screen; preserve original; verified restore |
| `SCHEMA_TOO_NEW` | Older app opens newer catalog | Refuse write, retain files, report required app format |
| `STALE_CURSOR` | Search snapshot invalidated | Refresh page while preserving ID selection |
| `HISTORY_CONFLICT` | Undo precondition differs | Review; no blind overwrite |
| `CANCELLED` | Cancellation fenced and acknowledged | Show retained partial work |
| `WORKER_CRASH` | Child terminated unexpectedly | Bounded retry; diagnostic record |
| `IDEMPOTENCY_MISMATCH` | Same operation ID, different request | Reject; caller supplies new operation ID |

Unknown errors become an internal-error result with a correlation ID and local log. Logs must avoid audio contents, notes, embedding text and full paths by default. Diagnostic export includes sensitive path details only through explicit user choice.
