#pragma once
/// @file
/// SampleTableModel: a bounded, paged view of a search result of any size.
///
/// Memory and threading contract (docs/02_ARCHITECTURE.md §3, docs/03_DATA_AND_SEARCH.md §7):
///  - pages of at most kPageSize rows are fetched asynchronously through a SamplePageSource with
///    keyset cursors; rowCount() grows as the view scrolls (canFetchMore/fetchMore);
///  - at most kMaxHydratedPages pages (2,000 rows) keep their row data. The least recently used
///    page beyond that is evicted and keeps only its start cursor (a few hundred bytes), so its
///    rows show as "Loading…" and are re-fetched when they become visible again;
///  - every result is tagged with the query generation it was requested for; results of an
///    obsolete generation are discarded;
///  - sorting re-queries the whole result with the server-side sort.
///
/// Known P1 limitation: refresh() (after catalog changes) re-reads the result from its start up to
/// the anchor row, capped at 2,000 rows; a view scrolled deeper returns to that window. Start cursors
/// of all pages ever fetched are kept, so very deep scrolling costs one small string per 200 rows.
#include <QAbstractTableModel>
#include <QString>

#include <cstdint>
#include <optional>
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
    static constexpr int kColumnCount = static_cast<int>(Column::Added) + 1;

    /// Extra item data roles.
    enum Role {
        /// QVariant holding a SampleKey (loaded rows only).
        SampleKeyRole = Qt::UserRole + 1,
        /// True while the row's page is being (re-)fetched.
        LoadingRole,
        /// int value of search::RowAvailability.
        AvailabilityRole,
        /// int rating 0..5 (the rating column paints and edits stars from it).
        RatingRole,
    };

    static constexpr int kPageSize = 200;
    static constexpr int kMaxHydratedPages = 10;

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
    /// Re-reads the result after catalog changes, keeping rows visible until the new pages
    /// arrive. `anchor_row` is the first row the view shows; rows up to it are restored.
    void refresh(int anchor_row);

    const std::string& query_text() const { return text_; }
    const NavigationScope& scope() const { return scope_; }
    search::SortField sort_field() const { return sort_field_; }
    bool sort_descending() const { return sort_descending_; }
    /// Column showing the current sort, for the header's sort indicator.
    int sort_column() const;
    /// Server-side sort of a column, or nullopt for columns the search service cannot sort.
    static std::optional<search::SortField> sort_field_for(Column column);

    // ------------------------------------------------------------ row access
    /// Row data when that row's page is loaded, else nullptr. Valid until the next model change.
    const search::SampleRow* loaded_row(int row) const;
    /// Row number of a key among the loaded rows.
    std::optional<int> row_of(const SampleKey& key) const;
    /// True while any page or count request of the current generation is outstanding.
    bool is_loading() const;

signals:
    /// The result size is being recounted (shown as "updating").
    void countUpdating();
    /// Exact total of the current generation; `complete` is false when a library was skipped.
    void countChanged(qint64 total, bool complete);
    /// A page or count request failed; the previous rows stay visible.
    void queryFailed(const QString& message);
    /// Some libraries could not be searched; empty text means every library answered.
    void coverageChanged(const QString& message);
    /// The header asked for a sort the search service cannot perform.
    void sortRejected();
    /// refresh() replaced the rows; selections by key should be restored now.
    void refreshed();
    /// Loading started or finished (for the status bar).
    void loadingChanged(bool loading);

private:
    /// One page of the result. Rows are dropped on eviction; the cursors stay.
    struct Page {
        std::optional<search::Cursor> start;  ///< cursor that fetches this page (nullopt = first)
        std::optional<search::Cursor> next;   ///< cursor of the following page (nullopt = last)
        int first_row = 0;
        int row_count = 0;
        std::vector<search::SampleRow> rows;  ///< empty when evicted
        bool hydrating = false;
        std::uint64_t last_used = 0;  ///< LRU clock value of the last data() access
    };

    search::SearchRequest make_request(const std::optional<search::Cursor>& cursor) const;
    /// Starts a new generation (query, scope or sort changed). The previous rows stay on screen
    /// until the first page of the new generation replaces them in one model reset.
    void restart();
    /// Installs the first page of a new generation (or an empty result) with one model reset.
    void replace_rows(std::optional<search::SearchPage> first_page);
    /// Appends the page after the last loaded one (or the first page).
    void request_next_page();
    /// Fetches the next page of an in-progress refresh into staged_pages_.
    void request_staged_page();
    /// Swaps the staged pages in, keeping the view's rows stable where the counts agree.
    void commit_staged_pages();
    /// Re-reads an evicted page with its stored start cursor.
    void request_hydration(int page_index) const;
    /// Stores a re-read page; a changed row count triggers a refresh.
    void on_page_hydrated(int page_index, search::SearchPage page);
    void evict_least_recently_used();
    void request_count();
    void report_failure(const Error& error);
    void report_coverage(const std::vector<search::LibraryCoverage>& coverage);
    void set_pending(int delta);
    int page_index_of_row(int row) const;
    QVariant display_value(const search::SampleRow& row, Column column) const;
    QVariant tooltip_value(const search::SampleRow& row, Column column) const;

    SamplePageSource& source_;
    std::string text_;
    NavigationScope scope_;
    search::SortField sort_field_ = search::SortField::Name;
    bool sort_descending_ = false;
    bool active_ = false;

    std::uint64_t generation_ = 0;
    // Pages are a cache: data() (const by Qt's API) updates LRU clocks and requests re-hydration.
    mutable std::vector<Page> pages_;
    bool append_pending_ = false;

    /// A new query's first page is pending; the previous rows stay visible until it arrives.
    bool replacing_ = false;
    bool refreshing_ = false;
    int refresh_target_pages_ = 0;
    std::vector<Page> staged_pages_;

    int pending_requests_ = 0;
    mutable std::uint64_t lru_clock_ = 0;
};

}  // namespace mso::ui
