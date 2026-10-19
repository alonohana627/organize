#pragma once
/// @file
/// HistoryPanel: the durable undo/redo history of one library (R16).
#include <QString>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <optional>

#include "mso/domain/core.hpp"

class QLabel;
class QListWidget;
class QPushButton;

namespace mso::ui {

class ApplicationSession;

/// History dock. Shows the library of the current selection (or navigator node); Undo and Redo
/// are forwarded to the main window, which routes them to the same library.
class HistoryPanel final : public QWidget {
    Q_OBJECT

public:
    /// Most recent actions listed.
    static constexpr std::size_t kVisibleEntries = 100;

    explicit HistoryPanel(ApplicationSession& session, QWidget* parent = nullptr);

    /// Switches to another library (nullopt: nothing to show) and reloads.
    void show_library(const std::optional<uuids::uuid>& library);
    /// Re-reads the history of the shown library.
    void reload();
    const std::optional<uuids::uuid>& library() const { return library_; }

signals:
    /// Undo the latest action of the shown library.
    void undoRequested();
    /// Redo the latest undone action of the shown library.
    void redoRequested();

private:
    /// Reads the history through the Application.
    ApplicationSession& session_;
    /// Library whose history is shown.
    std::optional<uuids::uuid> library_;
    /// Drops stale history deliveries.
    std::uint64_t request_ = 0;
    /// Widgets (owned through Qt's parent tree).
    QLabel* title_label_ = nullptr;
    QListWidget* list_ = nullptr;
    QPushButton* undo_button_ = nullptr;
    QPushButton* redo_button_ = nullptr;
};

}  // namespace mso::ui
