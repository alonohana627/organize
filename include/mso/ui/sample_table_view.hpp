#pragma once
/// @file
/// SampleTableView: the central sample table — row selection by key, column visibility menu,
/// and drag-and-drop of resolved local files to DAWs.
#include <QTableView>

#include <cstddef>
#include <optional>
#include <vector>

#include "mso/search/search_service.hpp"
#include "mso/ui/sample_key.hpp"

namespace mso::ui {

class DragPathResolver;
class RootLocations;
class SampleRowDelegate;
class SampleTableModel;

/// Table view over SampleTableModel. Drags carry file URLs of resolved local paths only; a
/// selection that contains unavailable samples is refused (never silently trimmed) and the
/// window offers to select the available ones instead.
class SampleTableView final : public QTableView {
    Q_OBJECT

public:
    explicit SampleTableView(QWidget* parent = nullptr);

    /// Sets the model; must be a SampleTableModel.
    void set_sample_model(SampleTableModel* model);
    /// Background file check consulted at drag start (optional).
    void set_drag_resolver(DragPathResolver* resolver) { resolver_ = resolver; }
    /// Root base paths used to build drag URLs (required for dragging).
    void set_root_locations(const RootLocations* locations) { root_locations_ = locations; }

    /// Copies of the selected rows whose data is loaded, in row order (at most 2,000 exist).
    std::vector<search::SampleRow> selected_samples() const;
    /// Keys of the selected rows whose data is loaded, in row order.
    std::vector<SampleKey> selected_keys() const;
    /// Number of selected rows, loaded or not.
    std::size_t selected_row_count() const;
    /// Key of the current (focused) row, when loaded.
    std::optional<SampleKey> current_key() const;
    /// Selects exactly the given keys among the loaded rows; returns how many were found.
    std::size_t select_keys(const std::vector<SampleKey>& keys, const std::optional<SampleKey>& current);
    /// First row visible at the top of the viewport (0 when empty).
    int first_visible_row() const;
    /// Row under the mouse pointer, or -1 (drives the row delegate's hover tint).
    int hovered_row() const { return hovered_row_; }
    /// Pointer position in viewport coordinates while hovered_row() is valid.
    QPoint hover_position() const { return hover_position_; }

signals:
    /// A drag could not start; `reason` explains why (shown in the status bar).
    void dragRefused(const QString& reason);
    /// The selection mixes available and unavailable samples, so the drag was refused. `available`
    /// lists the samples that could be dragged. Emitted from inside startDrag(): receivers must
    /// defer any dialog (a queued call) instead of opening it while the drag is being set up.
    void mixedDragRefused(const std::vector<mso::ui::SampleKey>& available, std::size_t unavailable_count);
    /// A star in the rating column was clicked: set `rating` (0 clears) on that one sample.
    void ratingClicked(const mso::ui::SampleKey& key, int rating);
    /// Right-click: show the sample context menu at `global_position` for the (now) selected rows.
    /// `row` is the clicked row, or -1 for empty space.
    void sampleMenuRequested(const QPoint& global_position, int row);

protected:
    void startDrag(Qt::DropActions supported_actions) override;
    bool viewportEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void currentChanged(const QModelIndex& current, const QModelIndex& previous) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    /// Header context menu with one checkable entry per column.
    void show_column_menu(const QPoint& position);
    /// Tracks the hovered row and repaints what changed.
    void set_hover(int row, const QPoint& position);
    /// Schedules a repaint of one whole row (no-op for -1).
    void update_row(int row);

    /// The model (owned by the window).
    SampleTableModel* sample_model_ = nullptr;
    /// Background file check for drags (optional).
    DragPathResolver* resolver_ = nullptr;
    /// Root base paths for drag URLs.
    const RootLocations* root_locations_ = nullptr;
    /// Paints rows and handles star clicks.
    SampleRowDelegate* row_delegate_ = nullptr;
    /// Row under the pointer, or -1.
    int hovered_row_ = -1;
    /// Pointer position in viewport coordinates.
    QPoint hover_position_;
};

}  // namespace mso::ui
