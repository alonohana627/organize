#include "mso/ui/sample_row_delegate.hpp"

#include <QApplication>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

#include "mso/ui/sample_table_model.hpp"
#include "mso/ui/sample_table_view.hpp"
#include "mso/ui/star_rating.hpp"

namespace mso::ui {

namespace {

/// Opacity of the hover tint (a fraction of the highlight color), subtle like Explorer's.
constexpr float kHoverTintOpacity = 0.14f;
constexpr int kRatingPadding = 4;

bool is_rating_column(const QModelIndex& index) {
    return index.column() == static_cast<int>(SampleTableModel::Column::Rating);
}

QPalette::ColorGroup color_group(const QStyleOptionViewItem& option) {
    if (!(option.state & QStyle::State_Enabled)) return QPalette::Disabled;
    return (option.state & QStyle::State_Active) ? QPalette::Active : QPalette::Inactive;
}

}  // namespace

SampleRowDelegate::SampleRowDelegate(SampleTableView& view) : QStyledItemDelegate(&view), view_(view) {}

QRect SampleRowDelegate::rating_area(const QStyleOptionViewItem& option) {
    return option.rect.adjusted(kRatingPadding, 0, -kRatingPadding, 0);
}

int SampleRowDelegate::rating_star_size(const QStyleOptionViewItem& option) {
    return std::clamp(std::min(option.fontMetrics.height(), option.rect.height() - 4), 8, 32);
}

void SampleRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& base_option,
                              const QModelIndex& index) const {
    QStyleOptionViewItem option = base_option;
    initStyleOption(&option, index);
    const QPalette::ColorGroup group = color_group(option);
    const bool selected = option.state & QStyle::State_Selected;

    painter->save();
    paint_background(*painter, option, index, group);

    // The background is ours; the style draws only text and icons, never its per-cell panels.
    option.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver | QStyle::State_HasFocus);
    option.features &= ~QStyleOptionViewItem::Alternate;
    if (selected) {
        const QColor text = option.palette.color(group, QPalette::HighlightedText);
        option.palette.setColor(QPalette::Text, text);
        option.palette.setColor(QPalette::WindowText, text);
    }

    if (is_rating_column(index) && !index.data(SampleTableModel::LoadingRole).toBool()) {
        const QRect area = rating_area(option);
        const int star_size = rating_star_size(option);
        StarRating::Appearance appearance;
        appearance.rating = index.data(SampleTableModel::RatingRole).toInt();
        appearance.on_selection = selected;
        appearance.enabled = option.state & QStyle::State_Enabled;
        if (index.row() == view_.hovered_row())
            appearance.preview = StarRating::star_at(area, star_size, view_.hover_position());
        StarRating::paint(*painter, area, star_size, appearance, option.palette, group);
    } else {
        const QWidget* widget = option.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &option, painter, widget);
    }

    paint_focus_outline(*painter, option, index, group);
    painter->restore();
}

void SampleRowDelegate::paint_background(QPainter& painter, const QStyleOptionViewItem& option,
                                         const QModelIndex& index, QPalette::ColorGroup group) const {
    // Each cell fills its whole rectangle, so adjacent cells join into one continuous band.
    if (option.state & QStyle::State_Selected) {
        painter.fillRect(option.rect, option.palette.color(group, QPalette::Highlight));
    } else if (index.row() == view_.hovered_row()) {
        QColor tint = option.palette.color(group, QPalette::Highlight);
        tint.setAlphaF(kHoverTintOpacity);
        painter.fillRect(option.rect, tint);
    }
}

void SampleRowDelegate::paint_focus_outline(QPainter& painter, const QStyleOptionViewItem& option,
                                            const QModelIndex& index, QPalette::ColorGroup group) const {
    // One outline around the whole current row (visible keyboard focus), drawn piecewise: top and
    // bottom edges in every cell, the side edges only in the first and last visible columns.
    if (!view_.hasFocus() || index.row() != view_.currentIndex().row()) return;
    const QHeaderView* header = view_.horizontalHeader();
    int first_visual = -1;
    int last_visual = -1;
    for (int visual = 0; visual < header->count(); ++visual) {
        if (header->isSectionHidden(header->logicalIndex(visual))) continue;
        if (first_visual < 0) first_visual = visual;
        last_visual = visual;
    }
    const int visual = header->visualIndex(index.column());
    const bool selected = view_.selectionModel() && view_.selectionModel()->isRowSelected(index.row(), index.parent());
    QColor color = option.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Highlight);
    color.setAlphaF(selected ? 0.6f : 1.0f);
    painter.setPen(QPen(color, 1, Qt::DotLine));
    const QRect cell = option.rect.adjusted(0, 0, -1, -1);
    painter.drawLine(cell.topLeft(), cell.topRight());
    painter.drawLine(cell.bottomLeft(), cell.bottomRight());
    if (visual == first_visual) painter.drawLine(cell.topLeft(), cell.bottomLeft());
    if (visual == last_visual) painter.drawLine(cell.topRight(), cell.bottomRight());
}

bool SampleRowDelegate::editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                                    const QModelIndex& index) {
    if (!is_rating_column(index) || index.data(SampleTableModel::LoadingRole).toBool())
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    if (event->type() == QEvent::MouseButtonRelease) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const int star =
            StarRating::star_at(rating_area(option), rating_star_size(option), mouse->position().toPoint());
        if (mouse->button() == Qt::LeftButton && star > 0) {
            const int current = index.data(SampleTableModel::RatingRole).toInt();
            emit ratingClicked(index, star == current ? 0 : star);  // clicking the current rating clears it
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonDblClick) {
        // A quick second click on a star is another rating click, not "play this sample".
        const auto* mouse = static_cast<QMouseEvent*>(event);
        return StarRating::star_at(rating_area(option), rating_star_size(option), mouse->position().toPoint()) > 0;
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

}  // namespace mso::ui
