#pragma once
/// @file
/// In-memory index of every live sample of one open library (the owner's responsiveness request:
/// "load the entire tree to memory, update the GUI immediately and SQLite in the background").
///
/// The index holds one search::SampleRow per live sample plus the navigation data needed to
/// evaluate scopes (tag tree, category paths). It is loaded once per open library on a background
/// thread (search::for_each_row) and then kept current incrementally from the catalog outbox
/// (catalog::Library::changes_since + search::load_rows for the changed ids). Folder clicks,
/// sorting, paging and row-evaluable filters are then answered from memory in milliseconds;
/// only full-text terms and collection membership still ask SQLite for a set of ids.
///
/// Ordering is identical to search::SearchService: per sort field, values ascending or descending,
/// nulls always last, ties by sample id in the sort direction; across libraries ties are broken by
/// library UUID. Paging uses an opaque cursor holding each library's structural generation and an
/// offset; it becomes STALE_CURSOR when a library's structure generation moves (rows added or
/// removed, navigation changed, index reloaded), so callers re-query instead of skipping rows.
///
/// Root and folder scopes test the sample's primary location (SampleRow::root_id and the path key
/// of SampleRow::relative_path, exact on case-sensitive roots), exactly like the SQL scope.
///
/// Threading: every method is thread-safe. Readers share a std::shared_mutex; load() and
/// synchronize() are serialized and never hold the exclusive lock while running SQL, so queries
/// keep being answered from the previous state during a reload.
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "mso/catalog/library.hpp"
#include "mso/search/search_service.hpp"

namespace mso::app {

namespace detail {
class IndexedSearch;  ///< query evaluation over several indexes (sample_index.cpp)
}

/// One folder of a root as derived from the indexed rows (navigation tree data).
struct IndexedFolder {
    std::string relative_path;        ///< root-relative, "/"-separated; "" for the root itself
    std::int64_t direct_samples = 0;  ///< live samples directly inside this folder
    std::int64_t total_samples = 0;   ///< live samples in this folder and every subfolder
};

/// Monotonic counters describing how far an index has moved.
struct IndexGenerations {
    std::uint64_t structure = 0;  ///< bumps on load, navigation changes and rows added or removed
    std::uint64_t data = 0;       ///< bumps on every applied change (row values included)
};

/// What one synchronize() applied.
struct IndexChange {
    std::vector<std::int64_t> sample_ids;    ///< rows inserted, updated or removed
    bool rows_added_or_removed = false;      ///< result sets of running queries may have changed
    bool structure_changed = false;          ///< roots, folders, tags, categories or collections changed
    bool reload_required = false;            ///< the outbox cannot describe the changes: call load() again
    std::optional<Error> acknowledge_error;  ///< the outbox cursor could not be stored (retried later)

    /// True when nothing was applied.
    bool empty() const {
        return sample_ids.empty() && !rows_added_or_removed && !structure_changed && !reload_required;
    }
};

class SampleIndex {
public:
    /// Outbox consumer name passed to catalog::Library::acknowledge_changes.
    static constexpr std::string_view kConsumerName = "sample_index";
    /// Outbox envelopes read per synchronize() call (more remaining means a full reload).
    static constexpr std::size_t kChangesPerSynchronization = 4096;
    /// Filtered result lists kept per index for paging (bounded, least recently used dropped).
    static constexpr std::size_t kCachedResults = 4;

    explicit SampleIndex(uuids::uuid library);
    ~SampleIndex();
    SampleIndex(const SampleIndex&) = delete;
    SampleIndex& operator=(const SampleIndex&) = delete;

    /// UUID of the indexed library.
    const uuids::uuid& library() const noexcept { return library_; }

    /// (Re)loads every live row of `library` (blocking; run it on a background thread). Queries
    /// keep using the previous state until the new one is swapped in. CANCELLED when `stop` fires.
    Result<void> load(catalog::Library& library, std::stop_token stop = {});

    /// Applies the changes committed since the last applied revision (IndexChange::reload_required
    /// asks the caller to schedule load() instead). INDEX_UNAVAILABLE before the first load.
    Result<IndexChange> synchronize(catalog::Library& library);

    /// True once a load() has completed.
    bool ready() const noexcept { return ready_.load(std::memory_order_acquire); }
    /// Catalog revision the index reflects.
    std::int64_t revision() const noexcept { return revision_.load(std::memory_order_acquire); }
    /// Current generation counters.
    IndexGenerations generations() const noexcept;
    /// Number of live rows.
    std::size_t size() const;

    /// Copy of one row; nullopt when the sample is not live (or not loaded yet).
    std::optional<search::SampleRow> row(std::int64_t sample_id) const;
    /// Copies of the rows that exist, in the order of `sample_ids` (missing ids are skipped).
    std::vector<search::SampleRow> rows(std::span<const std::int64_t> sample_ids) const;
    /// Every folder of a root that holds live samples (with its ancestors), sorted by path.
    std::vector<IndexedFolder> folders(std::int64_t root_id) const;

    /// Drops the cached filtered results and SQL id sets (sort orders stay). Benchmarks use it to
    /// measure a fresh query instead of a cache hit; nothing else needs it.
    void drop_query_caches() const;

    /// Whether a request can be answered from indexes. Relevance ordering of text terms needs
    /// SQLite's BM25 ranking, so those requests go to search::SearchService.
    static bool serves(const search::SearchRequest& request);

private:
    friend class detail::IndexedSearch;
    struct State;

    uuids::uuid library_;
    std::unique_ptr<State> state_;
    std::atomic<bool> ready_{false};
    std::atomic<std::int64_t> revision_{0};
};

/// One library taking part in an indexed search.
struct IndexedLibrary {
    const SampleIndex* index = nullptr;
    catalog::Library* library = nullptr;  ///< asked for full-text and collection id sets
    std::string name;                     ///< for coverage reports
};

/// Federated, paged search over loaded indexes with SearchService's ordering and coverage
/// semantics. Every index must be ready() and the request must satisfy SampleIndex::serves().
Result<search::SearchPage> search_indexes(std::span<const IndexedLibrary> libraries,
                                          const search::SearchRequest& request);

/// Exact count over loaded indexes (same filters as search_indexes).
Result<search::CountResult> count_indexes(std::span<const IndexedLibrary> libraries,
                                          const search::SearchRequest& request);

}  // namespace mso::app
