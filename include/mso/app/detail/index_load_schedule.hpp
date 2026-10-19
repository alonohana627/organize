#pragma once
/// @file
/// Coalescing of SampleIndex reloads for one library (internal to app::Application).
///
/// A reload of a large library takes seconds. While one runs, the maintenance thread, edits and
/// imports keep asking for the index to catch up, and each of those requests used to queue one
/// more full load: after a big change during analysis, tens of seconds of back-to-back reloads,
/// each doubling the index memory while it ran. The schedule keeps at most one load queued and at
/// most one running per library; requests that arrive while a load runs become exactly one
/// follow-up load, started after the running one ends (theme 2 of the third review: one in flight
/// plus a dirty flag).
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>

namespace mso::app::detail {

/// The reload state of one library's index: idle, queued, running, or running with a follow-up
/// requested. Thread-safe.
class IndexLoadSchedule {
public:
    /// Outcome of request().
    struct Request {
        /// The caller must put the library on the loader's queue (no load of it is queued yet).
        bool enqueue = false;
        /// Number of the load that will cover this request (see wait_for_load()).
        std::uint64_t load_number = 0;
    };

    /// Asks for a load. Idle: the caller enqueues one. Queued: nothing to do, the queued load covers
    /// it. Running: a follow-up load is noted and started when the running one finishes, however
    /// many requests arrive meanwhile.
    Request request();
    /// The loader took the library off its queue and starts the load; returns that load's number.
    std::uint64_t begin_load();
    /// The running load ended (successfully or not). True when requests arrived while it ran: the
    /// schedule is then "queued" again and the caller enqueues the one follow-up load.
    [[nodiscard]] bool finish_load();
    /// True while a load is queued or running (the index then catches up by itself afterwards, so
    /// synchronizing the old state would only report "reload required" again).
    bool load_pending() const;
    /// Number of the load that is running, or else of the one that is queued; nullopt when idle.
    std::optional<std::uint64_t> pending_load() const;
    /// Blocks until load number `load_number` has finished, no load is pending any more, or `stop`
    /// is requested. True when it finished.
    bool wait_for_load(std::uint64_t load_number, std::stop_token stop) const;
    /// Number of loads begun so far.
    std::uint64_t loads_begun() const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable_any changed_;
    bool queued_ = false;
    bool running_ = false;
    bool follow_up_ = false;      ///< requests arrived while running_
    std::uint64_t begun_ = 0;     ///< loads begun (the running one included)
    std::uint64_t finished_ = 0;  ///< loads finished
};

}  // namespace mso::app::detail
