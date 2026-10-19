#pragma once
/// @file
/// Reference-in-place import of one root (docs/02_ARCHITECTURE.md §6): enumerate → stat/identity →
/// bounded metadata probe (only new or changed files) → batch commit of ≤500 rows → progress.
/// Import never decodes whole files, hashes, or analyzes; it is cancellable and resumable.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

#include "mso/catalog/library.hpp"

namespace mso::files {

/// Monotonic progress counters, delivered at most ~10 times per second.
struct ImportProgress {
    std::int64_t directories_scanned = 0;
    std::int64_t files_discovered = 0;
    std::int64_t files_probed = 0;
    std::int64_t files_committed = 0;
    std::int64_t added = 0;
    std::int64_t updated = 0;
    std::int64_t relinked = 0;
    std::int64_t probe_failures = 0;
    std::int64_t inaccessible_directories = 0;
    std::string current_directory;
    bool finished = false;
    bool cancelled = false;
};

using ImportProgressCallback = std::function<void(const ImportProgress&)>;

struct ImportOptions {
    std::size_t probe_threads = 4;  ///< bounded parallel probes
    std::size_t batch_size = 500;   ///< rows per catalog transaction
    std::chrono::milliseconds batch_interval{100};
    bool follow_reparse_points = false;  ///< symlinks/junctions are not followed by default
};

/// Final outcome of a scan.
struct ImportSummary {
    ImportProgress progress;
    catalog::ScanFinishOutcome finish;
};

/// Scans `root_path` (the machine-local binding of `root_id`) into the library. Returns
/// CANCELLED (with committed work kept and a resume checkpoint) when `stop` is requested.
/// Errors from the catalog abort the scan; per-file probe errors only mark that file.
Result<ImportSummary> import_root(catalog::Library& library, std::int64_t root_id,
                                  const std::filesystem::path& root_path, std::stop_token stop,
                                  const ImportProgressCallback& progress = {}, const ImportOptions& options = {});

}  // namespace mso::files
