#pragma once
/// @file
/// In-memory index of every live sample of one open library (the owner's responsiveness request:
/// "load the entire tree to memory, update the GUI immediately and SQLite in the background").
///
/// The index holds one compact row per live sample plus the navigation data needed to evaluate
/// scopes (tag tree, category tree, merged-tag aliases). Text that many rows share (root names and
/// identities, folders and their path keys, formats, codecs, value sources, category paths and tag
/// sets) is stored once and referenced by number; a full search::SampleRow is assembled only for
/// rows that are returned. The index is loaded once per open library on a background thread
/// (search::for_each_row) and then kept current incrementally from the catalog outbox
/// (catalog::Library::changes_since + search::load_rows for the changed ids). Folder clicks,
/// sorting, paging and row-evaluable filters are then answered from memory in milliseconds; only
/// full-text terms and collection membership still ask SQLite for a set of ids.
///
/// Memory is bounded: load() refuses a library with more than its row limit (RESOURCE_LIMIT, checked
/// with one count before anything is streamed), and the owner then answers that library from SQLite
/// (app::Application does this automatically). The id-indexed tables are bounded by the same limit:
/// a library whose sample ids reach past four times the row limit (at least 2^20) is refused too.
/// Each row's three texts (display name, name key, file name) share one heap block.
///
/// Ordering is identical to search::SearchService: per sort field, values ascending or descending,
/// nulls always last, ties by sample id in the sort direction; across libraries ties are broken by
/// library UUID. Paging uses keyset cursors: a cursor records, per library, the sort key and sample
/// id of the last row it returned, and the next page starts at the first row after that key in the
/// current order. A row that was not edited is therefore never skipped or repeated, whatever values
/// change between two pages (ratings, analysis results); a row whose own sort value or filter
/// membership changes across the cursor appears where its new value puts it (possibly again, or not
/// at all in this pass), exactly like SearchService's keyset cursors, and the change event lists it
/// so a view can refresh it in place. A cursor becomes STALE_CURSOR when a library's structure
/// generation moves (rows added or removed, navigation changed, index reloaded), or when a library
/// that was missing from the earlier pages answers again (or the other way round), so a view
/// re-queries at its anchor and picks up inserted rows.
///
/// Sort orders are built per field on first use and then patched in place: a changed row whose
/// sort value did not move keeps its place, the others are erased and inserted by binary search.
///
/// Root and folder scopes test the sample's primary location (its root and the path key of its
/// folder, exact on case-sensitive roots), exactly like the SQL scope.
///
/// Threading: every method is thread-safe. Readers share a std::shared_mutex. synchronize() and the
/// final step of load() are serialized; neither holds the exclusive lock while running SQL, and a
/// load streams the library without blocking synchronize(), so queries and edits keep being served
/// from the previous state while a reload runs. The previous rows are freed after every lock is
/// released.
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
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

/// The in-memory index of one library: compact rows, cached sort orders and filtered results, kept
/// current from the catalog outbox. See the file comment for ordering, cursors and threading.
class SampleIndex {
public:
    /// Outbox consumer name passed to catalog::Library::acknowledge_changes.
    static constexpr std::string_view kConsumerName = "sample_index";
    /// Outbox envelopes read per synchronize() call (more remaining means a full reload).
    static constexpr std::size_t kChangesPerSynchronization = 4096;
    /// Filtered result lists kept per index for paging (bounded, least recently used dropped).
    static constexpr std::size_t kCachedResults = 4;
    /// Default row limit of load(): about 300 MiB of index (`mso bench` with realistic 46-byte file
    /// names measures about 320 bytes per row with every sort order built; see
    /// IMPLEMENTATION_STATUS.md), so even a reload, which briefly holds two copies, stays inside
    /// docs/06's 768 MiB browse budget with room for the UI and SQLite.
    static constexpr std::size_t kDefaultRowLimit = 1'000'000;

    /// An empty, not yet ready index of `library`.
    explicit SampleIndex(uuids::uuid library);
    ~SampleIndex();
    SampleIndex(const SampleIndex&) = delete;
    SampleIndex& operator=(const SampleIndex&) = delete;

    /// UUID of the indexed library.
    const uuids::uuid& library() const noexcept { return library_; }

    /// (Re)loads every live row of `library` (blocking; run it on a background thread). Queries and
    /// synchronize() keep using the previous state until the new one, caught up with everything
    /// committed meanwhile, is swapped in. CANCELLED when `stop` fires; RESOURCE_LIMIT (the index
    /// keeps its previous state) when the library has more than `row_limit` live samples; DB_BUSY
    /// when another load of this index is running.
    [[nodiscard]] Result<void> load(catalog::Library& library, std::stop_token stop = {},
                                    std::size_t row_limit = kDefaultRowLimit);

    /// Applies the changes committed since the last applied revision (IndexChange::reload_required
    /// asks the caller to schedule load() instead). INDEX_UNAVAILABLE before the first load.
    [[nodiscard]] Result<IndexChange> synchronize(catalog::Library& library);

    /// Frees every row and becomes not ready (the owner answers from SQLite from now on, until the
    /// next successful load()).
    void clear();

    /// True once a load() has completed (and clear() was not called since).
    bool ready() const noexcept { return ready_.load(std::memory_order_acquire); }
    /// Catalog revision the index reflects.
    std::int64_t revision() const noexcept { return revision_.load(std::memory_order_acquire); }
    /// Current generation counters.
    IndexGenerations generations() const noexcept;
    /// Number of live rows.
    std::size_t size() const;

    /// The row of one sample; nullopt when the sample is not live (or not loaded yet).
    std::optional<search::SampleRow> row(std::int64_t sample_id) const;
    /// The rows that exist, in the order of `sample_ids` (missing ids are skipped).
    std::vector<search::SampleRow> rows(std::span<const std::int64_t> sample_ids) const;
    /// Every folder of a root that holds live samples (with its ancestors), sorted by path.
    std::vector<IndexedFolder> folders(std::int64_t root_id) const;

    /// Drops the cached filtered results and SQL id sets (sort orders stay). Benchmarks use it to
    /// measure a fresh query instead of a cache hit; nothing else needs it.
    void drop_query_caches() const;

    /// Whether a request can be answered from indexes. Relevance ordering of text terms needs
    /// SQLite's BM25 ranking, so those requests go to search::SearchService.
    static bool serves(const search::SearchRequest& request);
    /// The same for a request whose text is already parsed into `query`.
    static bool serves(const search::Query& query, search::SortField sort);

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
/// semantics. The request must satisfy SampleIndex::serves(). INDEX_UNAVAILABLE when an index is
/// not ready (the caller answers from SQLite instead).
[[nodiscard]] Result<search::SearchPage> search_indexes(std::span<const IndexedLibrary> libraries,
                                                        const search::SearchRequest& request);
/// The same with the request's text already parsed: `query` must be search::parse_query(request.text).
[[nodiscard]] Result<search::SearchPage> search_indexes(std::span<const IndexedLibrary> libraries,
                                                        const search::SearchRequest& request,
                                                        const search::Query& query);

/// Exact count over loaded indexes (same filters and errors as search_indexes).
[[nodiscard]] Result<search::CountResult> count_indexes(std::span<const IndexedLibrary> libraries,
                                                        const search::SearchRequest& request);
/// The same with the request's text already parsed into `query`.
[[nodiscard]] Result<search::CountResult> count_indexes(std::span<const IndexedLibrary> libraries,
                                                        const search::SearchRequest& request,
                                                        const search::Query& query);

}  // namespace mso::app
