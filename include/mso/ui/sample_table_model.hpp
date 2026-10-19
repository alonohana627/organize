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
///    obsolete generation are discarded, and requests of an obsolete generation that have not
///    started yet are skipped (stop token). A new query keeps the previous rows on screen until its
///    first page replaces them in one model reset (no flash of empty rows); if that first page
///    fails, the rows are cleared and queryFailed() says why;
///  - every page keeps the keys of its rows (also when its row data is evicted): a re-read page
///    must hold exactly those samples, else the result changed and the table is refreshed;
///  - keyset cursors may return a row again on a later page when its own sort value changed
///    across the cursor: appended and refreshed pages skip samples the table already has;
///  - sorting re-queries the whole result with the server-side sort;
///  - edits are optimistic: patch_rows()/remove_rows() change the visible copies immediately and
///    begin_pending_edit()/end_pending_edit() keep every later read (in-place refresh, appended,
///    re-read or refreshed page) from showing the old value until the background write has been
///    reconciled.
///
/// Catalog changes refresh only the affected rows (refresh_rows). A full reload, and any
/// STALE_CURSOR (the result's structure changed under a cursor), runs an anchored refresh: the
/// result is re-read from its start with fresh cursors for as many pages as the table has loaded
/// (at least through the anchor row), keeping row data only around the anchor. The new pages
/// replace the old ones as a layout change, so the row count, the scroll position and the
/// selection (by key) are kept; nothing is cut back to the first pages.
///
/// At most one anchored refresh runs at a time. refresh() during a running one (or during the
/// short pause after a commit) only marks the table dirty, and exactly one more refresh follows
/// kOwedRefreshDelayMilliseconds after the commit, so paging and re-reads run in between and a
/// stream of changes (a long analysis run) can never keep a refresh from committing.
///
/// Row reads (refresh_rows) are tagged with the generation they were requested in. A read that
/// arrives after the generation moved (a refresh started or committed) is not lost: its keys are
/// read again in the current generation, after the commit when a refresh is running, so the last
/// value applied always comes from a read issued after the newest page read.
#include <QAbstractTableModel>
#include <QDeadlineTimer>
#include <QString>
#include <QTimer>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
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
    /// Pages an anchored refresh re-reads at most (500,000 rows; only their cursors are kept
    /// outside the anchor's window). A table scrolled deeper keeps this many rows after a refresh.
    static constexpr int kMaxRefreshPages = 2'500;
    /// Rows the table loads at most (paging stops here, so a refresh can always cover them all).
    static constexpr int kMaxLoadedRows = kMaxRefreshPages * kPageSize;
    /// Pause between a committed refresh and the one owed to changes that arrived meanwhile.
    static constexpr int kOwedRefreshDelayMilliseconds = 250;
    /// First and last delay before re-reading a page whose last read failed.
    static constexpr int kMinimumRetryMilliseconds = 1'000;
    static constexpr int kMaximumRetryMilliseconds = 30'000;
    /// Longest wait before an anchored refresh that met another structure change starts again.
    static constexpr int kMaximumStaleRetryMilliseconds = 1'000;

    /// A model fed by `source`, which must outlive it.
    explicit SampleTableModel(SamplePageSource& source, QObject* parent = nullptr);
    ~SampleTableModel() override;

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
    /// Anchored refresh (see the file comment). Rows stay visible until the new pages arrive;
    /// `anchor_row` is the first row the view shows, and the rows around it are read first-hand.
    /// While a refresh runs this only owes one more (see the file comment).
    void refresh(int anchor_row);
    /// Re-reads the given samples in place (catalog change events); samples that no longer exist
    /// are removed, and a failed read is reported (queryFailed) without removing anything. Keys
    /// whose row data is not in memory or that have a pending optimistic edit are not read (an
    /// evicted page is re-read when it is shown again). Returns how many of `keys` the table
    /// already holds, loaded or evicted (the others may be new or may now match: the caller
    /// decides whether the result window needs a re-read).
    std::size_t refresh_rows(const std::vector<SampleKey>& keys);

    /// Current query text.
    const std::string& query_text() const { return text_; }
    /// Current navigation scope.
    const NavigationScope& scope() const { return scope_; }
    /// Current server-side sort.
    search::SortField sort_field() const { return sort_field_; }
    /// True when the current sort is descending.
    bool sort_descending() const { return sort_descending_; }
    /// Column showing the current sort, for the header's sort indicator.
    int sort_column() const;
    /// Server-side sort of a column, or nullopt for columns the search service cannot sort.
    static std::optional<search::SortField> sort_field_for(Column column);

    // ------------------------------------------------------------ optimistic edits
    /// Changes one row the way an edit will (OptimisticEdit::apply).
    using RowPatch = std::function<void(search::SampleRow&)>;

    /// Applies `patch` to the loaded rows of `keys` and repaints them. Returns copies of the rows
    /// as they were before the patch.
    std::vector<search::SampleRow> patch_rows(const std::vector<SampleKey>& keys, const RowPatch& patch);
    /// Replaces loaded rows with `current` (matched by key); keys in `requested` that are loaded but
    /// absent from `current` are removed (the sample was deleted).
    void apply_rows(const std::vector<SampleKey>& requested, const std::vector<search::SampleRow>& current);
    /// Undoes an optimistic patch on screen: loaded rows of `keys` that have a copy in `previous`
    /// (patch_rows()'s result) get it back; any other loaded row of `keys` (its page was re-read
    /// with the prediction meanwhile) drops its page's row data, so it is read again when shown.
    void revert_rows(const std::vector<SampleKey>& keys, const std::vector<search::SampleRow>& previous);
    /// Removes the loaded rows of `keys` (optimistic "remove from library").
    void remove_rows(const std::vector<SampleKey>& keys);
    /// Marks `keys` as having an edit in flight. Their loaded rows (already patched) are what the
    /// table shows until the edit ends: every later read of those samples is overlaid with them.
    /// Rows that are not loaded now (evicted pages of a large selection) get `patch` applied when
    /// their page is read again, so they show the prediction as well.
    void begin_pending_edit(const std::vector<SampleKey>& keys, RowPatch patch = {});
    /// Ends one in-flight edit of each key.
    void end_pending_edit(const std::vector<SampleKey>& keys);
    /// True while an optimistic edit of `key` is in flight.
    bool has_pending_edit(const SampleKey& key) const;

    // ------------------------------------------------------------ row access
    /// Row data when that row's page is loaded, else nullptr. Valid until the next model change.
    const search::SampleRow* loaded_row(int row) const;
    /// Key of a row, loaded or evicted (every page keeps its keys); nullopt outside the table.
    std::optional<SampleKey> key_at(int row) const;
    /// Row number of a key among the loaded rows.
    std::optional<int> row_of(const SampleKey& key) const;
    /// Row numbers of the loaded rows among `keys` (one pass over the loaded rows).
    std::map<SampleKey, int> rows_of(const std::vector<SampleKey>& keys) const;
    /// True while any page or count request of the current generation is outstanding.
    bool is_loading() const;
    /// True while an anchored refresh is reading its pages (or waiting to walk again).
    bool is_refreshing() const { return refreshing_; }
    /// True while a refresh is owed to changes that arrived during the last one.
    bool refresh_owed() const { return refresh_owed_; }

signals:
    /// The result size is being recounted (shown as "updating").
    void countUpdating();
    /// Exact total of the current generation; `complete` is false when a library was skipped.
    void countChanged(qint64 total, bool complete);
    /// A page, count or row request failed.
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
        std::vector<SampleKey> keys;          ///< keys of all row_count rows, kept when evicted
        bool hydrating = false;               ///< a re-read is in flight
        int failed_reads = 0;                 ///< consecutive failed re-reads (drives the back-off)
        QDeadlineTimer retry_after;           ///< no re-read before this deadline
        std::uint64_t last_used = 0;          ///< LRU clock value of the last data() access
    };

    /// The optimistic edits in flight for one sample. Kept small (a large selection has one per
    /// sample): the patched row is held only for samples that were loaded when an edit began.
    struct PendingEdit {
        int count = 0;                                         ///< edits of the sample still in flight
        std::unique_ptr<const search::SampleRow> row;          ///< the patched row the table shows meanwhile
        std::vector<std::shared_ptr<const RowPatch>> patches;  ///< for re-read rows without `row`, in order
    };

    /// Builds the search request for the current query starting at `cursor`.
    search::SearchRequest make_request(const std::optional<search::Cursor>& cursor) const;
    /// Moves to a new result generation: deliveries of older ones are dropped and their requests
    /// that have not started are skipped.
    void advance_generation();
    /// Starts a new generation (query, scope or sort changed). The previous rows stay on screen
    /// until the first page of the new generation replaces them in one model reset.
    void restart();
    /// Forgets every refresh in progress or owed (a new query or a cleared table replaces them).
    void cancel_refreshes();
    /// Starts (or, after a structure change, restarts) the walk of an anchored refresh.
    void start_refresh_walk(int anchor_row);
    /// Runs the refresh owed to changes that arrived during the last one (owed_refresh_timer_).
    void start_owed_refresh();
    /// Re-reads, in the current generation, rows whose read was delivered to an older one.
    void reread_rows(std::vector<SampleKey> keys);
    /// Installs the first page of a new generation (or an empty result) with one model reset.
    void replace_rows(std::optional<search::SearchPage> first_page);
    /// Appends the page after the last loaded one.
    void request_next_page();
    /// Fetches the next page of an in-progress refresh into staged_pages_.
    void request_staged_page();
    /// Swaps the staged pages in as one layout change that moves persistent indexes with their
    /// samples: extra rows are inserted before it, missing rows removed after it, so an index whose
    /// sample is still in the result stays valid. Announces refreshed().
    void commit_staged_pages();
    /// Restarts an anchored refresh that met another structure change, after a short back-off.
    /// The refresh stays running meanwhile (no re-reads with stale cursors, no paging).
    void retry_stale_refresh();
    /// Cuts `pages` down to their first `rows` rows.
    static void truncate_pages(std::vector<Page>& pages, int rows);
    /// A page that only covers rows (`row_count` rows from `first_row`, no data, unknown keys): it
    /// fills the old or new page table for the instant of a commit's row insertion or removal.
    static Page placeholder_page(int first_row, int row_count);
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
    /// Reports a failed request. A stale cursor runs an anchored refresh at `anchor_row` instead.
    void report_failure(const Error& error, int anchor_row);
    /// Reports libraries that could not be searched.
    void report_coverage(const std::vector<search::LibraryCoverage>& coverage);
    /// Removes rows whose sample is already in `seen` (adding the others to it) and fills
    /// `page.keys`; returns the rows kept.
    static std::vector<search::SampleRow> drop_seen_rows(std::vector<search::SampleRow> rows, std::set<SampleKey>& seen,
                                                         std::vector<SampleKey>& keys);
    /// Replaces rows of samples with an edit in flight by the edit's patched row (or patches them).
    void overlay_pending_edits(std::vector<search::SampleRow>& rows) const;
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
    /// True when the current sort is descending.
    bool sort_descending_ = false;
    /// A query has been set (clear() resets it).
    bool active_ = false;

    /// Tags every request; deliveries of older generations are dropped.
    std::uint64_t generation_ = 0;
    /// Stopped when the generation advances (skips its requests that have not started).
    std::stop_source generation_stop_;
    // Pages are a cache: data() (const by Qt's API) updates LRU clocks and asks for re-reads.
    mutable std::vector<Page> pages_;      ///< the result, page by page (rows of evicted pages dropped)
    mutable std::set<int> wanted_pages_;   ///< evicted pages data() wants re-read
    mutable QTimer hydration_timer_;       ///< zero-delay: re-reads start outside data()
    mutable std::uint64_t lru_clock_ = 0;  ///< increments on every data() access of a page
    /// A fetchMore() page is in flight.
    bool append_pending_ = false;
    /// A new query's first page is pending; the previous rows stay visible until it arrives.
    bool replacing_ = false;

    /// An anchored refresh is reading staged pages (or waiting to walk again after a stale cursor).
    bool refreshing_ = false;
    /// refresh() was called while one ran or was owed: exactly one more refresh follows.
    bool refresh_owed_ = false;
    /// Anchor of the owed refresh (the latest one asked for).
    int owed_refresh_anchor_row_ = 0;
    /// Starts the owed refresh kOwedRefreshDelayMilliseconds after a commit.
    QTimer owed_refresh_timer_;
    /// The owed refresh came due while a page was being appended: it starts when that page lands.
    bool owed_refresh_after_append_ = false;
    /// Rows whose read arrived while a refresh ran: read again once it has committed.
    std::set<SampleKey> rows_owed_after_refresh_;
    /// The running refresh reads until its staged pages cover this many rows (or the result ends).
    int refresh_target_rows_ = 0;
    /// Staged rows in [first, last) keep their data; the others keep only their cursors.
    int refresh_window_first_row_ = 0;
    int refresh_window_end_row_ = 0;  ///< end (exclusive) of the hydrated window of the refresh
    /// Anchor of the running refresh (restarted with it after a structure change).
    int refresh_anchor_row_ = 0;
    /// Consecutive refreshes that met a structure change before committing (drives the back-off).
    int stale_refresh_retries_ = 0;
    /// Pages of the running refresh, swapped in at the end.
    std::vector<Page> staged_pages_;
    /// Every sample in pages_ (de-duplicates appended pages); bounded by the rows the table covers.
    std::set<SampleKey> seen_keys_;
    /// Every sample in staged_pages_.
    std::set<SampleKey> staged_keys_;

    /// Outstanding page and count requests of this generation.
    int pending_requests_ = 0;
    std::map<SampleKey, PendingEdit> pending_edits_;  ///< samples with optimistic edits in flight
};

}  // namespace mso::ui
