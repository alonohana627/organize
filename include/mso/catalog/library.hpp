#pragma once
/// @file
/// A portable library: catalog directory with library.json, catalog.sqlite and versioned sidecars
/// (docs/02_ARCHITECTURE.md §4). The Library owns one serialized writer connection and a bounded
/// pool of reader connections. Every committed mutation increments catalog_revision once and
/// appends an outbox envelope in the same transaction (docs/03_DATA_AND_SEARCH.md §3).
#include <chrono>
#include <condition_variable>
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

class Library;

/// RAII loan of a reader connection from a library's pool. Return happens on destruction.
class ReaderLease {
public:
    ReaderLease(ReaderLease&& other) noexcept;
    ReaderLease& operator=(ReaderLease&&) = delete;
    ReaderLease(const ReaderLease&) = delete;
    ~ReaderLease();
    sqlite::Connection& connection() { return *connection_; }

private:
    friend class Library;
    ReaderLease(Library* owner, std::unique_ptr<sqlite::Connection> connection);
    Library* owner_;
    std::unique_ptr<sqlite::Connection> connection_;
};

/// Options for opening a library.
struct OpenOptions {
    bool read_only = false;  ///< also forced when the catalog schema is newer than this build
    int max_readers = 4;     ///< bounded reader pool
};

/// Outcome of the startup recovery pass.
struct RecoveryReport {
    std::int64_t interrupted_scans = 0;
    std::int64_t partial_history_actions = 0;
    std::int64_t interrupted_jobs = 0;
    bool migrated = false;
    std::optional<std::filesystem::path> pre_migration_backup;
};

class Library {
public:
    static constexpr std::string_view kCatalogFileName = "catalog.sqlite";
    static constexpr std::string_view kManifestFileName = "library.json";
    static constexpr int kLibraryFormatVersion = 1;

    /// Creates a new library directory (must be empty or absent) with a fresh UUID.
    static Result<std::unique_ptr<Library>> create(const std::filesystem::path& directory, std::string_view name);
    /// Opens an existing library: validates library.json against the database identity, verifies
    /// released migration checksums, takes a verified backup before applying pending migrations,
    /// then runs crash recovery. A newer schema opens read-only (SCHEMA_TOO_NEW for writes).
    static Result<std::unique_ptr<Library>> open(const std::filesystem::path& directory,
                                                 const OpenOptions& options = {});

    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    const std::filesystem::path& directory() const { return directory_; }
    const uuids::uuid& uuid() const { return uuid_; }  ///< immutable for the library's lifetime
    LibraryIdentity identity();
    bool read_only() const { return read_only_; }
    const RecoveryReport& recovery_report() const { return recovery_; }

    /// Borrows a reader connection (blocks while all readers are leased).
    Result<ReaderLease> acquire_reader();

    // ------------------------------------------------------------ roots and scanning
    Result<RootRecord> add_root(std::string_view name, RootMode mode);
    Result<std::vector<RootRecord>> list_roots();
    Result<void> set_root_availability(std::int64_t root_id, Availability availability);
    Result<void> rename_root(std::int64_t root_id, std::string_view name);

    /// Starts a new scan generation for a root. Interrupted scans are resumable: directories the
    /// previous session completed are reported by completed_directories().
    Result<ScanSession> begin_scan(std::int64_t root_id);
    /// Directories already completed by the latest unfinished scan of this root (resume support).
    Result<std::vector<std::string>> completed_directories(const ScanSession& session);
    /// Answers whether a root-relative path still exists on disk. Supplied by the files module so the
    /// catalog can confirm a same-volume rename (old path gone) without touching the filesystem itself.
    using PathExistsProbe = std::function<bool(std::string_view relative_path)>;

    /// Commits one bounded batch (≤500 files) and marks the given directories complete, atomically.
    /// New files whose volume + file ID match exactly one location whose old path no longer exists
    /// are recognized as renames and keep their sample, annotations and history.
    Result<ScanBatchOutcome> commit_scan_batch(const ScanSession& session, std::span<const ScannedFile> files,
                                               std::span<const std::string> completed_directories,
                                               const PathExistsProbe& path_exists);
    /// Stored stat of the live locations directly inside one directory of a root, keyed by path
    /// key. The importer skips probing files whose size and mtime are unchanged.
    struct KnownStat {
        std::optional<std::int64_t> byte_size;
        std::optional<std::int64_t> mtime_ns;
    };
    Result<std::map<std::string, KnownStat>> known_stats_in_directory(std::int64_t root_id,
                                                                      std::string_view relative_directory);

    /// Records a directory that could not be enumerated; its subtree is unknown, not empty.
    Result<void> record_scan_gap(const ScanSession& session, std::string_view relative_directory, bool denied);
    /// Ends the scan. Only with complete coverage are unseen locations marked missing, and never
    /// inside a denied/failed subtree.
    Result<ScanFinishOutcome> finish_scan(const ScanSession& session, bool cancelled);

    // ------------------------------------------------------------ user edits and history
    Result<MutationReceipt> apply_edit(const EditCommand& command);
    Result<MutationReceipt> apply_taxonomy(const TaxonomyCommand& command);
    /// Reverts the action at the history cursor; HISTORY_CONFLICT when a later edit changed a
    /// field the action touched (a review item is recorded, nothing is overwritten).
    Result<MutationReceipt> undo();
    Result<MutationReceipt> redo();
    Result<std::vector<HistoryEntry>> history(std::size_t limit = 100);

    // ------------------------------------------------------------ taxonomy and collections
    Result<std::int64_t> ensure_tag(std::string_view path);  ///< creates missing path segments
    Result<std::int64_t> ensure_category(std::string_view path);
    Result<std::vector<TaxonomyNode>> list_tags();
    Result<std::vector<TaxonomyNode>> list_categories();
    Result<std::vector<CollectionRecord>> list_collections();
    Result<std::vector<FolderRecord>> list_folders(std::int64_t root_id);

    // ------------------------------------------------------------ reads
    Result<SampleDetail> sample_detail(std::int64_t sample_id);
    Result<std::optional<std::int64_t>> sample_id_by_uuid(const uuids::uuid& uuid);
    Result<std::vector<ReviewItem>> open_review_items(std::size_t limit = 200);
    Result<std::int64_t> sample_count();

    // ------------------------------------------------------------ analysis results
    /// Effective-metadata update produced by a verified analysis job (system change: logged as
    /// observations, never consumes the user undo cursor, never overrides user values).
    struct AnalysisFacts {
        std::int64_t sample_id = 0;
        std::int64_t content_revision = 0;  ///< fence: ignored with INPUT_CHANGED if stale
        std::optional<std::int64_t> bpm_milli;
        std::optional<double> bpm_confidence;
        std::optional<MusicalKey> key;
        std::optional<double> key_confidence;
        std::optional<double> peak_dbfs;
        std::optional<double> rms_dbfs;
        std::optional<std::string> byte_sha256;
        std::optional<std::string> pcm_sha256;
        std::optional<std::string> waveform_segment;  ///< relative path under derived/
        std::string tempo_algorithm, key_algorithm;
        bool tempo_requested = false, key_requested = false;
    };
    Result<void> record_analysis(const AnalysisFacts& facts);

    // ------------------------------------------------------------ durable analysis queue (P3.1)
    /// A run is created only by an explicit user action. The selection is frozen: sample ids and
    /// content revisions are snapshotted into jobs; later matches are not added.
    struct AnalysisRunRequest {
        std::string profile_name;         ///< e.g. "Musical"
        std::vector<std::string> stages;  ///< analysis stage names
        std::vector<std::int64_t> sample_ids;
        bool force = false;  ///< re-analyze even when current results exist
    };
    struct AnalysisRunStatus {
        std::int64_t id = 0;
        uuids::uuid uuid;
        std::string profile_name;
        std::string state;
        std::int64_t requested_at = 0;
        std::int64_t queued = 0, running = 0, completed = 0, failed = 0, skipped = 0, cancelled = 0, blocked = 0;
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
    enum class RunControl { Pause, Resume, Cancel };

    Result<AnalysisRunStatus> create_analysis_run(const AnalysisRunRequest& request);
    Result<void> control_run(std::int64_t run_id, RunControl control);
    Result<std::vector<AnalysisRunStatus>> list_analysis_runs(std::size_t limit = 50);
    /// Leases the next queued job of a running/queued run (fair: oldest run first, then id).
    Result<std::optional<JobLease>> lease_next_job(std::chrono::milliseconds lease_duration);
    Result<void> renew_lease(const JobLease& lease, std::chrono::milliseconds lease_duration);
    /// Publishes a verified result. Late or fenced completions (cancelled run, changed epoch,
    /// lost lease, changed content) are discarded with CANCELLED or INPUT_CHANGED.
    Result<void> complete_job(const JobLease& lease, const AnalysisFacts& facts);
    /// Records a failure; transient errors are retried up to three attempts.
    Result<void> fail_job(const JobLease& lease, ErrorCode code, bool transient);
    /// Returns a job to the queue without counting an attempt (e.g. the worker was shut down).
    Result<void> release_job(const JobLease& lease);

    // ------------------------------------------------------------ maintenance
    /// Verified backup: online backup API into backups/, then integrity + foreign-key check of the copy.
    Result<std::filesystem::path> create_backup(std::string_view reason);
    Result<std::string> integrity_report();
    /// Rebuilds the FTS index from sample_search in a maintenance transaction and verifies counts.
    Result<void> rebuild_search_index();

    // Internal: used by sibling catalog translation units.
    sqlite::Connection& writer_connection() { return writer_; }
    std::mutex& writer_mutex() { return writer_mutex_; }

private:
    friend class ReaderLease;
    Library() = default;
    Result<void> initialize_new(std::string_view name);
    Result<void> verify_and_migrate();
    Result<void> recover_after_crash();
    void release_reader(std::unique_ptr<sqlite::Connection> connection);
    Result<void> ensure_writable() const;

    std::filesystem::path directory_;
    std::filesystem::path catalog_file_;
    uuids::uuid uuid_;
    bool read_only_ = false;
    RecoveryReport recovery_;

    std::mutex writer_mutex_;  ///< one serialized writer per library
    sqlite::Connection writer_;

    std::mutex readers_mutex_;
    std::condition_variable reader_available_;
    std::vector<std::unique_ptr<sqlite::Connection>> idle_readers_;
    int open_readers_ = 0;
    int max_readers_ = 4;
};

}  // namespace mso::catalog
