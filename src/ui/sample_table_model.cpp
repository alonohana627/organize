#include "mso/ui/sample_table_model.hpp"

#include <QGuiApplication>
#include <QPalette>
#include <QStringList>

#include <algorithm>
#include <iterator>
#include <utility>

#include "mso/ui/display_format.hpp"

namespace mso::ui {

namespace {

using Column = SampleTableModel::Column;

/// True when two navigation scopes select the same samples.
bool same_scope(const NavigationScope& left, const NavigationScope& right) {
    const search::Scope& left_scope = left.scope;
    const search::Scope& right_scope = right.scope;
    return left.library == right.library && left_scope.root_id == right_scope.root_id &&
           left_scope.folder == right_scope.folder && left_scope.collection_id == right_scope.collection_id &&
           left_scope.tag_id == right_scope.tag_id && left_scope.category_id == right_scope.category_id &&
           left_scope.uncategorized == right_scope.uncategorized;
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

SampleKey key_of(const search::SampleRow& row) {
    return SampleKey{row.library, row.sample_id};
}

}  // namespace

SampleTableModel::SampleTableModel(SamplePageSource& source, QObject* parent)
    : QAbstractTableModel(parent), source_(source) {
    hydration_timer_.setSingleShot(true);
    hydration_timer_.setInterval(0);
    connect(&hydration_timer_, &QTimer::timeout, this, &SampleTableModel::hydrate_wanted_pages);
}

SampleTableModel::~SampleTableModel() {
    generation_stop_.request_stop();  // queued requests for this model skip their work
}

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
        // Evicted row: show a distinct loading state and ask for a re-read (started outside data()).
        // While a new query is pending these rows belong to the old one: never re-read them.
        // During a refresh the old cursors may be stale: the refreshed pages arrive soon anyway.
        if (page.rows.empty() && !page.hydrating && !replacing_ && !refreshing_ && page.retry_after.hasExpired()) {
            wanted_pages_.insert(page_index);
            hydration_timer_.start();
        }
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
            return DisplayFormat::tag_list(row.tag_paths);
        case Column::Rating:
            return DisplayFormat::rating_stars(row.rating);
        case Column::Favorite:
            return row.favorite ? QString(QChar(0x2665)) : QString();
        case Column::Location: {
            const QString root = DisplayFormat::from_utf8(row.root_name);
            const QString path = DisplayFormat::from_utf8(row.relative_path);
            if (path.isEmpty()) return root;
            return root.isEmpty() ? path : tr("%1/%2").arg(root, path);
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
    advance_generation();
    replacing_ = false;
    refreshing_ = false;
    append_pending_ = false;
    staged_pages_.clear();
    staged_keys_.clear();
    wanted_pages_.clear();
    beginResetModel();
    pages_.clear();
    seen_keys_.clear();
    endResetModel();
    reset_pending();
    emit coverageChanged({});
    emit countChanged(0, true);
}

void SampleTableModel::refresh(int anchor_row) {
    if (!active_) return;
    if (pages_.empty() || replacing_) {
        // Nothing to keep, or a new query is already on its way: a fresh generation covers both
        // (and never leaves replacing_ set, which would stop paging).
        restart();
        return;
    }
    advance_generation();
    reset_pending();
    append_pending_ = false;
    refreshing_ = true;
    staged_pages_.clear();
    staged_keys_.clear();
    wanted_pages_.clear();
    for (Page& page : pages_)
        page.hydrating = false;  // their deliveries belong to the old generation and are dropped
    const int loaded_rows = rowCount();
    refresh_anchor_row_ = std::clamp(anchor_row, 0, std::max(0, loaded_rows - 1));
    // Cover every loaded row (a view scrolled to row 1,200 stays there) and at least the anchor's
    // page and the next one (a viewport can straddle a page boundary).
    refresh_target_rows_ =
        std::min(std::max(loaded_rows, refresh_anchor_row_ + 2 * kPageSize), kMaxRefreshPages * kPageSize);
    // Row data is kept around the anchor only; the view asks for anything else it shows.
    refresh_window_first_row_ = std::max(0, refresh_anchor_row_ - kPageSize);
    refresh_window_end_row_ = refresh_window_first_row_ + (kMaxHydratedPages - 1) * kPageSize;
    request_staged_page();
}

std::size_t SampleTableModel::refresh_rows(const std::vector<SampleKey>& keys) {
    if (!active_ || pages_.empty()) return 0;
    const std::map<SampleKey, int> loaded_rows = rows_of(keys);
    std::vector<SampleKey> settled;
    for (const auto& [key, row] : loaded_rows)
        if (!has_pending_edit(key)) settled.push_back(key);
    if (settled.empty()) return loaded_rows.size();
    const std::uint64_t generation = generation_;
    source_.request_rows(settled, this, [this, generation, settled](Result<std::vector<search::SampleRow>> current) {
        if (generation != generation_) return;  // the rows were replaced meanwhile
        if (!current) {
            // Unknown is not deleted: the rows stay as they are.
            emit queryFailed(tr("Some rows could not be updated: %1").arg(DisplayFormat::error(current.error())));
            return;
        }
        std::vector<SampleKey> still_settled;
        for (const SampleKey& key : settled)
            if (!has_pending_edit(key)) still_settled.push_back(key);
        apply_rows(still_settled, *current);
    });
    return loaded_rows.size();
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

// ---------------------------------------------------------------- optimistic edits

std::vector<search::SampleRow> SampleTableModel::patch_rows(const std::vector<SampleKey>& keys,
                                                            const std::function<void(search::SampleRow&)>& patch) {
    std::vector<search::SampleRow> previous;
    for (const auto& [key, row] : rows_of(keys)) {
        search::SampleRow* sample = loaded_row_for_update(row);
        previous.push_back(*sample);
        patch(*sample);
        if (auto pending = pending_edits_.find(key); pending != pending_edits_.end()) pending->second.row = *sample;
        emit_row_changed(row);
    }
    return previous;
}

void SampleTableModel::apply_rows(const std::vector<SampleKey>& requested,
                                  const std::vector<search::SampleRow>& current) {
    std::map<SampleKey, const search::SampleRow*> by_key;
    for (const search::SampleRow& row : current)
        by_key.emplace(key_of(row), &row);
    std::vector<SampleKey> deleted;
    for (const auto& [key, row] : rows_of(requested)) {
        const auto found = by_key.find(key);
        if (found == by_key.end()) {
            deleted.push_back(key);
            continue;
        }
        *loaded_row_for_update(row) = *found->second;
        emit_row_changed(row);
    }
    remove_rows(deleted);
}

void SampleTableModel::remove_rows(const std::vector<SampleKey>& keys) {
    std::vector<int> rows;
    for (const auto& [key, row] : rows_of(keys))
        rows.push_back(row);
    std::ranges::sort(rows, std::greater<>());  // bottom-up keeps the remaining row numbers valid
    for (int row : rows)
        remove_row(row);
}

void SampleTableModel::remove_row(int row) {
    const int page_index = page_index_of_row(row);
    if (page_index < 0) return;
    beginRemoveRows({}, row, row);
    Page& page = pages_[static_cast<std::size_t>(page_index)];
    const auto offset = static_cast<std::ptrdiff_t>(row - page.first_row);
    if (offset < static_cast<std::ptrdiff_t>(page.rows.size())) page.rows.erase(page.rows.begin() + offset);
    seen_keys_.erase(page.keys[static_cast<std::size_t>(offset)]);
    page.keys.erase(page.keys.begin() + offset);
    --page.row_count;
    for (std::size_t later = static_cast<std::size_t>(page_index) + 1; later < pages_.size(); ++later)
        --pages_[later].first_row;
    if (page.row_count == 0) {
        // Keep the chain of cursors: the previous page now continues where this one did.
        if (page_index > 0) pages_[static_cast<std::size_t>(page_index) - 1].next = page.next;
        pages_.erase(pages_.begin() + page_index);
    }
    endRemoveRows();
}

void SampleTableModel::begin_pending_edit(const std::vector<SampleKey>& keys) {
    const std::map<SampleKey, int> loaded = rows_of(keys);
    for (const SampleKey& key : keys) {
        PendingEdit& pending = pending_edits_[key];
        ++pending.count;
        // The loaded row already carries the prediction: it is what every later read shows.
        if (const auto row = loaded.find(key); row != loaded.end()) pending.row = *loaded_row(row->second);
    }
}

void SampleTableModel::end_pending_edit(const std::vector<SampleKey>& keys) {
    for (const SampleKey& key : keys) {
        const auto found = pending_edits_.find(key);
        if (found != pending_edits_.end() && --found->second.count <= 0) pending_edits_.erase(found);
    }
}

bool SampleTableModel::has_pending_edit(const SampleKey& key) const {
    return pending_edits_.contains(key);
}

void SampleTableModel::overlay_pending_edits(std::vector<search::SampleRow>& rows) const {
    if (pending_edits_.empty()) return;
    for (search::SampleRow& row : rows) {
        const auto pending = pending_edits_.find(key_of(row));
        if (pending != pending_edits_.end() && pending->second.row) row = *pending->second.row;
    }
}

void SampleTableModel::emit_row_changed(int row) {
    emit dataChanged(index(row, 0), index(row, kColumnCount - 1));
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
    const auto rows = rows_of({key});
    if (rows.empty()) return std::nullopt;
    return rows.begin()->second;
}

std::map<SampleKey, int> SampleTableModel::rows_of(const std::vector<SampleKey>& keys) const {
    std::map<SampleKey, int> found;
    if (keys.empty()) return found;
    const std::set<SampleKey> wanted(keys.begin(), keys.end());
    for (const Page& page : pages_) {
        for (std::size_t offset = 0; offset < page.rows.size(); ++offset) {
            const SampleKey key = key_of(page.rows[offset]);
            if (wanted.contains(key)) found.emplace(key, page.first_row + static_cast<int>(offset));
        }
    }
    return found;
}

std::optional<SampleKey> SampleTableModel::key_at(int row) const {
    const int page_index = page_index_of_row(row);
    if (page_index < 0) return std::nullopt;
    const Page& page = pages_[static_cast<std::size_t>(page_index)];
    const auto offset = static_cast<std::size_t>(row - page.first_row);
    return offset < page.keys.size() ? std::optional(page.keys[offset]) : std::nullopt;
}

std::vector<search::SampleRow> SampleTableModel::drop_seen_rows(std::vector<search::SampleRow> rows,
                                                                std::set<SampleKey>& seen,
                                                                std::vector<SampleKey>& keys) {
    std::erase_if(rows, [&seen](const search::SampleRow& row) { return !seen.insert(key_of(row)).second; });
    keys.clear();
    keys.reserve(rows.size());
    for (const search::SampleRow& row : rows)
        keys.push_back(key_of(row));
    return rows;
}

search::SampleRow* SampleTableModel::loaded_row_for_update(int row) {
    const int page_index = page_index_of_row(row);
    if (page_index < 0) return nullptr;
    Page& page = pages_[static_cast<std::size_t>(page_index)];
    const auto offset = static_cast<std::size_t>(row - page.first_row);
    return offset < page.rows.size() ? &page.rows[offset] : nullptr;
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

void SampleTableModel::advance_generation() {
    ++generation_;
    generation_stop_.request_stop();  // queued requests of the old generation skip their work
    generation_stop_ = std::stop_source();
}

void SampleTableModel::restart() {
    advance_generation();
    reset_pending();
    refreshing_ = false;
    staged_pages_.clear();
    wanted_pages_.clear();
    replacing_ = true;
    append_pending_ = true;
    emit coverageChanged({});
    const std::uint64_t generation = generation_;
    set_pending(+1);
    source_.request_page(make_request(std::nullopt), scope_.library, generation_stop_.get_token(), this,
                         [this, generation](Result<search::SearchPage> result) {
                             if (generation != generation_) return;  // superseded by a newer query
                             set_pending(-1);
                             if (!result) {
                                 replace_rows(std::nullopt);
                                 report_failure(result.error(), 0);
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
    seen_keys_.clear();
    if (first_page && !first_page->rows.empty()) {
        Page page;
        page.next = std::move(first_page->next);
        page.rows = drop_seen_rows(std::move(first_page->rows), seen_keys_, page.keys);
        page.row_count = static_cast<int>(page.rows.size());
        overlay_pending_edits(page.rows);
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
    source_.request_page(make_request(start), scope_.library, generation_stop_.get_token(), this,
                         [this, generation, start](Result<search::SearchPage> result) {
                             if (generation != generation_) return;  // obsolete query
                             set_pending(-1);
                             append_pending_ = false;
                             if (!result) {
                                 report_failure(result.error(), std::max(0, rowCount() - 1));
                                 return;
                             }
                             report_coverage(result->coverage);
                             if (result->rows.empty()) {
                                 if (!pages_.empty()) pages_.back().next.reset();  // the result ends here
                                 return;
                             }
                             Page page;
                             page.start = start;
                             page.next = std::move(result->next);
                             page.first_row = rowCount();
                             // A row whose sort value changed across the cursor comes again: skip it.
                             page.rows = drop_seen_rows(std::move(result->rows), seen_keys_, page.keys);
                             page.row_count = static_cast<int>(page.rows.size());
                             if (page.row_count == 0) {
                                 // Only repeats: the chain goes on from this page's continuation.
                                 if (!pages_.empty()) pages_.back().next = std::move(page.next);
                                 return;
                             }
                             overlay_pending_edits(page.rows);
                             page.last_used = ++lru_clock_;
                             beginInsertRows({}, page.first_row, page.first_row + page.row_count - 1);
                             pages_.push_back(std::move(page));
                             endInsertRows();
                             evict_least_recently_used();
                         });
}

void SampleTableModel::request_staged_page() {
    const std::optional<search::Cursor> start = staged_pages_.empty() ? std::nullopt : staged_pages_.back().next;
    const std::uint64_t generation = generation_;
    set_pending(+1);
    source_.request_page(
        make_request(start), scope_.library, generation_stop_.get_token(), this,
        [this, generation, start](Result<search::SearchPage> result) {
            if (generation != generation_) return;
            set_pending(-1);
            if (!result) {
                refreshing_ = false;
                staged_pages_.clear();
                // The structure changed again while this refresh walked its fresh cursors: walk
                // again (the old rows stay on screen meanwhile).
                if (result.error().code == ErrorCode::StaleCursor) {
                    retry_stale_refresh();
                    return;
                }
                report_failure(result.error(), refresh_anchor_row_);
                return;
            }
            report_coverage(result->coverage);
            if (!result->rows.empty()) {
                Page page;
                page.start = start;
                page.next = std::move(result->next);
                page.first_row =
                    staged_pages_.empty() ? 0 : staged_pages_.back().first_row + staged_pages_.back().row_count;
                std::vector<search::SampleRow> rows = drop_seen_rows(std::move(result->rows), staged_keys_, page.keys);
                page.row_count = static_cast<int>(rows.size());
                const bool in_window = page.first_row < refresh_window_end_row_ &&
                                       page.first_row + page.row_count > refresh_window_first_row_;
                // Outside the anchor's window only the keys and cursors matter: the rows go now.
                if (in_window) {
                    page.rows = std::move(rows);
                    page.last_used = ++lru_clock_;
                }
                if (page.row_count > 0)
                    staged_pages_.push_back(std::move(page));
                else if (!staged_pages_.empty())
                    staged_pages_.back().next = std::move(page.next);  // only repeats: skip the page
            } else if (!staged_pages_.empty()) {
                staged_pages_.back().next.reset();
            }
            const bool more = !staged_pages_.empty() && staged_pages_.back().next &&
                              staged_pages_.back().first_row + staged_pages_.back().row_count < refresh_target_rows_ &&
                              static_cast<int>(staged_pages_.size()) < kMaxRefreshPages;
            if (more)
                request_staged_page();
            else
                commit_staged_pages();
        });
}

void SampleTableModel::retry_stale_refresh() {
    // 50 ms, 100 ms, 200 ms … capped: an import changing the structure every 150 ms is not raced.
    const int delay = std::min(kMaximumStaleRetryMilliseconds, 50 << std::min(stale_refresh_retries_, 5));
    ++stale_refresh_retries_;
    const std::uint64_t generation = generation_;
    const int anchor = refresh_anchor_row_;
    QTimer::singleShot(delay, this, [this, generation, anchor] {
        if (generation == generation_) refresh(anchor);
    });
}

void SampleTableModel::commit_staged_pages() {
    refreshing_ = false;
    stale_refresh_retries_ = 0;
    // Hydrations requested against the old page table must not land in the new one.
    advance_generation();
    reset_pending();
    std::vector<Page> staged = std::exchange(staged_pages_, {});
    seen_keys_ = std::exchange(staged_keys_, {});
    for (Page& page : staged)
        overlay_pending_edits(page.rows);
    const int old_rows = rowCount();
    const int new_rows = staged.empty() ? 0 : staged.back().first_row + staged.back().row_count;
    const int common_rows = std::min(old_rows, new_rows);

    // A shrinking result loses rows at the end first (ordinary row removal on the old data) …
    if (new_rows < old_rows) {
        beginRemoveRows({}, new_rows, old_rows - 1);
        truncate_pages(pages_, new_rows);
        endRemoveRows();
    }
    // … a growing one gains its extra rows after the layout change: they are split off here (only
    // the page straddling the boundary is copied; later pages are moved).
    std::optional<Page> boundary_page;
    std::vector<Page> extra_pages;
    if (new_rows > old_rows) {
        const auto boundary = std::ranges::find_if(
            staged, [common_rows](const Page& page) { return page.first_row + page.row_count > common_rows; });
        if (boundary != staged.end() && boundary->first_row < common_rows) boundary_page = *boundary;
        const auto first_extra = boundary_page ? std::next(boundary) : boundary;
        extra_pages.assign(std::make_move_iterator(first_extra), std::make_move_iterator(staged.end()));
        staged.erase(first_extra, staged.end());
    }

    // … and the overlapping rows are replaced as a layout change: persistent indexes (selection,
    // current row) follow their sample by key instead of staying on a row number, and the view keeps
    // its scroll position (a model reset would jump to the top).
    emit layoutAboutToBeChanged();
    // Keys are known for every row, loaded or not, so the whole selection follows its samples.
    std::map<SampleKey, int> new_row_of_key;
    for (const Page& page : staged)
        for (std::size_t offset = 0; offset < page.keys.size(); ++offset)
            if (page.first_row + static_cast<int>(offset) < common_rows)
                new_row_of_key.emplace(page.keys[offset], page.first_row + static_cast<int>(offset));
    const QModelIndexList persistent = persistentIndexList();
    QModelIndexList moved;
    for (const QModelIndex& old_index : persistent) {
        const std::optional<SampleKey> key = key_at(old_index.row());
        const auto found = key ? new_row_of_key.find(*key) : new_row_of_key.end();
        moved << (found == new_row_of_key.end() ? QModelIndex() : index(found->second, old_index.column()));
    }
    truncate_pages(staged, common_rows);
    pages_ = std::move(staged);
    changePersistentIndexList(persistent, moved);
    emit layoutChanged();
    if (new_rows > old_rows) {
        beginInsertRows({}, old_rows, new_rows - 1);
        if (boundary_page) pages_.back() = std::move(*boundary_page);
        for (Page& page : extra_pages)
            pages_.push_back(std::move(page));
        endInsertRows();
    }
    evict_least_recently_used();
    emit refreshed();
    request_count();
}

void SampleTableModel::truncate_pages(std::vector<Page>& pages, int rows) {
    while (!pages.empty() && pages.back().first_row >= rows)
        pages.pop_back();
    if (pages.empty()) return;
    Page& last = pages.back();
    const int kept = rows - last.first_row;
    if (kept < last.row_count) {
        last.row_count = kept;
        if (static_cast<int>(last.rows.size()) > kept) last.rows.resize(static_cast<std::size_t>(kept));
        last.keys.resize(static_cast<std::size_t>(kept));
        last.next.reset();  // the following rows are gone; a later fetch re-reads from a full refresh
    }
}

void SampleTableModel::hydrate_wanted_pages() {
    const std::set<int> wanted = std::exchange(wanted_pages_, {});
    if (refreshing_ || replacing_) return;  // the running refresh or query replaces every page
    for (int page_index : wanted) {
        if (page_index < 0 || static_cast<std::size_t>(page_index) >= pages_.size()) continue;
        const Page& page = pages_[static_cast<std::size_t>(page_index)];
        if (page.rows.empty() && !page.hydrating) request_hydration(page_index);
    }
}

void SampleTableModel::request_hydration(int page_index) {
    Page& page = pages_[static_cast<std::size_t>(page_index)];
    page.hydrating = true;
    const std::uint64_t generation = generation_;
    source_.request_page(make_request(page.start), scope_.library, generation_stop_.get_token(), this,
                         [this, generation, page_index](Result<search::SearchPage> result) {
                             if (generation != generation_) return;
                             if (!result) {
                                 on_hydration_failed(page_index, result.error());
                                 return;
                             }
                             on_page_hydrated(page_index, std::move(*result));
                         });
}

void SampleTableModel::on_hydration_failed(int page_index, const Error& error) {
    if (page_index < 0 || static_cast<std::size_t>(page_index) >= pages_.size()) return;
    Page& page = pages_[static_cast<std::size_t>(page_index)];
    page.hydrating = false;
    if (error.code == ErrorCode::StaleCursor) {
        // The page's cursor predates a structure change: re-read the table around this page.
        report_failure(error, page.first_row);
        return;
    }
    ++page.failed_reads;
    // 1 s, 2 s, 4 s … capped: a failing source is not hammered from every repaint.
    const int delay =
        std::min(kMaximumRetryMilliseconds, kMinimumRetryMilliseconds << std::min(page.failed_reads - 1, 5));
    page.retry_after = QDeadlineTimer(delay);
    const std::uint64_t generation = generation_;
    QTimer::singleShot(delay, this, [this, generation, page_index] {
        if (generation != generation_ || static_cast<std::size_t>(page_index) >= pages_.size()) return;
        const Page& retried = pages_[static_cast<std::size_t>(page_index)];
        if (retried.row_count > 0)
            emit dataChanged(index(retried.first_row, 0),
                             index(retried.first_row + retried.row_count - 1, kColumnCount - 1));
    });
    if (page.failed_reads == 1) report_failure(error, page.first_row);  // say it once, not on every retry
}

void SampleTableModel::on_page_hydrated(int page_index, search::SearchPage page) {
    if (page_index < 0 || static_cast<std::size_t>(page_index) >= pages_.size()) return;
    Page& target = pages_[static_cast<std::size_t>(page_index)];
    target.hydrating = false;
    target.failed_reads = 0;
    // The page holds exactly the samples it held before (rows skipped as repeats stay skipped);
    // anything else means rows were added, removed or moved since: re-read around it.
    const std::set<SampleKey> expected(target.keys.begin(), target.keys.end());
    std::erase_if(page.rows, [&expected](const search::SampleRow& row) { return !expected.contains(key_of(row)); });
    bool result_changed = static_cast<int>(page.rows.size()) != target.row_count;
    for (std::size_t offset = 0; !result_changed && offset < page.rows.size(); ++offset)
        result_changed = key_of(page.rows[offset]) != target.keys[offset];
    if (result_changed) {
        target.last_used = ++lru_clock_;
        refresh(target.first_row);
        return;
    }
    target.rows = std::move(page.rows);
    overlay_pending_edits(target.rows);
    target.last_used = ++lru_clock_;
    const int first_row = target.first_row;
    if (target.row_count > 0)
        emit dataChanged(index(first_row, 0), index(first_row + target.row_count - 1, kColumnCount - 1));
    evict_least_recently_used();
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
    source_.request_count(make_request(std::nullopt), scope_.library, generation_stop_.get_token(), this,
                          [this, generation](Result<search::CountResult> result) {
                              if (generation != generation_) return;
                              set_pending(-1);
                              if (!result) {
                                  report_failure(result.error(), 0);
                                  return;
                              }
                              const bool complete = std::ranges::all_of(
                                  result->coverage, [](const auto& library) { return library.complete; });
                              emit countChanged(result->total, complete);
                          });
}

void SampleTableModel::report_failure(const Error& error, int anchor_row) {
    if (error.code == ErrorCode::StaleCursor) {
        // The result's structure changed under a cursor (rows added or removed): an anchored
        // refresh keeps every loaded row and the scroll position. A refresh already running
        // covers it.
        if (!refreshing_) refresh(anchor_row);
        return;
    }
    if (error.code == ErrorCode::Cancelled) return;  // an obsolete request skipped its work
    emit queryFailed(DisplayFormat::error(error));
}

void SampleTableModel::report_coverage(const std::vector<search::LibraryCoverage>& coverage) {
    QStringList missing;
    for (const auto& library : coverage) {
        if (library.complete) continue;
        missing << (library.error ? tr("%1 (%2)").arg(DisplayFormat::from_utf8(library.name),
                                                      DisplayFormat::error(*library.error))
                                  : DisplayFormat::from_utf8(library.name));
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

void SampleTableModel::reset_pending() {
    set_pending(-pending_requests_);
}

}  // namespace mso::ui
