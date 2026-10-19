#pragma once
/// @file
/// SampleTableModel: a bounded, paged view of a search result of any size.
///
/// Memory and threading contract (docs/02_ARCHITECTURE.md §3, docs/03_DATA_AND_SEARCH.md §7):
///  - pages of at most kPageSize rows are fetched asynchronously through a SamplePageSource
///    (the Application's in-memory index, so a page arrives in milliseconds); rowCount() grows as
///    the view scrolls (canFetchMore/fetchMore);
///  - at most kMaxHydratedPages pages (2,000 rows) keep their row data. The least recently used
///    page beyond that keeps only its start cursor; its rows show "Loading…" and are re-read when
///    they become visible again (with exponential back-off after a failure);
///  - every result is tagged with the query generation it was requested for; results of an
///    obsolete generation are discarded. A new query keeps the previous rows on screen until its
///    first page replaces them in one model reset (no flash of empty rows); if that first page
///    fails, the rows are cleared and queryFailed() says why;
///  - sorting re-queries the whole result with the server-side sort;
///  - edits are optimistic: patch_rows()/remove_rows() change the visible copies immediately and
///    begin_pending_edit()/end_pending_edit() keep in-place refreshes from overwriting them until
///    the background write has been reconciled.
///
/// Catalog changes refresh only the affected rows (refresh_rows). Only a full reload re-reads the
/// result from its start up to the anchor row (refresh), capped at 2,000 rows: a view scrolled
/// deeper returns to that window.
#include <QAbstractTableModel>
#include <QDeadlineTimer>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "mso/search/search_service.hpp"
#include "mso/ui/navigation_scope.hpp"
#include "mso/ui/sample_key.hpp"
#include "mso/ui/sample_page_source.hpp"

namespace mso::ui {

/// Paged Qt model over SamplePageSource results. Lives on, and is used only from, the UI thread.
class SampleTableModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    /// Table columns, in default display order.
    enum class Column {
        Name,
        Format,
        Duration,
        SampleRate,
        Channels,
        BitDepth,
        Size,
        Bpm,
        Key,
        Category,
        Tags,
        Rating,
        Favorite,
        Location,
        Availability,
        Analyzed,
        Added,
    };
    /// Number of columns in Column.
    static constexpr int kColumnCount = static_cast<int>(Column::Added) + 1;

    /// Extra item data roles.
    enum Role {
        /// True while the row's page is being (re-)fetched.
        LoadingRole = Qt::UserRole + 1,
        /// int value of search::RowAvailability.
        AvailabilityRole,
        /// int rating 0..5 (the rating column paints and edits stars from it).
        RatingRole,
    };

    /// Rows per page request.
    static constexpr int kPageSize = 200;
    /// Pages whose row data stays in memory (2,000 rows).
    static constexpr int kMaxHydratedPages = 10;
    /// First and last delay before re-reading a page whose last read failed.
    static constexpr int kMinimumRetryMilliseconds = 1'000;
    static constexpr int kMaximumRetryMilliseconds = 30'000;

    /// A model fed by `source`, which must outlive it.
    explicit SampleTableModel(SamplePageSource& source, QObject* parent = nullptr);

    // ------------------------------------------------------------ QAbstractItemModel
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    Qt::DropActions supportedDragActions() const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;
    /// Re-queries with the server-side sort of `column`; non-sortable columns emit sortRejected().
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    // ------------------------------------------------------------ query
    /// Replaces the query text and navigation scope and starts a new result generation. Does
    /// nothing when both equal the current query, so one navigator click issues one query.
    void set_query(const std::string& text, const NavigationScope& scope);
    /// Like set_query() but always starts a new generation (e.g. a library was opened or closed,
    /// so the same query now covers different data).
    void run_query(const std::string& text, const NavigationScope& scope);
    /// Clears the result (no library open). No requests are made until set_query().
    void clear();
    /// Re-reads the result after a full catalog reload. Rows stay visible until the new pages
    /// arrive; `anchor_row` is the first row the view shows, and rows up to it are restored.
    void refresh(int anchor_row);
    /// Re-reads the given samples in place (catalog change events); samples that no longer exist
    /// are removed. Keys that are not loaded or have a pending optimistic edit are skipped.
    void refresh_rows(const std::vector<SampleKey>& keys);

    /// Current query text.
    const std::string& query_text() const { return text_; }
    /// Current navigation scope.
    const NavigationScope& scope() const { return scope_; }
    /// Current server-side sort.
    search::SortField sort_field() const { return sort_field_; }
    bool sort_descending() const { return sort_descending_; }
    /// Column showing the current sort, for the header's sort indicator.
    int sort_column() const;
    /// Server-side sort of a column, or nullopt for columns the search service cannot sort.
    static std::optional<search::SortField> sort_field_for(Column column);

    // ------------------------------------------------------------ optimistic edits
    /// Applies `patch` to the loaded rows of `keys` and repaints them. Returns copies of the rows
    /// as they were before the patch.
    std::vector<search::SampleRow> patch_rows(const std::vector<SampleKey>& keys,
                                              const std::function<void(search::SampleRow&)>& patch);
    /// Replaces loaded rows with `current` (matched by key); keys in `requested` that are loaded but
    /// absent from `current` are removed (the sample was deleted).
    void apply_rows(const std::vector<SampleKey>& requested, const std::vector<search::SampleRow>& current);
    /// Removes the loaded rows of `keys` (optimistic "remove from library").
    void remove_rows(const std::vector<SampleKey>& keys);
    /// Marks `keys` as having an edit in flight; refresh_rows() leaves their rows alone meanwhile.
    void begin_pending_edit(const std::vector<SampleKey>& keys);
    /// Ends one in-flight edit of each key.
    void end_pending_edit(const std::vector<SampleKey>& keys);
    /// True while an optimistic edit of `key` is in flight.
    bool has_pending_edit(const SampleKey& key) const;

    // ------------------------------------------------------------ row access
    /// Row data when that row's page is loaded, else nullptr. Valid until the next model change.
    const search::SampleRow* loaded_row(int row) const;
    /// Row number of a key among the loaded rows.
    std::optional<int> row_of(const SampleKey& key) const;
    /// Row numbers of the loaded rows among `keys` (one pass over the loaded rows).
    std::map<SampleKey, int> rows_of(const std::vector<SampleKey>& keys) const;
    /// True while any page or count request of the current generation is outstanding.
    bool is_loading() const;

signals:
    /// The result size is being recounted (shown as "updating").
    void countUpdating();
    /// Exact total of the current generation; `complete` is false when a library was skipped.
    void countChanged(qint64 total, bool complete);
    /// A page or count request failed.
    void queryFailed(const QString& message);
    /// Some libraries could not be searched; empty text means every library answered.
    void coverageChanged(const QString& message);
    /// The header asked for a sort the search service cannot perform.
    void sortRejected();
    /// refresh() replaced the rows (persistent indexes already follow their samples by key).
    void refreshed();
    /// Loading started or finished (for the status bar).
    void loadingChanged(bool loading);

private:
    /// One page of the result. Rows are dropped on eviction; the cursors stay.
    struct Page {
        std::optional<search::Cursor> start;  ///< cursor that fetches this page (nullopt = first)
        std::optional<search::Cursor> next;   ///< cursor of the following page (nullopt = last)
        int first_row = 0;                    ///< model row of the page's first row
        int row_count = 0;                    ///< rows the page covers, loaded or not
        std::vector<search::SampleRow> rows;  ///< empty when evicted
        bool hydrating = false;               ///< a re-read is in flight
        int failed_reads = 0;                 ///< consecutive failed re-reads (drives the back-off)
        QDeadlineTimer retry_after;           ///< no re-read before this deadline
        std::uint64_t last_used = 0;          ///< LRU clock value of the last data() access
    };

    /// Builds the search request for the current query starting at `cursor`.
    search::SearchRequest make_request(const std::optional<search::Cursor>& cursor) const;
    /// Starts a new generation (query, scope or sort changed). The previous rows stay on screen
    /// until the first page of the new generation replaces them in one model reset.
    void restart();
    /// Installs the first page of a new generation (or an empty result) with one model reset.
    void replace_rows(std::optional<search::SearchPage> first_page);
    /// Appends the page after the last loaded one.
    void request_next_page();
    /// Fetches the next page of an in-progress refresh into staged_pages_.
    void request_staged_page();
    /// Swaps the staged pages in: tail rows removed or inserted, the rest replaced as a layout change
    /// that moves persistent indexes with their samples. Announces refreshed().
    void commit_staged_pages();
    /// Cuts `pages` down to their first `rows` rows.
    static void truncate_pages(std::vector<Page>& pages, int rows);
    /// Re-reads every page data() asked for since the last call (queued, outside data()).
    void hydrate_wanted_pages();
    /// Re-reads one evicted page with its stored start cursor.
    void request_hydration(int page_index);
    /// Stores a re-read page; a changed row count triggers a refresh.
    void on_page_hydrated(int page_index, search::SearchPage page);
    /// Records a failed re-read and schedules a repaint once the back-off has passed.
    void on_hydration_failed(int page_index, const Error& error);
    /// Drops the row data of the least recently used pages beyond kMaxHydratedPages.
    void evict_least_recently_used();
    /// Counts the whole result (after the first page of a generation).
    void request_count();
    /// Reports a failed request; a stale cursor re-reads the result instead.
    void report_failure(const Error& error);
    /// Reports libraries that could not be searched.
    void report_coverage(const std::vector<search::LibraryCoverage>& coverage);
    /// Adjusts the outstanding-request count and emits loadingChanged() on transitions.
    void set_pending(int delta);
    /// Forgets every outstanding request (new generation).
    void reset_pending();
    /// Index into pages_ of the page holding `row`, or -1.
    int page_index_of_row(int row) const;
    /// Writable access to a loaded row (rows_of() guarantees it is loaded).
    search::SampleRow* loaded_row_for_update(int row);
    /// Removes one loaded row, shifting the following pages.
    void remove_row(int row);
    /// Repaints whole rows (one dataChanged per row; callers pass few rows).
    void emit_row_changed(int row);
    /// Cell text of a loaded row.
    QVariant display_value(const search::SampleRow& row, Column column) const;
    /// Cell tool tip of a loaded row.
    QVariant tooltip_value(const search::SampleRow& row, Column column) const;

    /// Where pages, counts and rows come from.
    SamplePageSource& source_;
    /// Current query text.
    std::string text_;
    /// Current navigation scope.
    NavigationScope scope_;
    /// Current server-side sort.
    search::SortField sort_field_ = search::SortField::Name;
    bool sort_descending_ = false;
    /// A query has been set (clear() resets it).
    bool active_ = false;

    /// Tags every request; deliveries of older generations are dropped.
    std::uint64_t generation_ = 0;
    // Pages are a cache: data() (const by Qt's API) updates LRU clocks and asks for re-reads.
    mutable std::vector<Page> pages_;
    mutable std::set<int> wanted_pages_;  ///< evicted pages data() wants re-read
    mutable QTimer hydration_timer_;      ///< zero-delay: re-reads start outside data()
    mutable std::uint64_t lru_clock_ = 0;
    /// A fetchMore() page is in flight.
    bool append_pending_ = false;
    /// A new query's first page is pending; the previous rows stay visible until it arrives.
    bool replacing_ = false;

    /// A full refresh is reading staged pages.
    bool refreshing_ = false;
    /// Pages the running refresh re-reads.
    int refresh_target_pages_ = 0;
    /// Pages of the running refresh, swapped in at the end.
    std::vector<Page> staged_pages_;

    /// Outstanding page and count requests of this generation.
    int pending_requests_ = 0;
    std::map<SampleKey, int> pending_edits_;  ///< key → in-flight optimistic edits
};

}  // namespace mso::ui
