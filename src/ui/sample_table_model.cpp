#include "mso/ui/sample_table_model.hpp"

#include <QGuiApplication>
#include <QPalette>
#include <QStringList>

#include <algorithm>
#include <iterator>

#include "mso/ui/display_format.hpp"

namespace mso::ui {

namespace {

using Column = SampleTableModel::Column;

/// True when two navigation scopes select the same samples.
bool same_scope(const NavigationScope& left, const NavigationScope& right) {
    const search::Scope& a = left.scope;
    const search::Scope& b = right.scope;
    return left.library == right.library && a.root_id == b.root_id && a.folder == b.folder &&
           a.collection_id == b.collection_id && a.tag_id == b.tag_id && a.category_id == b.category_id &&
           a.uncategorized == b.uncategorized;
}

/// Columns whose values are numbers and read best right-aligned.
bool is_numeric_column(Column column) {
    switch (column) {
        case Column::Duration:
        case Column::SampleRate:
        case Column::BitDepth:
        case Column::Size:
        case Column::Bpm:
            return true;
        default:
            return false;
    }
}

QVariant disabled_text_brush() {
    return QGuiApplication::palette().brush(QPalette::Disabled, QPalette::Text);
}

/// Display text of a value that may be unknown: the value, or the compact unknown marker.
QString or_unknown(const QString& text) {
    return text.isEmpty() ? DisplayFormat::unknown_marker() : text;
}

}  // namespace

SampleTableModel::SampleTableModel(SamplePageSource& source, QObject* parent)
    : QAbstractTableModel(parent), source_(source) {}

// ---------------------------------------------------------------- QAbstractItemModel

int SampleTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() || pages_.empty()) return 0;
    return pages_.back().first_row + pages_.back().row_count;
}

int SampleTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : kColumnCount;
}

QVariant SampleTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return {};
    const int page_index = page_index_of_row(index.row());
    if (page_index < 0) return {};
    Page& page = pages_[static_cast<std::size_t>(page_index)];
    page.last_used = ++lru_clock_;
    const auto column = static_cast<Column>(index.column());
    const auto offset = static_cast<std::size_t>(index.row() - page.first_row);

    if (offset >= page.rows.size()) {
        // Evicted (or not yet re-read) row: show a distinct loading state and fetch it again.
        // While a new query is pending these rows belong to the old one: never re-read them.
        if (page.rows.empty() && !page.hydrating && !replacing_) request_hydration(page_index);
        switch (role) {
            case Qt::DisplayRole:
                return column == Column::Name ? tr("Loading…") : QVariant();
            case Qt::ForegroundRole:
                return disabled_text_brush();
            case LoadingRole:
                return true;
            default:
                return {};
        }
    }

    const search::SampleRow& row = page.rows[offset];
    switch (role) {
        case Qt::DisplayRole:
            return display_value(row, column);
        case Qt::ToolTipRole:
            return tooltip_value(row, column);
        case Qt::TextAlignmentRole:
            return is_numeric_column(column) ? QVariant(Qt::AlignRight | Qt::AlignVCenter) : QVariant();
        case Qt::ForegroundRole:
            return row.availability == search::RowAvailability::Available ? QVariant() : disabled_text_brush();
        case Qt::AccessibleTextRole:
            if (column == Column::Rating) return DisplayFormat::rating_accessible(row.rating);
            if (column == Column::Favorite) return row.favorite ? tr("Favorite") : tr("Not favorite");
            return display_value(row, column);
        case SampleKeyRole:
            return QVariant::fromValue(SampleKey{row.library, row.sample_id});
        case LoadingRole:
            return false;
        case AvailabilityRole:
            return static_cast<int>(row.availability);
        case RatingRole:
            return row.rating;
        default:
            return {};
    }
}

QVariant SampleTableModel::display_value(const search::SampleRow& row, Column column) const {
    switch (column) {
        case Column::Name:
            return DisplayFormat::from_utf8(row.display_name.empty() ? row.file_name : row.display_name);
        case Column::Format:
            return or_unknown(DisplayFormat::from_utf8(row.format).toUpper());
        case Column::Duration:
            return or_unknown(DisplayFormat::duration(row.duration_us, row.duration_exact));
        case Column::SampleRate:
            return or_unknown(DisplayFormat::sample_rate(row.sample_rate));
        case Column::Channels:
            return or_unknown(DisplayFormat::channels(row.channels));
        case Column::BitDepth:
            return DisplayFormat::bit_depth(row.bit_depth);  // blank when not meaningful
        case Column::Size:
            return or_unknown(DisplayFormat::byte_size(row.byte_size));
        case Column::Bpm:
            if (!row.bpm_milli) return DisplayFormat::unknown_marker();
            return tr("%1 (%2)").arg(DisplayFormat::bpm(row.bpm_milli), DisplayFormat::source_label(row.bpm_source));
        case Column::Key:
            if (!row.key) return DisplayFormat::unknown_marker();
            return tr("%1 (%2)").arg(DisplayFormat::key(row.key), DisplayFormat::source_label(row.key_source));
        case Column::Category:
            return DisplayFormat::from_utf8(row.category);
        case Column::Tags:
            return DisplayFormat::from_utf8(row.tags);
        case Column::Rating:
            return DisplayFormat::rating_stars(row.rating);
        case Column::Favorite:
            return row.favorite ? QString(QChar(0x2665)) : QString();
        case Column::Location: {
            const QString root = DisplayFormat::from_utf8(row.root_name);
            const QString path = DisplayFormat::from_utf8(row.relative_path);
            if (path.isEmpty()) return root;
            return root.isEmpty() ? path : root + QLatin1Char('/') + path;
        }
        case Column::Availability:
            return DisplayFormat::availability(row.availability);
        case Column::Analyzed:
            return DisplayFormat::analysis_flags(row.analysis_flags);
        case Column::Added:
            return DisplayFormat::timestamp(row.created_at);
    }
    return {};
}

QVariant SampleTableModel::tooltip_value(const search::SampleRow& row, Column column) const {
    switch (column) {
        case Column::Name:
            if (!row.display_name.empty() && row.display_name != row.file_name)
                return tr("Original file name: %1").arg(DisplayFormat::from_utf8(row.file_name));
            return DisplayFormat::from_utf8(row.file_name);
        case Column::Duration:
            if (!row.duration_us) return DisplayFormat::unknown();
            return row.duration_exact ? tr("Exact duration") : tr("Estimated duration");
        case Column::Bpm:
            if (!row.bpm_milli)
                return row.bpm_source == "user_unknown" ? tr("Set to unknown by the user") : DisplayFormat::unknown();
            return DisplayFormat::provenance(row.bpm_source, row.bpm_confidence);
        case Column::Key:
            if (!row.key)
                return row.key_source == "user_unknown" ? tr("Set to unknown by the user") : DisplayFormat::unknown();
            return DisplayFormat::provenance(row.key_source, row.key_confidence);
        case Column::BitDepth:
            return row.bit_depth ? QVariant() : QVariant(tr("Not meaningful for this format, or unknown"));
        case Column::Location:
        case Column::Tags:
        case Column::Category:
            return display_value(row, column);
        case Column::Availability:
            if (row.availability == search::RowAvailability::SourceOffline)
                return tr("The source folder is disconnected; metadata stays searchable, playback is disabled.");
            if (row.availability == search::RowAvailability::Missing)
                return tr("The file is no longer at its recorded location.");
            return {};
        default:
            return {};
    }
}

QVariant SampleTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || section < 0 || section >= kColumnCount) return {};
    const auto column = static_cast<Column>(section);
    if (role == Qt::DisplayRole) {
        switch (column) {
            case Column::Name:
                return tr("Name");
            case Column::Format:
                return tr("Format");
            case Column::Duration:
                return tr("Duration");
            case Column::SampleRate:
                return tr("Rate");
            case Column::Channels:
                return tr("Channels");
            case Column::BitDepth:
                return tr("Bit depth");
            case Column::Size:
                return tr("Size");
            case Column::Bpm:
                return tr("BPM");
            case Column::Key:
                return tr("Key");
            case Column::Category:
                return tr("Category");
            case Column::Tags:
                return tr("Tags");
            case Column::Rating:
                return tr("Rating");
            case Column::Favorite:
                return tr("Favorite");
            case Column::Location:
                return tr("Location");
            case Column::Availability:
                return tr("Availability");
            case Column::Analyzed:
                return tr("Analyzed");
            case Column::Added:
                return tr("Date added");
        }
    }
    if (role == Qt::ToolTipRole && !sort_field_for(column)) return tr("This column cannot be sorted yet");
    if (role == Qt::TextAlignmentRole && is_numeric_column(column)) return QVariant(Qt::AlignRight | Qt::AlignVCenter);
    return {};
}

Qt::ItemFlags SampleTableModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (loaded_row(index.row())) result |= Qt::ItemIsDragEnabled;
    return result;
}

Qt::DropActions SampleTableModel::supportedDragActions() const {
    return Qt::CopyAction;
}

bool SampleTableModel::canFetchMore(const QModelIndex& parent) const {
    if (parent.isValid() || !active_ || replacing_ || refreshing_ || append_pending_ || pages_.empty()) return false;
    return pages_.back().next.has_value();
}

void SampleTableModel::fetchMore(const QModelIndex& parent) {
    if (canFetchMore(parent)) request_next_page();
}

void SampleTableModel::sort(int column, Qt::SortOrder order) {
    const auto field =
        column >= 0 && column < kColumnCount ? sort_field_for(static_cast<Column>(column)) : std::nullopt;
    if (!field) {
        emit sortRejected();
        return;
    }
    const bool descending = order == Qt::DescendingOrder;
    if (*field == sort_field_ && descending == sort_descending_) return;
    sort_field_ = *field;
    sort_descending_ = descending;
    if (active_) restart();
}

// ---------------------------------------------------------------- query

void SampleTableModel::set_query(const std::string& text, const NavigationScope& scope) {
    if (active_ && text == text_ && same_scope(scope, scope_)) return;
    run_query(text, scope);
}

void SampleTableModel::run_query(const std::string& text, const NavigationScope& scope) {
    text_ = text;
    scope_ = scope;
    active_ = true;
    restart();
}

void SampleTableModel::clear() {
    active_ = false;
    ++generation_;
    replacing_ = false;
    refreshing_ = false;
    append_pending_ = false;
    staged_pages_.clear();
    beginResetModel();
    pages_.clear();
    endResetModel();
    set_pending(-pending_requests_);
    emit coverageChanged({});
    emit countChanged(0, true);
}

void SampleTableModel::refresh(int anchor_row) {
    if (!active_) return;
    if (pages_.empty()) {
        restart();
        return;
    }
    ++generation_;
    set_pending(-pending_requests_);
    append_pending_ = false;
    refreshing_ = true;
    staged_pages_.clear();
    const int anchor_page = std::max(0, page_index_of_row(anchor_row));
    // The anchor page plus the next one: a viewport can straddle a page boundary.
    refresh_target_pages_ = std::clamp(anchor_page + 2, 1, kMaxHydratedPages);
    request_staged_page();
}

int SampleTableModel::sort_column() const {
    for (int column = 0; column < kColumnCount; ++column)
        if (sort_field_for(static_cast<Column>(column)) == sort_field_) return column;
    return -1;
}

std::optional<search::SortField> SampleTableModel::sort_field_for(Column column) {
    switch (column) {
        case Column::Name:
            return search::SortField::Name;
        case Column::Format:
            return search::SortField::Format;
        case Column::Duration:
            return search::SortField::Duration;
        case Column::SampleRate:
            return search::SortField::SampleRate;
        case Column::Size:
            return search::SortField::Size;
        case Column::Bpm:
            return search::SortField::Bpm;
        case Column::Key:
            return search::SortField::Key;
        case Column::Rating:
            return search::SortField::Rating;
        case Column::Added:
            return search::SortField::Added;
        default:
            return std::nullopt;
    }
}

// ---------------------------------------------------------------- row access

const search::SampleRow* SampleTableModel::loaded_row(int row) const {
    const int page_index = page_index_of_row(row);
    if (page_index < 0) return nullptr;
    const Page& page = pages_[static_cast<std::size_t>(page_index)];
    const auto offset = static_cast<std::size_t>(row - page.first_row);
    return offset < page.rows.size() ? &page.rows[offset] : nullptr;
}

std::optional<int> SampleTableModel::row_of(const SampleKey& key) const {
    for (const Page& page : pages_) {
        for (std::size_t offset = 0; offset < page.rows.size(); ++offset) {
            const search::SampleRow& row = page.rows[offset];
            if (row.sample_id == key.sample_id && row.library == key.library)
                return page.first_row + static_cast<int>(offset);
        }
    }
    return std::nullopt;
}

bool SampleTableModel::is_loading() const {
    return pending_requests_ > 0;
}

int SampleTableModel::page_index_of_row(int row) const {
    if (row < 0) return -1;
    const auto after = std::upper_bound(pages_.begin(), pages_.end(), row,
                                        [](int value, const Page& page) { return value < page.first_row; });
    if (after == pages_.begin()) return -1;
    const auto index = static_cast<int>(std::distance(pages_.begin(), after)) - 1;
    const Page& page = pages_[static_cast<std::size_t>(index)];
    return row < page.first_row + page.row_count ? index : -1;
}

// ---------------------------------------------------------------- fetching

search::SearchRequest SampleTableModel::make_request(const std::optional<search::Cursor>& cursor) const {
    search::SearchRequest request;
    request.text = text_;
    request.scope = scope_.scope;
    request.sort = sort_field_;
    request.descending = sort_descending_;
    request.page_size = kPageSize;
    request.cursor = cursor;
    return request;
}

void SampleTableModel::restart() {
    ++generation_;
    set_pending(-pending_requests_);
    refreshing_ = false;
    staged_pages_.clear();
    replacing_ = true;
    append_pending_ = true;
    emit coverageChanged({});
    const std::uint64_t generation = generation_;
    set_pending(+1);
    source_.request_page(make_request(std::nullopt), scope_.library, this,
                         [this, generation](Result<search::SearchPage> result) {
                             if (generation != generation_) return;  // superseded by a newer query
                             set_pending(-1);
                             if (!result) {
                                 replace_rows(std::nullopt);
                                 report_failure(result.error());
                                 return;
                             }
                             report_coverage(result->coverage);
                             replace_rows(std::move(*result));
                             request_count();
                         });
}

void SampleTableModel::replace_rows(std::optional<search::SearchPage> first_page) {
    replacing_ = false;
    append_pending_ = false;
    beginResetModel();
    pages_.clear();
    if (first_page && !first_page->rows.empty()) {
        Page page;
        page.next = std::move(first_page->next);
        page.row_count = static_cast<int>(first_page->rows.size());
        page.rows = std::move(first_page->rows);
        page.last_used = ++lru_clock_;
        pages_.push_back(std::move(page));
    }
    endResetModel();
}

void SampleTableModel::request_next_page() {
    // Only fetchMore() appends; the first page of a generation comes from restart().
    if (pages_.empty()) return;
    append_pending_ = true;
    const std::optional<search::Cursor> start = pages_.back().next;
    const std::uint64_t generation = generation_;
    set_pending(+1);
    source_.request_page(make_request(start), scope_.library, this,
                         [this, generation, start](Result<search::SearchPage> result) {
                             if (generation != generation_) return;  // obsolete query
                             set_pending(-1);
                             append_pending_ = false;
                             if (!result) {
                                 report_failure(result.error());
                                 return;
                             }
                             report_coverage(result->coverage);
                             if (!result->rows.empty()) {
                                 Page page;
                                 page.start = start;
                                 page.next = std::move(result->next);
                                 page.first_row = rowCount();
                                 page.row_count = static_cast<int>(result->rows.size());
                                 page.rows = std::move(result->rows);
                                 page.last_used = ++lru_clock_;
                                 beginInsertRows({}, page.first_row, page.first_row + page.row_count - 1);
                                 pages_.push_back(std::move(page));
                                 endInsertRows();
                                 evict_least_recently_used();
                             } else {
                                 pages_.back().next.reset();  // an empty continuation ends the result
                             }
                         });
}

void SampleTableModel::request_staged_page() {
    const std::optional<search::Cursor> start = staged_pages_.empty() ? std::nullopt : staged_pages_.back().next;
    const std::uint64_t generation = generation_;
    set_pending(+1);
    source_.request_page(
        make_request(start), scope_.library, this, [this, generation, start](Result<search::SearchPage> result) {
            if (generation != generation_) return;
            set_pending(-1);
            if (!result) {
                refreshing_ = false;
                staged_pages_.clear();
                report_failure(result.error());
                return;
            }
            report_coverage(result->coverage);
            if (!result->rows.empty()) {
                Page page;
                page.start = start;
                page.next = std::move(result->next);
                page.first_row =
                    staged_pages_.empty() ? 0 : staged_pages_.back().first_row + staged_pages_.back().row_count;
                page.row_count = static_cast<int>(result->rows.size());
                page.rows = std::move(result->rows);
                page.last_used = ++lru_clock_;
                staged_pages_.push_back(std::move(page));
            } else if (!staged_pages_.empty()) {
                staged_pages_.back().next.reset();
            }
            const bool more = !staged_pages_.empty() && staged_pages_.back().next &&
                              static_cast<int>(staged_pages_.size()) < refresh_target_pages_;
            if (more)
                request_staged_page();
            else
                commit_staged_pages();
        });
}

void SampleTableModel::commit_staged_pages() {
    refreshing_ = false;
    // Hydrations requested against the old page table must not land in the new one.
    ++generation_;
    set_pending(-pending_requests_);
    const int old_rows = rowCount();
    const int new_rows = staged_pages_.empty() ? 0 : staged_pages_.back().first_row + staged_pages_.back().row_count;
    if (new_rows < old_rows) {
        beginRemoveRows({}, new_rows, old_rows - 1);
        pages_ = std::move(staged_pages_);
        endRemoveRows();
    } else if (new_rows > old_rows) {
        beginInsertRows({}, old_rows, new_rows - 1);
        pages_ = std::move(staged_pages_);
        endInsertRows();
    } else {
        pages_ = std::move(staged_pages_);
    }
    staged_pages_.clear();
    const int unchanged_rows = std::min(old_rows, new_rows);
    if (unchanged_rows > 0) emit dataChanged(index(0, 0), index(unchanged_rows - 1, kColumnCount - 1));
    emit refreshed();
    request_count();
}

void SampleTableModel::request_hydration(int page_index) const {
    Page& page = pages_[static_cast<std::size_t>(page_index)];
    page.hydrating = true;
    // data() is const by Qt's API; re-reading an evicted page only refills this model's cache.
    auto* self = const_cast<SampleTableModel*>(this);
    const std::uint64_t generation = generation_;
    source_.request_page(make_request(page.start), scope_.library, self,
                         [self, generation, page_index](Result<search::SearchPage> result) {
                             if (generation != self->generation_) return;
                             if (!result) {
                                 self->pages_[static_cast<std::size_t>(page_index)].hydrating = false;
                                 self->report_failure(result.error());
                                 return;
                             }
                             self->on_page_hydrated(page_index, std::move(*result));
                         });
}

void SampleTableModel::on_page_hydrated(int page_index, search::SearchPage page) {
    if (page_index < 0 || static_cast<std::size_t>(page_index) >= pages_.size()) return;
    Page& target = pages_[static_cast<std::size_t>(page_index)];
    target.hydrating = false;
    const bool result_changed = static_cast<int>(page.rows.size()) != target.row_count;
    if (static_cast<int>(page.rows.size()) > target.row_count)
        page.rows.resize(static_cast<std::size_t>(target.row_count));
    target.rows = std::move(page.rows);
    target.last_used = ++lru_clock_;
    if (target.row_count > 0)
        emit dataChanged(index(target.first_row, 0), index(target.first_row + target.row_count - 1, kColumnCount - 1));
    evict_least_recently_used();
    // Rows were added or removed before this page since it was first read: re-read the window.
    if (result_changed) emit queryFailed(tr("The results changed while scrolling; refreshing."));
    if (result_changed) refresh(target.first_row);
}

void SampleTableModel::evict_least_recently_used() {
    auto hydrated_pages =
        std::count_if(pages_.begin(), pages_.end(), [](const Page& page) { return !page.rows.empty(); });
    while (hydrated_pages > kMaxHydratedPages) {
        Page* oldest = nullptr;
        for (Page& page : pages_)
            if (!page.rows.empty() && (!oldest || page.last_used < oldest->last_used)) oldest = &page;
        if (!oldest) break;
        // Logically the rows are unchanged; only their cached copy is dropped (re-read on demand).
        oldest->rows.clear();
        oldest->rows.shrink_to_fit();
        --hydrated_pages;
    }
}

void SampleTableModel::request_count() {
    const std::uint64_t generation = generation_;
    emit countUpdating();
    set_pending(+1);
    source_.request_count(make_request(std::nullopt), scope_.library, this,
                          [this, generation](Result<search::CountResult> result) {
                              if (generation != generation_) return;
                              set_pending(-1);
                              if (!result) {
                                  report_failure(result.error());
                                  return;
                              }
                              const bool complete = std::all_of(result->coverage.begin(), result->coverage.end(),
                                                                [](const auto& library) { return library.complete; });
                              emit countChanged(result->total, complete);
                          });
}

void SampleTableModel::report_failure(const Error& error) {
    if (error.code == ErrorCode::StaleCursor) {
        refresh(0);
        return;
    }
    emit queryFailed(DisplayFormat::error(error));
}

void SampleTableModel::report_coverage(const std::vector<search::LibraryCoverage>& coverage) {
    QStringList missing;
    for (const auto& library : coverage) {
        if (library.complete) continue;
        QString entry = DisplayFormat::from_utf8(library.name);
        if (library.error) entry += tr(" (%1)").arg(DisplayFormat::error(*library.error));
        missing << entry;
    }
    emit coverageChanged(missing.isEmpty()
                             ? QString()
                             : tr("Partial results; not searched: %1").arg(missing.join(QStringLiteral(", "))));
}

void SampleTableModel::set_pending(int delta) {
    const bool was_loading = pending_requests_ > 0;
    pending_requests_ = std::max(0, pending_requests_ + delta);
    const bool loading = pending_requests_ > 0;
    if (was_loading != loading) emit loadingChanged(loading);
}

}  // namespace mso::ui
