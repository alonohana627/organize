#include "mso/ui/sample_table_view.hpp"

#include <QDrag>
#include <QFocusEvent>
#include <QHeaderView>
#include <QHoverEvent>
#include <QItemSelectionModel>
#include <QList>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QUrl>

#include <algorithm>
#include <utility>

#include "mso/ui/drag_path_resolver.hpp"
#include "mso/ui/sample_row_delegate.hpp"
#include "mso/ui/sample_table_model.hpp"

namespace mso::ui {

SampleTableView::SampleTableView(QWidget* parent) : QTableView(parent) {
    setAccessibleName(tr("Samples"));
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setDefaultDropAction(Qt::CopyAction);
    setSortingEnabled(true);
    setWordWrap(false);
    // Explorer-like rows: no grid, no zebra stripes; SampleRowDelegate paints one band per row.
    setAlternatingRowColors(false);
    setShowGrid(false);
    setTabKeyNavigation(false);
    setMouseTracking(true);
    row_delegate_ = new SampleRowDelegate(*this);
    setItemDelegate(row_delegate_);
    connect(row_delegate_, &SampleRowDelegate::ratingClicked, this, [this](const QModelIndex& index, int rating) {
        if (!sample_model_) return;
        if (const auto* sample = sample_model_->loaded_row(index.row()))
            emit ratingClicked(SampleKey{sample->library, sample->sample_id}, rating);
    });
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        const int row = rowAt(position.y());
        if (row >= 0 && selectionModel() && !selectionModel()->isRowSelected(row, {})) {
            // Like Explorer: right-clicking an unselected row makes it the selection.
            setCurrentIndex(model()->index(row, std::max(0, columnAt(position.x()))));
            selectRow(row);
        }
        emit sampleMenuRequested(viewport()->mapToGlobal(position), row);
    });
    // Uniform row heights keep scrolling cost independent of the result size.
    verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    verticalHeader()->hide();

    QHeaderView* header = horizontalHeader();
    header->setSectionsMovable(true);
    header->setHighlightSections(false);
    header->setStretchLastSection(false);
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header, &QHeaderView::customContextMenuRequested, this, &SampleTableView::show_column_menu);
}

void SampleTableView::set_sample_model(SampleTableModel* model) {
    sample_model_ = model;
    setModel(model);
    horizontalHeader()->setSortIndicator(model->sort_column(),
                                         model->sort_descending() ? Qt::DescendingOrder : Qt::AscendingOrder);
    // A header click on a column the search service cannot sort puts the indicator back.
    connect(model, &SampleTableModel::sortRejected, this, [this] {
        QHeaderView* header = horizontalHeader();
        const QSignalBlocker blocker(header);
        header->setSortIndicator(sample_model_->sort_column(),
                                 sample_model_->sort_descending() ? Qt::DescendingOrder : Qt::AscendingOrder);
    });
    using Column = SampleTableModel::Column;
    const auto width = [this](Column column, int pixels) { setColumnWidth(static_cast<int>(column), pixels); };
    width(Column::Name, 280);
    width(Column::Location, 320);
    width(Column::Tags, 180);
    width(Column::Rating, 90);
    width(Column::Favorite, 70);
    for (Column hidden : {Column::Channels, Column::BitDepth, Column::Analyzed, Column::Added})
        setColumnHidden(static_cast<int>(hidden), true);
}

std::vector<search::SampleRow> SampleTableView::selected_samples() const {
    std::vector<search::SampleRow> samples;
    if (!sample_model_ || !selectionModel()) return samples;
    // Walk ranges rather than selectedRows(): a huge selection must not materialize every index.
    std::vector<std::pair<int, int>> ranges;
    for (const QItemSelectionRange& range : selectionModel()->selection())
        ranges.emplace_back(range.top(), range.bottom());
    std::sort(ranges.begin(), ranges.end());
    int previous_bottom = -1;
    for (auto [top, bottom] : ranges) {
        for (int row = std::max(top, previous_bottom + 1); row <= bottom; ++row)
            if (const auto* sample = sample_model_->loaded_row(row)) samples.push_back(*sample);
        previous_bottom = std::max(previous_bottom, bottom);
    }
    return samples;
}

std::vector<SampleKey> SampleTableView::selected_keys() const {
    std::vector<SampleKey> keys;
    for (const auto& sample : selected_samples())
        keys.push_back({sample.library, sample.sample_id});
    return keys;
}

std::size_t SampleTableView::selected_row_count() const {
    if (!selectionModel()) return 0;
    std::vector<std::pair<int, int>> ranges;
    for (const QItemSelectionRange& range : selectionModel()->selection())
        ranges.emplace_back(range.top(), range.bottom());
    std::sort(ranges.begin(), ranges.end());
    std::size_t count = 0;
    int previous_bottom = -1;
    for (auto [top, bottom] : ranges) {
        const int first = std::max(top, previous_bottom + 1);
        if (bottom >= first) count += static_cast<std::size_t>(bottom - first + 1);
        previous_bottom = std::max(previous_bottom, bottom);
    }
    return count;
}

std::optional<SampleKey> SampleTableView::current_key() const {
    if (!sample_model_ || !currentIndex().isValid()) return std::nullopt;
    if (const auto* sample = sample_model_->loaded_row(currentIndex().row()))
        return SampleKey{sample->library, sample->sample_id};
    return std::nullopt;
}

std::size_t SampleTableView::select_keys(const std::vector<SampleKey>& keys, const std::optional<SampleKey>& current) {
    if (!sample_model_ || !selectionModel()) return 0;
    QItemSelection selection;
    std::size_t found = 0;
    for (const SampleKey& key : keys) {
        if (auto row = sample_model_->row_of(key)) {
            selection.select(sample_model_->index(*row, 0),
                             sample_model_->index(*row, SampleTableModel::kColumnCount - 1));
            ++found;
        }
    }
    const auto flags = QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows;
    selectionModel()->select(selection, flags);
    if (current) {
        if (auto row = sample_model_->row_of(*current))
            selectionModel()->setCurrentIndex(
                sample_model_->index(*row, currentIndex().isValid() ? currentIndex().column() : 0),
                QItemSelectionModel::NoUpdate);
    }
    return found;
}

int SampleTableView::first_visible_row() const {
    return std::max(0, rowAt(0));
}

bool SampleTableView::viewportEvent(QEvent* event) {
    switch (event->type()) {
        case QEvent::MouseMove:
        case QEvent::HoverMove: {
            const QPoint position = event->type() == QEvent::MouseMove
                                        ? static_cast<QMouseEvent*>(event)->position().toPoint()
                                        : static_cast<QHoverEvent*>(event)->position().toPoint();
            set_hover(rowAt(position.y()), position);
            break;
        }
        case QEvent::Leave:
        case QEvent::HoverLeave:
            set_hover(-1, {});
            break;
        default:
            break;
    }
    return QTableView::viewportEvent(event);
}

void SampleTableView::set_hover(int row, const QPoint& position) {
    const bool row_changed = row != hovered_row_;
    const int previous_row = hovered_row_;
    hovered_row_ = row;
    hover_position_ = position;
    if (row_changed) update_row(previous_row);
    // Within a row only the rating column reacts to the pointer (star preview).
    if (row_changed || columnAt(position.x()) == static_cast<int>(SampleTableModel::Column::Rating)) update_row(row);
}

void SampleTableView::update_row(int row) {
    if (row < 0) return;
    viewport()->update(QRect(0, rowViewportPosition(row), viewport()->width(), rowHeight(row)));
}

void SampleTableView::currentChanged(const QModelIndex& current, const QModelIndex& previous) {
    QTableView::currentChanged(current, previous);
    // The focus outline spans the whole row, so both rows repaint entirely.
    update_row(previous.row());
    update_row(current.row());
}

void SampleTableView::focusInEvent(QFocusEvent* event) {
    QTableView::focusInEvent(event);
    update_row(currentIndex().row());
}

void SampleTableView::focusOutEvent(QFocusEvent* event) {
    QTableView::focusOutEvent(event);
    update_row(currentIndex().row());
}

void SampleTableView::startDrag(Qt::DropActions supported_actions) {
    if (!(supported_actions & Qt::CopyAction)) return;
    const std::vector<SampleKey> keys = selected_keys();
    if (keys.empty()) return;
    if (keys.size() < selected_row_count()) {
        emit dragRefused(tr("Some selected rows are not loaded; scroll them into view or select fewer samples."));
        return;
    }
    if (keys.size() > DragPathResolver::kMaxDragSamples) {
        emit dragRefused(tr("Drag at most %1 samples at a time.").arg(DragPathResolver::kMaxDragSamples));
        return;
    }
    if (!resolver_ || !resolver_->is_ready_for(keys)) {
        emit dragRefused(tr("Checking file locations… try dragging again in a moment."));
        return;
    }
    const std::vector<SampleKey> unavailable = resolver_->unavailable_keys();
    if (!unavailable.empty()) {
        emit mixedDragRefused(keys.size() - unavailable.size(), unavailable.size());
        return;
    }
    QList<QUrl> urls;
    for (const auto& path : resolver_->available_paths())
        urls << QUrl::fromLocalFile(QString::fromStdU16String(path.generic_u16string()));
    auto* mime = new QMimeData;
    mime->setUrls(urls);
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->exec(Qt::CopyAction, Qt::CopyAction);
}

void SampleTableView::show_column_menu(const QPoint& position) {
    if (!model()) return;
    QMenu menu(this);
    for (int column = 0; column < model()->columnCount(); ++column) {
        QAction* action = menu.addAction(model()->headerData(column, Qt::Horizontal).toString());
        action->setCheckable(true);
        action->setChecked(!isColumnHidden(column));
        // The name column identifies the row and cannot be hidden.
        action->setEnabled(column != static_cast<int>(SampleTableModel::Column::Name));
        connect(action, &QAction::toggled, this, [this, column](bool visible) { setColumnHidden(column, !visible); });
    }
    menu.exec(horizontalHeader()->viewport()->mapToGlobal(position));
}

}  // namespace mso::ui
