#pragma once
/// @file
/// A portable library: catalog directory with library.json, catalog.sqlite and versioned sidecars
/// (docs/02_ARCHITECTURE.md §4). The Library owns one serialized writer connection and a bounded
/// pool of reader connections. Every committed catalog mutation increments catalog_revision once and
/// appends an outbox envelope in the same transaction (docs/03_DATA_AND_SEARCH.md §3); the envelope
/// lists the samples whose row data changed and whether navigation structure changed, and
/// changes_since() serves it to consumers. Analysis-queue bookkeeping (leases, renewals, failures)
/// is not catalog data and does not advance the revision.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mso/catalog/edit_command.hpp"
#include "mso/catalog/sqlite.hpp"
#include "mso/catalog/types.hpp"

namespace mso::catalog {

namespace detail {
class ReaderPool;
}

/// RAII loan of a reader connection from a library's pool; the connection returns on destruction.
/// The lease shares ownership of the pool, so it stays valid even if the Library is destroyed first
/// (its connection is then simply closed when the lease ends).
class ReaderLease {
public:
    ReaderLease(ReaderLease&& other) noexcept = default;
    ReaderLease& operator=(ReaderLease&&) = delete;
    ReaderLease(const ReaderLease&) = delete;
    ReaderLease& operator=(const ReaderLease&) = delete;
    ~ReaderLease();
    /// The leased read-only connection.
    sqlite::Connection& connection() { return *connection_; }

private:
    friend class Library;
    ReaderLease(std::shared_ptr<detail::ReaderPool> pool, std::unique_ptr<sqlite::Connection> connection);
    std::shared_ptr<detail::ReaderPool> pool_;
    std::unique_ptr<sqlite::Connection> connection_;
};

/// Options for opening a library.
struct OpenOptions {
    bool read_only = false;  ///< also forced when the catalog schema is newer than this build
    int max_readers = 4;     ///< bounded reader pool
};

/// Outcome of the startup recovery pass.
struct RecoveryReport {
    std::int64_t interrupted_scans = 0;                         ///< running scans paused for resumption
    std::int64_t partial_history_actions = 0;                   ///< actions interrupted mid-way, now `partial`
    std::int64_t interrupted_jobs = 0;                          ///< running analysis jobs returned as `interrupted`
    bool migrated = false;                                      ///< pending migrations were applied
    std::optional<std::filesystem::path> pre_migration_backup;  ///< verified backup taken before migrating
};

struct LibraryTestAccess;

/// One open library: identity, catalog mutations with durable history, scans, analysis queue,
/// change feed and maintenance. Thread-safe: writes serialize on one writer connection, reads use
/// the bounded reader pool.
class Library {
public:
    static constexpr std::string_view kCatalogFileName = "catalog.sqlite";
    static constexpr std::string_view kManifestFileName = "library.json";
    static constexpr int kLibraryFormatVersion = 1;
    /// How long acquire_reader() waits for a free reader by default.
    static constexpr std::chrono::milliseconds kDefaultReaderWait{10'000};
    /// Newest backups kept in backups/; older ones are deleted after each verified backup.
    static constexpr std::size_t kMaximumBackups = 10;
    /// Most files one commit_scan_batch() accepts.
    static constexpr std::size_t kMaximumScanBatchFiles = 500;

    /// Creates a new library directory (must be empty or absent) with a fresh UUID. All migrations
    /// apply in one transaction; on failure everything created is removed again.
    static Result<std::unique_ptr<Library>> create(const std::filesystem::path& directory, std::string_view name);
    /// Opens an existing library: validates library.json against the database identity, verifies
    /// released migration checksums and their contiguity, takes a verified backup before applying
    /// pending migrations, then runs crash recovery. A newer schema opens read-only (SCHEMA_TOO_NEW
    /// for writes); an older schema cannot be opened read-only (INVALID_ARGUMENT: open it writable
    /// once to upgrade).
    static Result<std::unique_ptr<Library>> open(const std::filesystem::path& directory,
                                                 const OpenOptions& options = {});

    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    /// The library directory.
    const std::filesystem::path& directory() const { return directory_; }
    /// Library UUID; immutable for the library's lifetime.
    const uuids::uuid& uuid() const { return uuid_; }
    /// Cached identity (no database access, never blocks on the writer). catalog_revision is the
    /// latest revision this process committed or read at open; use catalog_revision() for a fresh read.
    LibraryIdentity identity() const;
    /// Current catalog revision read from the database.
    Result<std::int64_t> catalog_revision();
    /// True when the library was opened read-only (requested, or the schema is newer than this build).
    bool read_only() const { return read_only_; }
    /// What crash recovery and migration did when the library was opened.
    const RecoveryReport& recovery_report() const { return recovery_; }

    /// Borrows a reader connection, waiting at most `timeout` while all readers are leased
    /// (RESOURCE_LIMIT afterwards; a thread asking for a second lease cannot deadlock forever).
    Result<ReaderLease> acquire_reader(std::chrono::milliseconds timeout = kDefaultReaderWait);

    // ------------------------------------------------------------ roots and scanning
    /// Registers a root. `case_sensitive` is the result of detect_case_sensitivity() on the root's
    /// folder (root_paths.hpp); nullopt uses default_case_sensitivity(). It decides the location keys
    /// of every file under the root and never changes afterwards.
    Result<RootRecord> add_root(std::string_view name, RootMode mode,
                                std::optional<bool> case_sensitive = std::nullopt);
    /// Live roots ordered by name.
    Result<std::vector<RootRecord>> list_roots();
    /// Records a root's reachability. A change marks the root's samples changed, blocks queued
    /// analysis jobs that became unreachable and unblocks jobs that became reachable again. A no-op
    /// change commits nothing. NOT_FOUND for an unknown root.
    Result<void> set_root_availability(std::int64_t root_id, Availability availability);
    /// Renames a root; its samples' search paths are refreshed. NOT_FOUND for an unknown root.
    Result<void> rename_root(std::int64_t root_id, std::string_view name);

    /// Starts a new scan generation for a root. Interrupted scans are resumable: directories the
    /// previous session completed are reported by completed_directories().
    Result<ScanSession> begin_scan(std::int64_t root_id);
    /// Directories already completed by the latest unfinished scan of this root (resume support).
    Result<std::vector<std::string>> completed_directories(const ScanSession& session);
    /// Answers whether a root-relative path still exists on disk. Supplied by the files module so the
    /// catalog can confirm a same-volume rename (old path gone) without touching the filesystem itself.
    using PathExistsProbe = std::function<bool(std::string_view relative_path)>;

    /// Commits one bounded batch (≤ kMaximumScanBatchFiles files) and marks the given directories
    /// complete, atomically. New files whose volume + file ID match exactly one location whose old
    /// path no longer exists are recognized as renames and keep their sample, annotations and
    /// history. The `path_exists` probes run before the write transaction starts, never while the
    /// writer is held.
    Result<ScanBatchOutcome> commit_scan_batch(const ScanSession& session, std::span<const ScannedFile> files,
                                               std::span<const std::string> completed_directories,
                                               const PathExistsProbe& path_exists);
    /// Stored stat of a live location, for probe skipping.
    struct KnownStat {
        std::optional<std::int64_t> byte_size;
        std::optional<std::int64_t> mtime_ns;
    };
    /// Stored stat of the live locations directly inside one directory of a root, keyed by
    /// mso::path_key(relative_path, root.case_sensitive). The importer skips probing files whose
    /// size and mtime are unchanged.
    Result<std::map<std::string, KnownStat>> known_stats_in_directory(std::int64_t root_id,
                                                                      std::string_view relative_directory);

    /// Records a directory that could not be enumerated; its subtree is unknown, not empty.
    Result<void> record_scan_gap(const ScanSession& session, std::string_view relative_directory, bool denied);
    /// Ends the scan. Unseen locations are marked missing everywhere except inside denied/failed
    /// subtrees (those stay as they were: unknown, not empty), whether or not coverage was complete;
    /// `complete_coverage` reports whether any gap was recorded. Also unblocks analysis jobs whose
    /// samples are reachable again and blocks those that became unreachable.
    Result<ScanFinishOutcome> finish_scan(const ScanSession& session, bool cancelled);

    // ------------------------------------------------------------ user edits and history
    /// Applies a user edit as one undoable action, in chunks of EditCommand::kSamplesPerChunk.
    /// Replaying a completed operation id returns the stored receipt (`replayed`). If a chunk fails,
    /// the committed chunks form a `partial` action and the error is returned; retrying the same
    /// operation id resumes after the last committed chunk while the action is still the newest
    /// history entry, otherwise it returns the stored receipt with `partial` set.
    Result<MutationReceipt> apply_edit(const EditCommand& command);
    /// Applies a structural tag/category/collection edit as one undoable action.
    Result<MutationReceipt> apply_taxonomy(const TaxonomyCommand& command);
    /// Reverts the action at the history cursor. HISTORY_CONFLICT when a later edit changed a field
    /// the action touched: nothing is overwritten, the action is marked `conflict` with one review
    /// item, and the cursor moves past it so older actions stay undoable. Retrying with the same
    /// `operation` returns the stored receipt instead of undoing a second action.
    Result<MutationReceipt> undo(const uuids::uuid& operation = new_uuid());
    /// Re-applies the oldest undone action (restoring `partial` for a partial action); conflicts are
    /// handled like undo's and supersede the rest of the redo tail. Idempotent per `operation`.
    Result<MutationReceipt> redo(const uuids::uuid& operation = new_uuid());
    /// Newest history entries first (superseded entries are omitted).
    Result<std::vector<HistoryEntry>> history(std::size_t limit = 100);

    // ------------------------------------------------------------ taxonomy and collections
    /// Resolves a tag path, creating missing segments; commits a revision only when something was
    /// created. Not undoable (explicit creation); tags created implicitly by apply_edit are.
    Result<std::int64_t> ensure_tag(std::string_view path);
    /// Category counterpart of ensure_tag().
    Result<std::int64_t> ensure_category(std::string_view path);
    /// Live tags with display paths and live-sample counts, ordered by folded path.
    Result<std::vector<TaxonomyNode>> list_tags();
    /// Live categories with display paths and live-sample counts, ordered by folded path.
    Result<std::vector<TaxonomyNode>> list_categories();
    /// Live collections ordered by name.
    Result<std::vector<CollectionRecord>> list_collections();
    /// Folders of a root ordered by path key.
    Result<std::vector<FolderRecord>> list_folders(std::int64_t root_id);

    // ------------------------------------------------------------ reads
    /// Everything the inspector shows about one sample, read in one snapshot. NOT_FOUND when absent.
    Result<SampleDetail> sample_detail(std::int64_t sample_id);
    /// Sample id for a sample UUID, or nullopt.
    Result<std::optional<std::int64_t>> sample_id_by_uuid(const uuids::uuid& uuid);
    /// Open review items, newest first.
    Result<std::vector<ReviewItem>> open_review_items(std::size_t limit = 200);
    /// Number of live (not removed) samples.
    Result<std::int64_t> sample_count();

    // ------------------------------------------------------------ change feed (catalog_outbox)
    /// Changes committed after `after_revision`, bounded by `limit` envelopes. Sets full_reload (with
    /// `revision` = the current revision) when more envelopes remain, when the outbox was pruned past
    /// `after_revision`, when `after_revision` is ahead of the catalog, or when an envelope is of an
    /// older version or lists too many samples.
    Result<ChangeSet> changes_since(std::int64_t after_revision, std::size_t limit = 4096);
    /// Records that `consumer` (1–128 characters) has applied everything up to `revision`, then prunes
    /// the outbox at and below the minimum acknowledged revision of all consumers (consumer_cursors).
    /// A no-op on a read-only library.
    Result<void> acknowledge_changes(std::string_view consumer, std::int64_t revision);

    // ------------------------------------------------------------ analysis results
    /// Effective-metadata update produced by a verified analysis job (system change: logged as
    /// observations, never consumes the user undo cursor, never overrides user values).
    struct AnalysisFacts {
        std::int64_t sample_id = 0;
        std::int64_t content_revision = 0;            ///< fence: ignored with INPUT_CHANGED if stale
        std::optional<std::int64_t> bpm_milli;        ///< 20 000 … 400 000
        std::optional<double> bpm_confidence;         ///< 0 … 1
        std::optional<MusicalKey> key;                ///< pitch class 0 … 11
        std::optional<double> key_confidence;         ///< 0 … 1
        std::optional<double> peak_dbfs;              ///< finite
        std::optional<double> rms_dbfs;               ///< finite
        std::optional<std::string> byte_sha256;       ///< 64 hex digits
        std::optional<std::string> pcm_sha256;        ///< 64 hex digits
        std::optional<std::string> waveform_segment;  ///< safe relative path under derived/
        std::string tempo_algorithm, key_algorithm;
        bool tempo_requested = false, key_requested = false;
    };
    /// Publishes analysis facts outside the queue (tests, imports of trusted results). Out-of-range
    /// facts are rejected with INVALID_ARGUMENT before anything is written.
    Result<void> record_analysis(const AnalysisFacts& facts);

    // ------------------------------------------------------------ durable analysis queue (P3.1)
    /// An explicit run over chosen samples. The selection is frozen: sample ids and content
    /// revisions are snapshotted into jobs; later matches are not added.
    struct AnalysisRunRequest {
        std::string profile_name;              ///< e.g. "Musical"
        std::vector<std::string> stages;       ///< analysis stage names (mso/domain/analysis_flags.hpp)
        std::vector<std::int64_t> sample_ids;  ///< duplicates are ignored
        bool force = false;                    ///< re-analyze even when current results exist
    };
    /// Progress of a run.
    struct AnalysisRunStatus {
        std::int64_t id = 0;
        uuids::uuid uuid;
        std::string profile_name;
        std::string state;
        std::int64_t requested_at = 0;
        std::int64_t queued = 0, running = 0, completed = 0, failed = 0, cancelled = 0, blocked = 0;
        std::int64_t skipped = 0;  ///< selected samples not queued (removed, or results already current)
        std::int64_t stale = 0;    ///< jobs discarded because the sample changed while it was analyzed
    };
    /// A leased job handed to a worker. Every result is fenced against these values.
    struct JobLease {
        std::int64_t job_id = 0;
        std::int64_t run_id = 0;
        uuids::uuid job_uuid;
        std::string lease_token;
        std::int64_t cancel_epoch = 0;
        std::int64_t sample_id = 0;
        std::int64_t content_revision = 0;
        std::vector<std::string> stages;
        std::int64_t attempt = 0;
    };
    /// Control of a whole run.
    enum class RunControl { Pause, Resume, Cancel };
    /// Attempts per job before a transient failure or an expired lease fails it permanently.
    static constexpr int kMaximumJobAttempts = 3;

    /// Creates a run for explicit samples (set-based). Jobs of unreachable samples start blocked.
    Result<AnalysisRunStatus> create_analysis_run(const AnalysisRunRequest& request);
    /// Creates an analysis run for every live sample of a root (set-based INSERT … SELECT; skips
    /// samples whose results are current unless force). Used by automatic analysis.
    Result<AnalysisRunStatus> create_analysis_run_for_root(std::int64_t root_id, std::string_view profile_name,
                                                           const std::vector<std::string>& stages, bool force);
    /// Pauses, resumes or cancels a run. NOT_FOUND for an unknown run; commits nothing when the run
    /// is already in the requested state.
    Result<void> control_run(std::int64_t run_id, RunControl control);
    /// Newest runs first with per-state job counts.
    Result<std::vector<AnalysisRunStatus>> list_analysis_runs(std::size_t limit = 50);
    /// Leases the next job whose audio is reachable (fair: oldest run first, then priority, then id).
    /// Unreachable jobs never hold up reachable ones. Expired leases are reclaimed first; a job whose
    /// lease expired kMaximumJobAttempts times fails permanently.
    Result<std::optional<JobLease>> lease_next_job(std::chrono::milliseconds lease_duration);
    /// Extends a held lease; CANCELLED when it is no longer held.
    Result<void> renew_lease(const JobLease& lease, std::chrono::milliseconds lease_duration);
    /// Publishes a verified result. The facts' sample and content revision must match the lease
    /// (INPUT_CHANGED otherwise). Late or fenced completions (cancelled run, changed epoch, lost
    /// lease, changed content) are discarded with CANCELLED or INPUT_CHANGED; out-of-range facts fail
    /// the job permanently with INVALID_ARGUMENT.
    Result<void> complete_job(const JobLease& lease, const AnalysisFacts& facts);
    /// Records a failure; transient errors are retried with backoff up to kMaximumJobAttempts
    /// attempts. A lease that is no longer held is ignored (success); other errors are returned.
    Result<void> fail_job(const JobLease& lease, ErrorCode code, bool transient);
    /// Returns a job to the queue without counting an attempt (e.g. the worker was shut down).
    Result<void> release_job(const JobLease& lease);

    // ------------------------------------------------------------ maintenance
    /// Verified backup: online backup API into backups/, then integrity + foreign-key + identity check
    /// of the copy. `reason` is sanitized into the file name; at most kMaximumBackups are kept.
    Result<std::filesystem::path> create_backup(std::string_view reason);
    /// Integrity, foreign-key and FTS checks in one line.
    Result<std::string> integrity_report();
    /// Rebuilds the FTS index from sample_search in a maintenance transaction and verifies counts.
    Result<void> rebuild_search_index();

private:
    friend struct LibraryTestAccess;
    Library() = default;
    Result<void> initialize_new(std::string_view name);
    Result<void> verify_and_migrate();
    Result<void> recover_after_crash();
    Result<void> load_cached_identity();
    Result<void> ensure_writable() const;
    /// Commits a transaction that advanced the revision to `revision` and publishes it to identity().
    Result<void> commit_revision(sqlite::WriteTransaction& transaction, std::int64_t revision);
    /// Shared body of undo() and redo() (`undoing` selects the direction).
    Result<MutationReceipt> step_history(const uuids::uuid& operation, bool undoing);

    std::filesystem::path directory_;
    std::filesystem::path catalog_file_;
    uuids::uuid uuid_;
    bool read_only_ = false;
    RecoveryReport recovery_;
    std::string name_;                                 ///< cached at open (never changes while open)
    int schema_version_ = 0;                           ///< cached at open
    std::atomic<std::int64_t> published_revision_{0};  ///< latest revision committed or read

    std::mutex writer_mutex_;  ///< one serialized writer per library
    sqlite::Connection writer_;

    std::shared_ptr<detail::ReaderPool> readers_;
};

}  // namespace mso::catalog
