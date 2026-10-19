#pragma once
/// @file
/// Index maintenance of app::Application (internal to the app module): keeps the in-memory index of
/// every open library current and announces catalog changes.
///
/// Two threads:
/// - the maintenance thread applies the catalog outbox to each index (requests of one library are
///   coalesced to one update per 100 ms, plus a poll every second), re-checks libraries that did
///   not fit the index budget, and emits the coalesced CatalogChangedEvent and
///   AnalysisProgressEvent of each library;
/// - the loader thread runs full loads, coalesced per library by IndexLoadSchedule. While a load of
///   a library is queued or running, its index is not synchronized: the old state cannot describe
///   what made the reload necessary, and the loader catches up after the load.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mso/app/application.hpp"
#include "mso/app/detail/library_entry.hpp"

namespace mso::app::detail {

/// An INTERNAL error for an exception caught by a thread barrier.
Error internal_error(std::string_view where, const std::exception& exception);

/// Keeps the indexes of the open libraries current and announces their changes (see the file
/// comment). Thread-safe; every method may be called from any thread.
class IndexMaintainer {
public:
    /// What the maintainer needs from its Application.
    struct Services {
        /// The open libraries right now.
        std::function<std::vector<std::shared_ptr<LibraryEntry>>()> libraries;
        /// Delivers an event to the application's handler.
        std::function<void(const ApplicationEvent&)> emit;
    };

    /// A maintainer whose indexes together hold at most `index_row_limit` rows. Call start().
    IndexMaintainer(Services services, std::size_t index_row_limit);
    /// Stops both threads (see stop()).
    ~IndexMaintainer();
    IndexMaintainer(const IndexMaintainer&) = delete;
    IndexMaintainer& operator=(const IndexMaintainer&) = delete;

    /// Starts the maintenance and loader threads.
    void start();
    /// Stops and joins both threads. A running load is cancelled through its library's
    /// LibraryEntry::closing, which the caller requests first.
    void stop();

    /// Asks the maintenance thread to catch up with `entry`'s outbox soon (coalesced).
    void request_update(LibraryEntry& entry);
    /// Asks for a full load of `entry`'s index (coalesced per library).
    void queue_load(const std::shared_ptr<LibraryEntry>& entry);
    /// Brings `entry`'s index up to date with its outbox now, on the calling thread.
    void update_index(const std::shared_ptr<LibraryEntry>& entry);
    /// Waits until `entry`'s index reflects `committed_revision` (an edit just committed), waiting
    /// for the reload an edit too large for the outbox causes. Returns at once when SQLite answers
    /// the library anyway, when the library closes, or on the loader thread.
    void bring_index_to(const std::shared_ptr<LibraryEntry>& entry, std::int64_t committed_revision);
    /// A library closed, so its rows left the budget: libraries answered from SQLite are checked
    /// again on the maintenance thread.
    void request_budget_recheck();

    /// Marks changes for the next coalesced CatalogChangedEvent of `entry`.
    void note_catalog_change(LibraryEntry& entry, const std::vector<std::int64_t>& sample_ids, bool structure_changed,
                             bool full_reload);
    /// Records the worker status for the next coalesced AnalysisProgressEvent of `entry`.
    void note_analysis_status(LibraryEntry& entry, std::string status);
    /// Reports a background error of `entry` once per distinct message.
    void report_once(LibraryEntry& entry, std::string context, const Error& error);

private:
    using Clock = std::chrono::steady_clock;

    void wake_maintainer();
    void enqueue_load(const std::shared_ptr<LibraryEntry>& entry);
    void report(std::optional<uuids::uuid> library, std::string context, Error error);
    /// Rows the indexes of the libraries other than `excluded` hold.
    std::size_t indexed_rows_except(const LibraryEntry& excluded) const;
    /// The share of the row budget left for `entry`.
    std::size_t index_allowance(const LibraryEntry& entry) const;
    /// Answers `entry` from SQLite from now on: frees its index and tells views to re-query.
    void disable_index(LibraryEntry& entry, const Error& reason);
    /// Indexes again the libraries answered from SQLite that fit the budget now.
    void recheck_index_budget();
    /// Emits the coalesced events of `entry` that are due; returns when the next one is due.
    std::optional<Clock::time_point> announce(LibraryEntry& entry, Clock::time_point now);
    /// One full load of `entry`'s index (loader thread).
    void load_index(const std::shared_ptr<LibraryEntry>& entry);
    void maintain(std::stop_token stop);
    void load(std::stop_token stop);

    Services services_;
    std::size_t index_row_limit_;

    std::mutex maintenance_mutex_;
    std::condition_variable_any maintenance_wake_;
    bool maintenance_requested_ = false;
    std::atomic<bool> budget_recheck_requested_{false};

    std::mutex loads_mutex_;
    std::condition_variable_any loads_wake_;
    std::deque<std::weak_ptr<LibraryEntry>> load_queue_;  ///< at most one entry per library (IndexLoadSchedule)

    // Threads last: they start after everything above exists.
    std::jthread loader_;
    std::jthread maintainer_;
};

}  // namespace mso::app::detail
