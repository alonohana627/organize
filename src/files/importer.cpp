// Import of one root: an enumeration thread feeds a bounded queue; the calling thread stats,
// probes (in a bounded pool) and commits batches.
#include "mso/files/importer.hpp"

#include <readerwriterqueue/readerwritercircularbuffer.h>
#include <BS_thread_pool.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <future>
#include <map>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "mso/files/detail/file_identity.hpp"
#include "mso/media/detail/native_file_support.hpp"
#include "mso/media/media.hpp"

namespace mso::files {

namespace {

using Clock = std::chrono::steady_clock;
using media::detail::native_path;

constexpr std::size_t kEnumerationQueueCapacity = 4096;
constexpr auto kQueueWait = std::chrono::milliseconds(50);
constexpr auto kProgressInterval = std::chrono::milliseconds(100);
/// Directories nested deeper than this are recorded as gaps. Enumeration keeps one open listing
/// per level, so the depth bounds its memory and handles.
constexpr std::size_t kMaxDirectoryDepth = 256;

/// A candidate audio file found by enumeration.
struct FileFound {
    std::string relative_path;  ///< '/'-separated, valid UTF-8, safe to store
    std::string directory;      ///< relative directory, "" for the root
    std::filesystem::path absolute_path;
};

/// All files of a directory (and its subtree) have been enumerated.
struct DirectoryDone {
    std::string directory;
};

/// A directory could not be listed (or only partly): its subtree is unknown, not empty.
struct DirectoryGap {
    std::string directory;
    bool denied = false;
};

/// Why an entry was left out of the import; each reason has an ImportProgress counter.
enum class SkipReason { Link, LinkCycle, CloudOnly, Unsupported, UnsafePath };

/// An entry deliberately not imported.
struct EntrySkipped {
    SkipReason reason = SkipReason::Unsupported;
};

/// Sentinel: enumeration finished.
struct EnumerationEnd {};

/// Sentinel: enumeration stopped on an unexpected exception (the scan fails).
struct EnumerationFailed {
    std::string detail;
};

using EnumeratedItem =
    std::variant<FileFound, DirectoryDone, DirectoryGap, EntrySkipped, EnumerationEnd, EnumerationFailed>;
using EnumerationQueue = moodycamel::BlockingReaderWriterCircularBuffer<EnumeratedItem>;

/// Volume and file id of a directory, for detecting link cycles.
using DirectoryKey = std::pair<std::string, files::detail::FileId>;

/// Walks the root depth-first on its own thread, feeding the bounded queue. Backpressure blocks
/// enumeration instead of accumulating paths in memory, and the walk keeps one open listing per
/// directory level instead of a list of pending directories, so memory stays bounded by depth
/// however wide the tree is.
class Enumerator {
public:
    Enumerator(std::filesystem::path root, std::set<std::string> completed, bool follow_links, EnumerationQueue& queue)
        : root_(std::move(root)), completed_(std::move(completed)), follow_links_(follow_links), queue_(queue) {}

    /// Thread body. Always ends the stream with EnumerationEnd, or EnumerationFailed when an
    /// exception escaped the walk (unless `stop` was requested first).
    void run(std::stop_token stop) noexcept {
        try {
            walk(stop);
            push(EnumerationEnd{}, stop);
        } catch (const std::exception& error) {
            EnumerationFailed failure;
            try {
                failure.detail = error.what();
            } catch (const std::exception&) {
                failure.detail.clear();  // no memory even for the message: report without detail
            }
            push(std::move(failure), stop);
        }
    }

private:
    /// One directory being listed.
    struct OpenDirectory {
        std::filesystem::path relative;
        std::string text;  ///< relative directory as stored ("" for the root)
        std::filesystem::directory_iterator iterator;
        bool committed = false;           ///< completed by an interrupted earlier scan: files are not re-sent
        bool listing_failed = false;      ///< the listing broke off part-way
        bool denied = false;              ///< ... because access was denied
        std::optional<DirectoryKey> key;  ///< identity, when following links
    };

    /// Depth-first walk over a stack of open listings.
    void walk(const std::stop_token& stop) {
        std::optional<DirectoryKey> root_key;
        if (follow_links_) root_key = directory_key(root_);
        open_directory(std::filesystem::path(), root_key, stop);
        while (!stack_.empty() && !stop.stop_requested()) {
            OpenDirectory& top = stack_.back();
            if (top.iterator == std::filesystem::directory_iterator()) {
                finish_directory(stop);
                continue;
            }
            const std::filesystem::directory_entry entry = *top.iterator;
            std::error_code error;
            top.iterator.increment(error);
            if (error) {
                // What was seen stays valid; the rest of this listing is unknown.
                top.listing_failed = true;
                top.denied = error == std::errc::permission_denied;
                top.iterator = std::filesystem::directory_iterator();
            }
            visit_entry(entry, stop);  // may push a subdirectory: `top` is not used after this
        }
    }

    /// Opens the listing of a directory and pushes it; a directory that cannot be listed becomes a gap.
    void open_directory(const std::filesystem::path& relative, std::optional<DirectoryKey> key,
                        const std::stop_token& stop) {
        const std::string text = path_to_utf8(relative);
        std::error_code error;
        std::filesystem::directory_iterator iterator(native_path(root_ / relative), error);
        if (error) {
            push(DirectoryGap{text, error == std::errc::permission_denied}, stop);
            return;
        }
        stack_.push_back(OpenDirectory{relative, text, std::move(iterator), completed_.contains(text), false, false,
                                       std::move(key)});
    }

    /// Pops the finished directory and reports it as done, or as a gap when its listing failed.
    void finish_directory(const std::stop_token& stop) {
        OpenDirectory finished = std::move(stack_.back());
        stack_.pop_back();
        if (finished.listing_failed) {
            push(DirectoryGap{std::move(finished.text), finished.denied}, stop);
        } else {
            push(DirectoryDone{std::move(finished.text)}, stop);
        }
    }

    /// Identity of a directory (following links); nullopt when it cannot be read.
    static std::optional<DirectoryKey> directory_key(const std::filesystem::path& directory) {
        auto identity = files::detail::read_file_identity(directory);
        if (!identity || !identity->volume_id || !identity->file_id) return std::nullopt;
        return DirectoryKey{*identity->volume_id, *identity->file_id};
    }

    /// Applies the link policy to one entry, then descends into directories and sends candidate files.
    void visit_entry(const std::filesystem::directory_entry& entry, const std::stop_token& stop) {
        const std::filesystem::path relative = stack_.back().relative / entry.path().filename();
        switch (files::detail::classify_link(entry.path())) {
            case files::detail::LinkKind::CloudOnly:
                push(EntrySkipped{SkipReason::CloudOnly}, stop);  // never trigger a download
                return;
            case files::detail::LinkKind::OtherReparsePoint:
                push(EntrySkipped{SkipReason::Unsupported}, stop);
                return;
            case files::detail::LinkKind::SymbolicLink:
            case files::detail::LinkKind::Junction:
                if (!follow_links_) {
                    push(EntrySkipped{SkipReason::Link}, stop);
                    return;
                }
                break;
            case files::detail::LinkKind::None:
            case files::detail::LinkKind::CloudPlaceholder:  // available locally: an ordinary entry
            case files::detail::LinkKind::Deduplicated:      // reads like an ordinary file
                break;
        }
        std::error_code status_error;
        if (entry.is_directory(status_error)) {
            descend(relative, entry.path(), stop);
            return;
        }
        if (!entry.is_regular_file(status_error)) {
            // FIFOs, sockets and devices are never opened; dangling links simply vanish.
            if (!status_error && entry.exists(status_error)) push(EntrySkipped{SkipReason::Unsupported}, stop);
            return;
        }
        const OpenDirectory& parent = stack_.back();
        if (parent.committed) return;
        const std::string extension = to_lower_ascii(path_to_utf8(entry.path().extension()));
        if (extension.size() < 2 || !media::is_candidate_extension(std::string_view(extension).substr(1))) return;
        std::string relative_text = path_to_utf8(relative);
        if (!is_valid_utf8(relative_text) || !is_safe_relative_path(relative_text)) {
            push(EntrySkipped{SkipReason::UnsafePath}, stop);  // the catalog could not store this path
            return;
        }
        push(FileFound{std::move(relative_text), parent.text, entry.path()}, stop);
    }

    /// Enters a subdirectory unless that would exceed the depth limit or (when following links)
    /// revisit a directory that is already open further up: a link cycle.
    void descend(const std::filesystem::path& relative, const std::filesystem::path& absolute,
                 const std::stop_token& stop) {
        if (stack_.size() >= kMaxDirectoryDepth) {
            push(DirectoryGap{path_to_utf8(relative), false}, stop);
            return;
        }
        std::optional<DirectoryKey> key;
        if (follow_links_) {
            key = directory_key(absolute);
            const bool cycle =
                key && std::ranges::any_of(stack_, [&key](const OpenDirectory& open) { return open.key == key; });
            if (cycle) {
                push(EntrySkipped{SkipReason::LinkCycle}, stop);
                return;
            }
        }
        open_directory(relative, std::move(key), stop);
    }

    /// Enqueues an item, waiting while the queue is full; gives up when `stop` is requested. The
    /// queue's storage is allocated up front and every alternative moves without allocating.
    void push(EnumeratedItem item, const std::stop_token& stop) noexcept {
        while (!stop.stop_requested()) {
            if (queue_.wait_enqueue_timed(std::move(item),
                                          std::chrono::duration_cast<std::chrono::microseconds>(kQueueWait).count()))
                return;
        }
    }

    std::filesystem::path root_;
    std::set<std::string> completed_;
    bool follow_links_;
    EnumerationQueue& queue_;
    std::vector<OpenDirectory> stack_;  ///< open listings, root first; at most kMaxDirectoryDepth
};

/// Bounded probe of one file plus filename heuristics, converted to catalog observations.
catalog::ObservedMetadata observe(const media::ProbeResult& probe, std::string_view relative_path) {
    catalog::ObservedMetadata metadata;
    metadata.format = probe.format;
    metadata.codec = probe.codec;
    metadata.sample_rate = probe.sample_rate;
    metadata.channels = probe.channels;
    metadata.bit_depth = probe.bit_depth;
    metadata.frames = probe.frames;
    metadata.duration_us = probe.duration_us;
    metadata.duration_exact = probe.duration_exact;
    metadata.partial = probe.partial;
    metadata.loop_count = static_cast<std::int64_t>(probe.loops.size());
    const auto& tags = probe.tags;
    for (const auto* text : {&tags.title, &tags.artist, &tags.album, &tags.genre, &tags.comment})
        if (*text && !(*text)->empty()) metadata.embedded_text += (metadata.embedded_text.empty() ? "" : " ") + **text;
    for (const auto& keyword : tags.keywords)
        metadata.embedded_text += (metadata.embedded_text.empty() ? "" : " ") + keyword;
    metadata.embedded_bpm_milli = tags.bpm_milli;
    if (tags.key) metadata.embedded_key = tags.key;

    const auto stem = path_from_utf8(relative_path).stem();
    const auto hints = media::filename_hints(path_to_utf8(stem));
    metadata.filename_bpm_milli = hints.bpm_milli;
    if (hints.key) metadata.filename_key = hints.key;
    return metadata;
}

/// What a probe task produced: metadata, an error code, or neither (cancelled before it ran).
struct ProbeOutcome {
    std::optional<catalog::ObservedMetadata> metadata;
    std::optional<ErrorCode> error;
};

/// Probes one file. Runs on a pool thread and owns copies of everything it uses, so it never
/// refers to the committer's state. Never throws: an exception becomes an Internal probe error.
ProbeOutcome probe_task(const std::filesystem::path& path, const std::string& relative_path,
                        const std::stop_token& stop, const media::ProbeLimits& limits) noexcept {
    try {
        if (stop.stop_requested()) return {};
        auto probe = media::probe_file(path, limits);
        if (!probe) return {std::nullopt, probe.error().code};
        return {observe(*probe, relative_path), std::nullopt};
    } catch (const std::exception&) {
        return {std::nullopt, ErrorCode::Internal};
    }
}

/// Consumes the enumeration queue on the calling thread: stats, probes in parallel, commits.
class ImportCommitter {
public:
    /// `case_sensitive` is the root's path-key rule (RootRecord::case_sensitive).
    ImportCommitter(catalog::Library& library, std::int64_t root_id, bool case_sensitive,
                    std::filesystem::path root_path, const catalog::ScanSession& session, const ImportOptions& options,
                    const ImportProgressCallback& callback)
        : library_(library),
          root_id_(root_id),
          case_sensitive_(case_sensitive),
          root_path_(std::move(root_path)),
          session_(session),
          options_(options),
          callback_(callback),
          probe_pool_(std::max<std::size_t>(1, options.probe_threads)) {}

    /// Drains the queue until EnumerationEnd, committing batches as they fill or come due.
    /// Returns early (committing nothing more) once `stop` is requested.
    Result<void> run(EnumerationQueue& queue, const std::stop_token& stop) {
        auto batch_started = Clock::now();
        while (true) {
            EnumeratedItem item;
            const bool received = queue.wait_dequeue_timed(
                item, std::chrono::duration_cast<std::chrono::microseconds>(kQueueWait).count());
            if (stop.stop_requested()) return {};  // nothing after the cancellation fence is committed
            if (received) {
                if (std::holds_alternative<EnumerationEnd>(item)) break;
                if (auto handled = handle(std::move(item)); !handled) return handled;
            }
            // The catalog refuses batches above kMaximumScanBatchFiles.
            const std::size_t batch_size =
                std::clamp<std::size_t>(options_.batch_size, 1, catalog::Library::kMaximumScanBatchFiles);
            const bool batch_full = pending_files_.size() >= batch_size;
            const bool batch_due = !pending_files_.empty() && Clock::now() - batch_started >= options_.batch_interval;
            if (batch_full || batch_due || (!pending_directories_.empty() && pending_files_.empty())) {
                if (auto committed = commit_batch(stop); !committed) return committed;
                batch_started = Clock::now();
            }
            // Only news is reported, so the first report already reflects the first item handled.
            if (received) {
                if (auto reported = report(false); !reported) return reported;
            }
        }
        return commit_batch(stop);
    }

    /// Counters so far (the final report is completed by import_root()).
    ImportProgress& progress() { return progress_; }

    /// Delivers progress at most every kProgressInterval, or now when `force` is set.
    /// Errors: Internal when the callback threw.
    Result<void> report(bool force) {
        if (!callback_) return {};
        const auto now = Clock::now();
        if (!force && now - last_report_ < kProgressInterval) return {};
        last_report_ = now;
        try {
            callback_(progress_);
        } catch (const std::exception& error) {
            return fail(ErrorCode::Internal, std::format("import progress callback failed: {}", error.what()));
        }
        return {};
    }

private:
    /// Accounts for one enumerated item; files and finished directories wait for the next batch.
    Result<void> handle(EnumeratedItem item) {
        if (auto* found = std::get_if<FileFound>(&item)) {
            ++progress_.files_discovered;
            pending_files_.push_back(std::move(*found));
            return {};
        }
        if (auto* done = std::get_if<DirectoryDone>(&item)) {
            ++progress_.directories_scanned;
            progress_.current_directory = done->directory;
            pending_directories_.push_back(std::move(done->directory));
            return {};
        }
        if (auto* gap = std::get_if<DirectoryGap>(&item)) {
            ++progress_.inaccessible_directories;
            return library_.record_scan_gap(session_, gap->directory, gap->denied);
        }
        if (auto* skipped = std::get_if<EntrySkipped>(&item)) {
            count_skip(skipped->reason);
            return {};
        }
        if (auto* failed = std::get_if<EnumerationFailed>(&item)) {
            return fail(ErrorCode::Internal, std::format("enumerating the folder failed: {}", failed->detail));
        }
        return {};
    }

    /// Increments the ImportProgress counter of a skip reason.
    void count_skip(SkipReason reason) {
        switch (reason) {
            case SkipReason::Link:
                ++progress_.links_skipped;
                break;
            case SkipReason::LinkCycle:
                ++progress_.link_cycles_skipped;
                break;
            case SkipReason::CloudOnly:
                ++progress_.cloud_only_skipped;
                break;
            case SkipReason::Unsupported:
                ++progress_.unsupported_entries_skipped;
                break;
            case SkipReason::UnsafePath:
                ++progress_.unsafe_paths_skipped;
                break;
        }
    }

    /// Stored stats of a directory, loaded once per directory for probe skipping.
    Result<const std::map<std::string, catalog::Library::KnownStat>*> known_stats(const std::string& directory) {
        if (cached_directory_ != directory || !cached_stats_loaded_) {
            auto stats = library_.known_stats_in_directory(root_id_, directory);
            if (!stats) return std::unexpected(stats.error());
            cached_stats_ = std::move(*stats);
            cached_directory_ = directory;
            cached_stats_loaded_ = true;
        }
        return &cached_stats_;
    }

    /// Stats the pending files, probes the new and changed ones in parallel, and commits the batch
    /// with the finished directories in one catalog transaction.
    Result<void> commit_batch(const std::stop_token& stop) {
        if (pending_files_.empty() && pending_directories_.empty()) return {};

        // Phase 1, on this thread: identities and stored stats. Every catalog read happens before
        // any probe task exists, so an error here can return without waiting for anything.
        std::vector<catalog::ScannedFile> scanned;
        scanned.reserve(pending_files_.size());
        std::vector<std::pair<std::size_t, std::filesystem::path>> to_probe;  // index into scanned, path
        for (const FileFound& found : pending_files_) {
            auto identity = files::detail::read_file_identity(found.absolute_path);
            if (!identity) {
                ++progress_.probe_failures;  // vanished or locked since enumeration: not committed
                continue;
            }
            auto stats = known_stats(found.directory);
            if (!stats) return std::unexpected(stats.error());
            catalog::ScannedFile& file = scanned.emplace_back();
            file.relative_path = found.relative_path;
            file.byte_size = identity->byte_size;
            file.mtime_ns = identity->last_write_time_ns;
            file.volume_id = identity->volume_id;
            if (identity->file_id) file.file_id = std::string(identity->file_id->begin(), identity->file_id->end());
            const auto known = (*stats)->find(path_key(found.relative_path, case_sensitive_));
            const bool unchanged = known != (*stats)->end() && known->second.byte_size == file.byte_size &&
                                   known->second.mtime_ns == file.mtime_ns;
            if (!unchanged) to_probe.emplace_back(scanned.size() - 1, found.absolute_path);  // same stat: no probe
        }

        // Phase 2: bounded parallel probes. Tasks own copies of their inputs and return their
        // results, so nothing they touch can go away while they run.
        std::vector<std::future<ProbeOutcome>> probes;
        probes.reserve(to_probe.size());
        for (const auto& [index, path] : to_probe) {
            probes.push_back(probe_pool_.submit_task(
                [path, relative = scanned[index].relative_path, stop, limits = options_.probe_limits]() noexcept {
                    return probe_task(path, relative, stop, limits);
                }));
        }
        for (std::size_t probe = 0; probe < probes.size(); ++probe) {
            catalog::ScannedFile& file = scanned[to_probe[probe].first];
            try {
                ProbeOutcome outcome = probes[probe].get();
                file.metadata = std::move(outcome.metadata);
                file.probe_error = outcome.error;
            } catch (const std::exception&) {
                file.probe_error = ErrorCode::Internal;  // the task could not run (e.g. broken promise)
            }
        }
        if (stop.stop_requested()) return {};
        for (const auto& file : scanned) {
            if (file.metadata) ++progress_.files_probed;
            if (file.probe_error) ++progress_.probe_failures;
        }

        const auto root = root_path_;
        auto outcome =
            library_.commit_scan_batch(session_, scanned, pending_directories_, [root](std::string_view relative) {
                std::error_code error;
                return std::filesystem::exists(native_path(root / path_from_utf8(relative)), error);
            });
        if (!outcome) return std::unexpected(outcome.error());
        // Enumeration already leaves out paths the catalog cannot store; count any it still refused.
        progress_.unsafe_paths_skipped += outcome->rejected_paths;
        progress_.files_committed += static_cast<std::int64_t>(scanned.size()) - outcome->rejected_paths;
        progress_.added += outcome->added;
        progress_.updated += outcome->updated;
        progress_.relinked += outcome->relinked;
        pending_files_.clear();
        pending_directories_.clear();
        cached_stats_loaded_ = false;  // the batch may have changed stored stats
        return {};
    }

    catalog::Library& library_;
    std::int64_t root_id_;
    bool case_sensitive_;  ///< stored path keys follow the root's case sensitivity
    std::filesystem::path root_path_;
    const catalog::ScanSession& session_;
    const ImportOptions& options_;
    const ImportProgressCallback& callback_;
    BS::thread_pool<> probe_pool_;  ///< its destructor waits for tasks still running
    std::vector<FileFound> pending_files_;
    std::vector<std::string> pending_directories_;
    std::map<std::string, catalog::Library::KnownStat> cached_stats_;
    std::string cached_directory_;
    bool cached_stats_loaded_ = false;
    ImportProgress progress_;
    Clock::time_point last_report_{};
};

/// Runs the committer, turning an exception (out of memory, a throwing catalog call) into an error.
Result<void> run_committer(ImportCommitter& committer, EnumerationQueue& queue, const std::stop_token& stop) {
    try {
        return committer.run(queue, stop);
    } catch (const std::exception& error) {
        return fail(ErrorCode::Internal, std::format("import failed: {}", error.what()));
    }
}

}  // namespace

Result<ImportSummary> import_root(catalog::Library& library, std::int64_t root_id,
                                  const std::filesystem::path& root_path, std::stop_token stop,
                                  const ImportProgressCallback& progress, const ImportOptions& options) {
    std::error_code error;
    if (!std::filesystem::is_directory(native_path(root_path), error)) {
        // The scan fails with SourceOffline either way; failing to record the availability as
        // well must not replace that answer, and the next scan records it again.
        const Result<void> marked = library.set_root_availability(root_id, catalog::Availability::Offline);
        static_cast<void>(marked);
        return fail(ErrorCode::SourceOffline, "the folder is not reachable");
    }
    auto roots = library.list_roots();
    if (!roots) return std::unexpected(roots.error());
    const auto root = std::ranges::find(*roots, root_id, &catalog::RootRecord::id);
    if (root == roots->end()) return fail(ErrorCode::NotFound, "the library has no such root");
    const bool case_sensitive = root->case_sensitive;

    auto session = library.begin_scan(root_id);
    if (!session) return std::unexpected(session.error());
    auto completed = library.completed_directories(*session);
    if (!completed) return std::unexpected(completed.error());

    EnumerationQueue queue(kEnumerationQueueCapacity);
    // Constructed before the enumeration thread exists: if this throws, there is nothing to join.
    ImportCommitter committer(library, root_id, case_sensitive, root_path, *session, options, progress);
    Enumerator enumerator(root_path, std::set<std::string>(completed->begin(), completed->end()),
                          options.follow_reparse_points, queue);
    Result<void> ran;
    {
        // The enumerator watches the jthread's own stop token, which the jthread destructor
        // requests before joining, so leaving this scope on any path (including an exception)
        // releases an enumerator blocked on a full queue.
        std::jthread enumeration_thread([&enumerator](std::stop_token token) { enumerator.run(token); });
        std::stop_callback forward_cancel(stop, [&enumeration_thread] { enumeration_thread.request_stop(); });
        ran = run_committer(committer, queue, stop);
    }

    const bool cancelled = stop.stop_requested();
    auto& final_progress = committer.progress();
    final_progress.finished = true;
    final_progress.cancelled = cancelled;
    if (!ran) {
        // Keep the resume point. The scan's own error is the answer; a failure to close the
        // session is secondary (the next begin_scan resumes or replaces it).
        const auto closed = library.finish_scan(*session, /*cancelled=*/true);
        static_cast<void>(closed);
        const Result<void> reported = committer.report(true);
        static_cast<void>(reported);  // the scan already failed; a throwing callback adds nothing
        return std::unexpected(ran.error());
    }
    auto finished = library.finish_scan(*session, cancelled);
    if (!finished) return std::unexpected(finished.error());
    if (auto reported = committer.report(true); !reported) return std::unexpected(reported.error());
    if (cancelled) {
        return fail(ErrorCode::Cancelled,
                    std::format("import cancelled after {} files", final_progress.files_committed));
    }
    return ImportSummary{final_progress, *finished};
}

}  // namespace mso::files
