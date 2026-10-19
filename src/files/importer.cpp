#include "mso/files/importer.hpp"

#include <readerwriterqueue/readerwritercircularbuffer.h>
#include <BS_thread_pool.hpp>

#include <chrono>
#include <deque>
#include <format>
#include <future>
#include <set>
#include <thread>
#include <variant>

#include "mso/files/file_identity.hpp"
#include "mso/media/media.hpp"

namespace mso::files {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kEnumerationQueueCapacity = 4096;
constexpr auto kQueueWait = std::chrono::milliseconds(50);
constexpr auto kProgressInterval = std::chrono::milliseconds(100);

/// A candidate audio file found by enumeration.
struct FileFound {
    std::string relative_path;  ///< '/'-separated
    std::string directory;      ///< relative directory, "" for the root
    std::filesystem::path absolute_path;
};

/// All files of a directory have been enumerated (sent after them).
struct DirectoryDone {
    std::string directory;
};

/// A directory could not be listed: its subtree is unknown, not empty.
struct DirectoryGap {
    std::string directory;
    bool denied = false;
};

/// Sentinel: enumeration finished.
struct EnumerationEnd {};

using EnumeratedItem = std::variant<FileFound, DirectoryDone, DirectoryGap, EnumerationEnd>;
using EnumerationQueue = moodycamel::BlockingReaderWriterCircularBuffer<EnumeratedItem>;

/// Converts a std::filesystem relative path into the stored '/'-separated UTF-8 form.
std::string relative_utf8(const std::filesystem::path& relative) {
    return path_to_utf8(relative);
}

/// Walks the root depth-first on its own thread, feeding the bounded queue. Backpressure blocks
/// enumeration instead of accumulating paths in memory.
class Enumerator {
public:
    Enumerator(std::filesystem::path root, std::set<std::string> completed, bool follow_reparse_points,
               EnumerationQueue& queue)
        : root_(std::move(root)),
          completed_(std::move(completed)),
          follow_reparse_points_(follow_reparse_points),
          queue_(queue) {}

    void run(std::stop_token stop) {
        std::deque<std::filesystem::path> pending{std::filesystem::path()};
        while (!pending.empty() && !stop.stop_requested()) {
            const auto directory = pending.front();
            pending.pop_front();
            visit_directory(directory, pending, stop);
        }
        push(EnumerationEnd{}, stop);
    }

private:
    void visit_directory(const std::filesystem::path& relative_directory, std::deque<std::filesystem::path>& pending,
                         std::stop_token stop) {
        const std::string directory_text = relative_utf8(relative_directory);
        const bool already_committed = completed_.contains(directory_text);
        std::error_code error;
        std::filesystem::directory_iterator iterator(root_ / relative_directory, error);
        if (error) {
            push(DirectoryGap{directory_text, error == std::errc::permission_denied}, stop);
            return;
        }
        std::vector<std::filesystem::path> subdirectories;
        for (const auto end = std::filesystem::directory_iterator(); iterator != end; iterator.increment(error)) {
            if (error || stop.stop_requested()) break;
            const auto& entry = *iterator;
            const auto name = entry.path().filename();
            if (!follow_reparse_points_ && is_reparse_point(entry.path())) continue;
            std::error_code status_error;
            if (entry.is_directory(status_error)) {
                subdirectories.push_back(relative_directory / name);
                continue;
            }
            if (already_committed || !entry.is_regular_file(status_error)) continue;
            const std::string extension = to_lower_ascii(path_to_utf8(name.extension()));
            if (extension.size() < 2 || !media::is_candidate_extension(std::string_view(extension).substr(1))) continue;
            push(FileFound{relative_utf8(relative_directory / name), directory_text, entry.path()}, stop);
        }
        if (error && !stop.stop_requested()) {
            // Listing broke off part-way: what was seen is valid, the rest is unknown.
            push(DirectoryGap{directory_text, error == std::errc::permission_denied}, stop);
        } else if (!stop.stop_requested()) {
            push(DirectoryDone{directory_text}, stop);
        }
        std::ranges::sort(subdirectories);
        pending.insert(pending.begin(), subdirectories.begin(), subdirectories.end());
    }

    void push(EnumeratedItem item, const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            if (queue_.wait_enqueue_timed(std::move(item),
                                          std::chrono::duration_cast<std::chrono::microseconds>(kQueueWait).count()))
                return;
        }
    }

    std::filesystem::path root_;
    std::set<std::string> completed_;
    bool follow_reparse_points_;
    EnumerationQueue& queue_;
};

/// Bounded probe of one file plus filename heuristics, converted to catalog observations.
catalog::ObservedMetadata observe(const media::ProbeResult& probe, std::string_view file_name) {
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

    const auto stem = std::filesystem::path(path_from_utf8(file_name)).stem();
    const auto hints = media::filename_hints(path_to_utf8(stem));
    metadata.filename_bpm_milli = hints.bpm_milli;
    if (hints.key) metadata.filename_key = hints.key;
    return metadata;
}

/// Consumes the enumeration queue on the calling thread: stats, probes in parallel, commits.
class ImportCommitter {
public:
    ImportCommitter(catalog::Library& library, std::int64_t root_id, std::filesystem::path root_path,
                    const catalog::ScanSession& session, const ImportOptions& options,
                    const ImportProgressCallback& callback)
        : library_(library),
          root_id_(root_id),
          root_path_(std::move(root_path)),
          session_(session),
          options_(options),
          callback_(callback),
          probe_pool_(std::max<std::size_t>(1, options.probe_threads)) {}

    Result<void> run(EnumerationQueue& queue, std::stop_token stop) {
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
            const bool batch_full = pending_files_.size() >= options_.batch_size;
            const bool batch_due = !pending_files_.empty() && Clock::now() - batch_started >= options_.batch_interval;
            if (batch_full || batch_due || (!pending_directories_.empty() && pending_files_.empty())) {
                if (auto committed = commit_batch(stop); !committed) return committed;
                batch_started = Clock::now();
            }
            report(false);
        }
        if (auto committed = commit_batch(stop); !committed) return committed;
        return {};
    }

    ImportProgress& progress() { return progress_; }

    void report(bool force) {
        if (!callback_) return;
        const auto now = Clock::now();
        if (!force && now - last_report_ < kProgressInterval) return;
        last_report_ = now;
        callback_(progress_);
    }

private:
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
        return {};
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

    Result<void> commit_batch(std::stop_token stop) {
        if (pending_files_.empty() && pending_directories_.empty()) return {};
        // Reserved up front: probe tasks hold references into this vector, so it must not grow.
        std::vector<catalog::ScannedFile> scanned;
        scanned.reserve(pending_files_.size());
        std::vector<std::future<void>> probes;
        for (const auto& found : pending_files_) {
            auto identity = read_file_identity(found.absolute_path);
            if (!identity) {
                ++progress_.probe_failures;  // vanished or locked since enumeration: not committed
                continue;
            }
            auto& file = scanned.emplace_back();
            file.relative_path = found.relative_path;
            file.byte_size = identity->byte_size;
            file.mtime_ns = identity->mtime_ns;
            file.volume_id = identity->volume_id;
            file.file_id = identity->file_id;

            auto stats = known_stats(found.directory);
            if (!stats) return std::unexpected(stats.error());
            const auto known = (*stats)->find(fold_path_key(found.relative_path));
            const bool unchanged = known != (*stats)->end() && known->second.byte_size == file.byte_size &&
                                   known->second.mtime_ns == file.mtime_ns;
            if (unchanged) continue;  // same path, same stat: no probe
            probes.push_back(probe_pool_.submit_task([&file, path = found.absolute_path, &stop] {
                if (stop.stop_requested()) return;
                auto probe = media::probe_file(path);
                if (probe)
                    file.metadata = observe(*probe, file.relative_path);
                else
                    file.probe_error = probe.error().code;
            }));
        }
        for (auto& probe : probes)
            probe.wait();
        if (stop.stop_requested()) return {};
        for (const auto& file : scanned) {
            if (file.metadata) ++progress_.files_probed;
            if (file.probe_error) ++progress_.probe_failures;
        }

        const auto root = root_path_;
        auto outcome =
            library_.commit_scan_batch(session_, scanned, pending_directories_, [root](std::string_view relative) {
                std::error_code error;
                return std::filesystem::exists(root / path_from_utf8(relative), error);
            });
        if (!outcome) return std::unexpected(outcome.error());
        progress_.files_committed += static_cast<std::int64_t>(scanned.size());
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
    std::filesystem::path root_path_;
    const catalog::ScanSession& session_;
    const ImportOptions& options_;
    const ImportProgressCallback& callback_;
    BS::thread_pool<> probe_pool_;
    std::vector<FileFound> pending_files_;
    std::vector<std::string> pending_directories_;
    std::map<std::string, catalog::Library::KnownStat> cached_stats_;
    std::string cached_directory_;
    bool cached_stats_loaded_ = false;
    ImportProgress progress_;
    Clock::time_point last_report_{};
};

}  // namespace

Result<ImportSummary> import_root(catalog::Library& library, std::int64_t root_id,
                                  const std::filesystem::path& root_path, std::stop_token stop,
                                  const ImportProgressCallback& progress, const ImportOptions& options) {
    std::error_code error;
    if (!std::filesystem::is_directory(root_path, error)) {
        (void)library.set_root_availability(root_id, catalog::Availability::Offline);
        return fail(ErrorCode::SourceOffline, "the folder is not reachable");
    }
    auto session = library.begin_scan(root_id);
    if (!session) return std::unexpected(session.error());
    auto completed = library.completed_directories(*session);
    if (!completed) return std::unexpected(completed.error());

    EnumerationQueue queue(kEnumerationQueueCapacity);
    std::stop_source enumeration_stop;
    std::stop_callback forward_cancel(stop, [&] { enumeration_stop.request_stop(); });
    Enumerator enumerator(root_path, std::set<std::string>(completed->begin(), completed->end()),
                          options.follow_reparse_points, queue);
    std::jthread enumeration_thread([&](std::stop_token) { enumerator.run(enumeration_stop.get_token()); });

    ImportCommitter committer(library, root_id, root_path, *session, options, progress);
    auto ran = committer.run(queue, stop);
    enumeration_stop.request_stop();
    enumeration_thread.join();

    const bool cancelled = stop.stop_requested();
    if (!ran) {
        (void)library.finish_scan(*session, /*cancelled=*/true);  // keep resume point
        return std::unexpected(ran.error());
    }
    auto finished = library.finish_scan(*session, cancelled);
    if (!finished) return std::unexpected(finished.error());
    auto& final_progress = committer.progress();
    final_progress.finished = true;
    final_progress.cancelled = cancelled;
    committer.report(true);
    if (cancelled)
        return fail(ErrorCode::Cancelled,
                    std::format("import cancelled after {} files", final_progress.files_committed));
    return ImportSummary{final_progress, *finished};
}

}  // namespace mso::files
