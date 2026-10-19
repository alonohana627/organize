#pragma once
/// @file
/// One library open in an app::Application (internal to the app module): its lock, catalog, index,
/// reload schedule and the coalescing state of its announcements.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "mso/app/detail/index_load_schedule.hpp"
#include "mso/app/file_lock.hpp"
#include "mso/app/sample_index.hpp"
#include "mso/catalog/library.hpp"

namespace mso::app::detail {

/// One open library. Shared by everything that works on it, so close_library() never destroys it
/// under running work; the last holder closes the catalog, then releases the lock.
struct LibraryEntry {
    using Clock = std::chrono::steady_clock;

    /// Changes of this library waiting to be announced (coalescing state).
    struct PendingAnnouncement {
        std::vector<std::int64_t> sample_ids;
        bool structure_changed = false;
        bool full_reload = false;
        bool catalog_pending = false;
        std::optional<std::string> analysis_status;
        Clock::time_point last_catalog_event{};
        Clock::time_point last_analysis_event{};
        std::string last_reported_error;  ///< repeated identical background errors are reported once
    };

    FileLock lock;  ///< first member: released last, after the catalog is closed
    std::unique_ptr<catalog::Library> library;
    std::unique_ptr<SampleIndex> index;  ///< null when ApplicationOptions::in_memory_index is off
    /// The library did not fit the index budget: it is answered from SQLite (index stays cleared).
    std::atomic<bool> index_disabled{false};
    uuids::uuid uuid;
    std::string name;  ///< cached: identity() takes the writer mutex, which imports hold for long
    bool in_use_elsewhere = false;
    std::stop_source closing;  ///< cancels an index load (and waits for one) when the library closes
    std::atomic<bool> update_requested{false};
    /// When the maintenance thread last brought the index up to date (maintenance thread only).
    Clock::time_point last_background_update{};
    /// At most one reload queued and one running; requests during a load become one follow-up.
    IndexLoadSchedule load_schedule;
    std::mutex announcement_mutex;
    PendingAnnouncement pending;  ///< guarded by announcement_mutex

    /// True when searches and rows of this library can be answered from its index right now.
    bool index_usable() const { return index && !index_disabled.load(std::memory_order_acquire) && index->ready(); }

    LibraryEntry() = default;
    LibraryEntry(const LibraryEntry&) = delete;
    LibraryEntry& operator=(const LibraryEntry&) = delete;
    ~LibraryEntry() {
        index.reset();
        library.reset();  // close the catalog before the lock is released
    }
};

}  // namespace mso::app::detail
