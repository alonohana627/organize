#include "mso/app/detail/index_maintainer.hpp"

#include <algorithm>
#include <format>

namespace mso::app::detail {

namespace {

/// Catch-up interval of the maintenance thread when nobody asked for an update.
constexpr auto kIndexPollInterval = std::chrono::seconds(1);
/// Minimum spacing of background index updates of one library: requests in between (one per
/// finished analysis job, one per committed import batch) are coalesced into the next update.
/// Edits through Application patch the index at once instead.
constexpr auto kBackgroundUpdateInterval = std::chrono::milliseconds(100);
/// How often a library answered from SQLite because it did not fit the index budget is checked
/// again (the budget also frees up at once when another library closes).
constexpr auto kBudgetRecheckInterval = std::chrono::seconds(30);
/// A library is indexed again only when it fits the budget with this much room (percent), so a
/// library right at the limit does not flip between memory and SQLite.
constexpr std::size_t kBudgetRecheckHeadroomPercent = 90;
/// Reloads an edit waits for before returning (a reload that keeps failing is reported, not awaited).
constexpr int kMaximumAwaitedLoads = 3;
/// Above this many changed rows an event asks for a full reload instead of listing them.
constexpr std::size_t kMaximumEventSampleIds = 4096;
/// Runs listed in an AnalysisProgressEvent.
constexpr std::size_t kRunsPerProgressEvent = 20;

}  // namespace

Error internal_error(std::string_view where, const std::exception& exception) {
    return Error{ErrorCode::Internal, true, std::format("{}: {}", where, exception.what()), std::nullopt, std::nullopt};
}

IndexMaintainer::IndexMaintainer(Services services, std::size_t index_row_limit)
    : services_(std::move(services)), index_row_limit_(index_row_limit) {}

IndexMaintainer::~IndexMaintainer() {
    stop();
}

void IndexMaintainer::start() {
    loader_ = std::jthread([this](std::stop_token stop) { load(stop); });
    maintainer_ = std::jthread([this](std::stop_token stop) { maintain(stop); });
}

void IndexMaintainer::stop() {
    loader_ = {};  // requests stop and joins
    maintainer_ = {};
}

// ---------------------------------------------------------------- requests

void IndexMaintainer::wake_maintainer() {
    {
        std::lock_guard lock(maintenance_mutex_);
        maintenance_requested_ = true;
    }
    maintenance_wake_.notify_all();
}

void IndexMaintainer::request_update(LibraryEntry& entry) {
    entry.update_requested.store(true, std::memory_order_release);
    wake_maintainer();
}

void IndexMaintainer::request_budget_recheck() {
    budget_recheck_requested_.store(true, std::memory_order_release);
    wake_maintainer();
}

void IndexMaintainer::queue_load(const std::shared_ptr<LibraryEntry>& entry) {
    if (entry->load_schedule.request().enqueue) enqueue_load(entry);
}

void IndexMaintainer::enqueue_load(const std::shared_ptr<LibraryEntry>& entry) {
    {
        std::lock_guard lock(loads_mutex_);
        load_queue_.push_back(entry);
    }
    loads_wake_.notify_all();
}

// ---------------------------------------------------------------- announcements

void IndexMaintainer::note_catalog_change(LibraryEntry& entry, const std::vector<std::int64_t>& sample_ids,
                                          bool structure_changed, bool full_reload) {
    {
        std::lock_guard lock(entry.announcement_mutex);
        auto& pending = entry.pending;
        pending.catalog_pending = true;
        pending.structure_changed = pending.structure_changed || structure_changed;
        pending.full_reload = pending.full_reload || full_reload;
        if (!pending.full_reload) {
            pending.sample_ids.insert(pending.sample_ids.end(), sample_ids.begin(), sample_ids.end());
            if (pending.sample_ids.size() > kMaximumEventSampleIds) pending.full_reload = true;
        }
        if (pending.full_reload) pending.sample_ids.clear();
    }
    wake_maintainer();
}

void IndexMaintainer::note_analysis_status(LibraryEntry& entry, std::string status) {
    {
        std::lock_guard lock(entry.announcement_mutex);
        entry.pending.analysis_status = std::move(status);
    }
    wake_maintainer();
}

void IndexMaintainer::report(std::optional<uuids::uuid> library, std::string context, Error error) {
    services_.emit(BackgroundErrorEvent{library, std::move(context), std::move(error)});
}

void IndexMaintainer::report_once(LibraryEntry& entry, std::string context, const Error& error) {
    {
        std::lock_guard lock(entry.announcement_mutex);
        const auto message = context + ": " + error.message();
        if (entry.pending.last_reported_error == message) return;
        entry.pending.last_reported_error = message;
    }
    report(entry.uuid, std::move(context), error);
}

std::optional<IndexMaintainer::Clock::time_point> IndexMaintainer::announce(LibraryEntry& entry,
                                                                            Clock::time_point now) {
    std::optional<CatalogChangedEvent> catalog_event;
    std::optional<std::string> analysis_status;
    std::optional<Clock::time_point> next_due;
    {
        std::lock_guard lock(entry.announcement_mutex);
        auto& pending = entry.pending;
        if (pending.catalog_pending) {
            if (now - pending.last_catalog_event >= Application::kEventInterval) {
                catalog_event = CatalogChangedEvent{entry.uuid, std::move(pending.sample_ids),
                                                    pending.structure_changed, pending.full_reload};
                pending.sample_ids = {};
                pending.structure_changed = pending.full_reload = pending.catalog_pending = false;
                pending.last_catalog_event = now;
            } else {
                next_due = pending.last_catalog_event + Application::kEventInterval;
            }
        }
        if (pending.analysis_status) {
            if (now - pending.last_analysis_event >= Application::kEventInterval) {
                analysis_status = std::move(pending.analysis_status);
                pending.analysis_status.reset();
                pending.last_analysis_event = now;
            } else {
                const auto due = pending.last_analysis_event + Application::kEventInterval;
                next_due = next_due ? std::min(*next_due, due) : due;
            }
        }
    }
    if (catalog_event) {
        std::ranges::sort(catalog_event->sample_ids);
        catalog_event->sample_ids.erase(std::ranges::unique(catalog_event->sample_ids).begin(),
                                        catalog_event->sample_ids.end());
        services_.emit(*catalog_event);
    }
    if (analysis_status) {
        AnalysisProgressEvent event{entry.uuid, {}, std::move(*analysis_status)};
        if (auto runs = entry.library->list_analysis_runs(kRunsPerProgressEvent))
            event.runs = std::move(*runs);
        else
            report_once(entry, "listing analysis runs", runs.error());
        services_.emit(event);
    }
    return next_due;
}

// ---------------------------------------------------------------- budget

std::size_t IndexMaintainer::indexed_rows_except(const LibraryEntry& excluded) const {
    std::size_t rows = 0;
    for (const auto& entry : services_.libraries())
        if (entry.get() != &excluded && entry->index_usable()) rows += entry->index->size();
    return rows;
}

std::size_t IndexMaintainer::index_allowance(const LibraryEntry& entry) const {
    const std::size_t others = indexed_rows_except(entry);
    return index_row_limit_ > others ? index_row_limit_ - others : 0;
}

void IndexMaintainer::disable_index(LibraryEntry& entry, const Error& reason) {
    entry.index_disabled.store(true, std::memory_order_release);
    entry.index->clear();
    report_once(entry, "keeping the library in memory (searching it in the database instead)", reason);
    note_catalog_change(entry, {}, true, true);
}

void IndexMaintainer::recheck_index_budget() {
    for (const auto& entry : services_.libraries()) {
        if (!entry->index || !entry->index_disabled.load(std::memory_order_acquire)) continue;
        auto samples = entry->library->sample_count();
        if (!samples) {
            report_once(*entry, "checking whether the library fits in memory again", samples.error());
            continue;
        }
        const std::size_t allowance = index_allowance(*entry);
        if (*samples < 0 || static_cast<std::size_t>(*samples) > allowance / 100 * kBudgetRecheckHeadroomPercent)
            continue;
        entry->index_disabled.store(false, std::memory_order_release);
        queue_load(entry);  // views switch back when the load is announced
    }
}

// ---------------------------------------------------------------- updates and loads

void IndexMaintainer::update_index(const std::shared_ptr<LibraryEntry>& entry) {
    if (!entry->index || entry->index_disabled.load(std::memory_order_acquire)) {
        // Without an index nothing knows which rows changed: ask views to re-query.
        note_catalog_change(*entry, {}, true, true);
        return;
    }
    // While a load is queued or running the loader catches up after it; synchronizing the old
    // state now would only ask for yet another reload.
    if (!entry->index->ready() || entry->load_schedule.load_pending()) return;
    auto change = entry->index->synchronize(*entry->library);
    if (!change) {
        report_once(*entry, "updating the sample index", change.error());
        return;
    }
    if (change->acknowledge_error)
        report_once(*entry, "recording the sample index position", *change->acknowledge_error);
    if (change->reload_required) {
        queue_load(entry);
        return;
    }
    if (change->rows_added_or_removed && entry->index->size() > index_allowance(*entry)) {
        disable_index(
            *entry,
            Error{ErrorCode::ResourceLimit, false, "the open libraries grew past the in-memory index limit", {}, {}});
        return;
    }
    if (!change->empty())
        note_catalog_change(*entry, change->sample_ids, change->structure_changed, change->rows_added_or_removed);
}

void IndexMaintainer::bring_index_to(const std::shared_ptr<LibraryEntry>& entry, std::int64_t committed_revision) {
    if (std::this_thread::get_id() == loader_.get_id()) return;  // it would wait for itself
    for (int awaited = 0; awaited <= kMaximumAwaitedLoads; ++awaited) {
        if (!entry->index_usable() || entry->index->revision() >= committed_revision) return;
        const auto pending = entry->load_schedule.pending_load();
        if (!pending) {
            update_index(entry);  // after a load: catch up, or queue the reload it still needs
            if (!entry->load_schedule.load_pending()) return;  // caught up, or a reported failure
            continue;
        }
        if (awaited == kMaximumAwaitedLoads) return;
        if (!entry->load_schedule.wait_for_load(*pending, entry->closing.get_token())) return;
    }
}

void IndexMaintainer::load_index(const std::shared_ptr<LibraryEntry>& entry) {
    if (!entry->index || entry->index_disabled.load(std::memory_order_acquire)) return;
    auto loaded = entry->index->load(*entry->library, entry->closing.get_token(), index_allowance(*entry));
    if (!loaded) {
        if (loaded.error().code == ErrorCode::ResourceLimit)
            disable_index(*entry, loaded.error());
        else if (loaded.error().code != ErrorCode::Cancelled)
            report(entry->uuid, "loading the sample index", loaded.error());
        return;
    }
    if (entry->index_disabled.load(std::memory_order_acquire)) {
        entry->index->clear();  // disabled while it loaded: SQLite answers, free the rows
        return;
    }
    note_catalog_change(*entry, {}, true, true);
}

void IndexMaintainer::load(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::shared_ptr<LibraryEntry> entry;
        {
            std::unique_lock lock(loads_mutex_);
            loads_wake_.wait(lock, stop, [&] { return !load_queue_.empty(); });
            if (stop.stop_requested()) return;
            entry = load_queue_.front().lock();
            load_queue_.pop_front();
        }
        if (!entry) continue;  // closed and gone meanwhile
        entry->load_schedule.begin_load();
        // Exception barrier: a throw is reported and the loader keeps serving.
        try {
            load_index(entry);
        } catch (const std::exception& exception) {
            report(entry->uuid, "loading the sample index", internal_error("index loader", exception));
        }
        // Requests that arrived while it ran become one follow-up load; otherwise catch up with
        // what was committed during the load (a no-op when the load failed or was skipped).
        try {
            if (entry->load_schedule.finish_load())
                enqueue_load(entry);
            else
                update_index(entry);
        } catch (const std::exception& exception) {
            report(entry->uuid, "updating the sample index", internal_error("index loader", exception));
        }
    }
}

void IndexMaintainer::maintain(std::stop_token stop) {
    auto next_poll = Clock::now() + kIndexPollInterval;
    auto next_budget_check = Clock::now() + kBudgetRecheckInterval;
    while (!stop.stop_requested()) {
        std::optional<Clock::time_point> next_due;
        auto due_at = [&](Clock::time_point due) { next_due = next_due ? std::min(*next_due, due) : due; };
        // Exception barrier per pass: report and keep maintaining.
        try {
            const auto now = Clock::now();
            const bool poll = now >= next_poll;
            if (poll) next_poll = now + kIndexPollInterval;
            if (budget_recheck_requested_.exchange(false, std::memory_order_acq_rel) || now >= next_budget_check) {
                next_budget_check = now + kBudgetRecheckInterval;
                recheck_index_budget();
            }
            for (const auto& entry : services_.libraries()) {
                const bool wanted = entry->update_requested.load(std::memory_order_acquire) ||
                                    (poll && entry->index && entry->index->ready());
                if (wanted) {
                    const auto allowed_at = entry->last_background_update + kBackgroundUpdateInterval;
                    if (now >= allowed_at) {
                        // Cleared before the update reads the outbox: a request arriving during the
                        // update is served by the next one, never lost.
                        entry->update_requested.store(false, std::memory_order_release);
                        entry->last_background_update = now;
                        update_index(entry);
                    } else {
                        due_at(allowed_at);
                    }
                }
                if (auto due = announce(*entry, Clock::now()); due) due_at(*due);
            }
        } catch (const std::exception& exception) {
            report(std::nullopt, "maintaining the sample indexes", internal_error("index maintenance", exception));
        }
        const auto wake_at = next_due ? std::min(*next_due, next_poll) : next_poll;
        std::unique_lock lock(maintenance_mutex_);
        maintenance_wake_.wait_until(lock, stop, wake_at, [&] { return maintenance_requested_; });
        maintenance_requested_ = false;
    }
}

}  // namespace mso::app::detail
