#pragma once
/// @file
/// Reference-in-place import of one root (docs/02_ARCHITECTURE.md §6): enumerate → stat/identity →
/// bounded metadata probe (only new or changed files) → batch commit of ≤500 rows → progress.
/// Import never decodes whole files, hashes, or analyzes; it is cancellable and resumable, and it
/// never modifies, moves or deletes a source file (invariant 11).
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

#include "mso/catalog/library.hpp"
#include "mso/media/media.hpp"

namespace mso::files {

/// Monotonic progress counters, delivered at most ~10 times per second and once more at the end.
struct ImportProgress {
    std::int64_t directories_scanned = 0;       ///< directories listed completely
    std::int64_t files_discovered = 0;          ///< candidate audio files found by enumeration
    std::int64_t files_probed = 0;              ///< files whose metadata was probed (new or changed)
    std::int64_t files_committed = 0;           ///< files written to the catalog (probed or unchanged)
    std::int64_t added = 0;                     ///< new samples
    std::int64_t updated = 0;                   ///< samples whose content possibly changed
    std::int64_t relinked = 0;                  ///< renames recognised by file identity
    std::int64_t probe_failures = 0;            ///< files that vanished, were locked, or failed to probe
    std::int64_t inaccessible_directories = 0;  ///< directories recorded as gaps (unknown, not empty)
    std::int64_t links_skipped = 0;             ///< symbolic links and junctions not followed (by option)
    std::int64_t link_cycles_skipped = 0;       ///< followed links to a directory this scan already reached
    std::int64_t cloud_only_skipped = 0;        ///< cloud-sync files not on this device (reading would download them)
    std::int64_t unsupported_entries_skipped = 0;  ///< devices, FIFOs and unknown reparse points
    std::int64_t unsafe_paths_skipped = 0;  ///< audio files whose path cannot be stored (invalid UTF-8, reserved names)
    std::string current_directory;          ///< root-relative directory last completed
    bool finished = false;                  ///< the final report of this scan
    bool cancelled = false;                 ///< the scan stopped on request (committed work is kept)
};

/// Receives progress on the importing thread.
using ImportProgressCallback = std::function<void(const ImportProgress&)>;

/// Tuning for one import.
struct ImportOptions {
    std::size_t probe_threads = 4;                  ///< bounded parallel probes
    std::size_t batch_size = 500;                   ///< rows per catalog transaction
    std::chrono::milliseconds batch_interval{100};  ///< a non-empty batch is committed at least this often
    /// Follow symbolic links and junctions (with cycle detection). Off by default: links are
    /// skipped and counted in ImportProgress::links_skipped.
    bool follow_reparse_points = false;
    media::ProbeLimits probe_limits;  ///< byte budget and deadline of each metadata probe
};

/// Final outcome of a scan.
struct ImportSummary {
    ImportProgress progress;
    catalog::ScanFinishOutcome finish;
};

/// Scans `root_path` (the machine-local binding of `root_id`) into the library. Returns
/// CANCELLED (with committed work kept and a resume checkpoint) when `stop` is requested; the
/// progress callback still receives a final report (finished and cancelled set) on every exit path.
/// Errors from the catalog abort the scan; per-file probe errors only mark that file.
/// Entries skipped on purpose (cloud-only files, links that are not followed, link cycles,
/// unsupported entries) exist, so their catalogued locations never become missing: a skipped file
/// is marked seen with its stored stat, and a skipped directory is recorded as a gap (its subtree
/// unknown, so ScanFinishOutcome::complete_coverage is false).
/// Errors: SourceOffline when the root is unreachable (the root is marked offline), catalog
/// errors, Cancelled, Internal when enumeration itself failed unexpectedly or the scan could not
/// be set up (the session is then closed with its resume point).
[[nodiscard]] Result<ImportSummary> import_root(catalog::Library& library, std::int64_t root_id,
                                                const std::filesystem::path& root_path, std::stop_token stop,
                                                const ImportProgressCallback& progress = {},
                                                const ImportOptions& options = {});

}  // namespace mso::files
