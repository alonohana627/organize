#pragma once
/// @file
/// SampleRowDelegate: paints sample rows the way Windows File Explorer does — one continuous
/// full-row selection band and hover tint, no per-cell boxes — and makes the rating column's
/// stars clickable without an editor.
#include <QStyledItemDelegate>

namespace mso::ui {

class SampleTableView;

/// Row-oriented delegate for SampleTableView.
///
/// The delegate paints the row background itself (selection band, subtle hover tint, a single
/// keyboard-focus outline around the current row) and removes State_Selected, State_MouseOver and
/// State_HasFocus before the style draws the cell text, so the platform style can no longer box
/// each cell. Colors come from the palette, so light and dark themes both work.
class SampleRowDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit SampleRowDelegate(SampleTableView& view);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;

    /// Area of the star strip inside a rating cell (painting and hit-testing share it).
    static QRect rating_area(const QStyleOptionViewItem& option);
    /// Star size for a rating cell: the font height, limited by the row height.
    static int rating_star_size(const QStyleOptionViewItem& option);

signals:
    /// A star in the rating column was clicked: set `rating` (0 clears) on that row's sample.
    void ratingClicked(const QModelIndex& index, int rating);

private:
    void paint_background(QPainter& painter, const QStyleOptionViewItem& option, const QModelIndex& index,
                          QPalette::ColorGroup group) const;
    void paint_focus_outline(QPainter& painter, const QStyleOptionViewItem& option, const QModelIndex& index,
                             QPalette::ColorGroup group) const;

    SampleTableView& view_;
};

}  // namespace mso::ui
