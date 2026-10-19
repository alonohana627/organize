#pragma once
/// @file
/// HistoryPanel: the durable undo/redo history of one library (R16).
#include <QString>
#include <QTimer>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "mso/catalog/types.hpp"
#include "mso/domain/core.hpp"

class QLabel;
class QListWidget;
class QPushButton;

namespace mso::ui {

class ApplicationSession;

/// History dock. Shows the library of the current selection (or navigator node); Undo and Redo
/// are forwarded to the main window, which routes them to the same library.
///
/// The list is read on demand only: when the shown library changes, when this window's own
/// edit, undo or redo settles (reload()), and — for changes made elsewhere, such as the command
/// line — at most once per kBackgroundReloadMilliseconds while the panel is visible
/// (catalog_changed()); a hidden panel only notes that it is stale and reads when it is shown.
/// A read whose entries equal the shown ones changes nothing, so the scroll position and the
/// current entry stay put.
class HistoryPanel final : public QWidget {
    Q_OBJECT

public:
    /// Most recent actions listed.
    static constexpr std::size_t kVisibleEntries = 100;
    /// Shortest spacing of reads caused by changes made elsewhere.
    static constexpr int kBackgroundReloadMilliseconds = 2000;

    /// A panel reading history through `session`, which must outlive it.
    explicit HistoryPanel(ApplicationSession& session, QWidget* parent = nullptr);

    /// Switches to another library (nullopt: nothing to show) and reloads.
    void show_library(const std::optional<uuids::uuid>& library);
    /// Re-reads the history of the shown library now (an edit, undo or redo of it settled).
    void reload();
    /// The catalog of `library` changed in a way that may have added history (made elsewhere):
    /// re-read soon if it is the shown library and the panel is visible, else when it is shown.
    void catalog_changed(const uuids::uuid& library);
    /// The library whose history is shown.
    const std::optional<uuids::uuid>& library() const { return library_; }
    /// History reads started since construction (tests check that events do not each read).
    std::uint64_t reads_started() const { return reads_started_; }

signals:
    /// Undo the latest action of the shown library.
    void undoRequested();
    /// Redo the latest undone action of the shown library.
    void redoRequested();

protected:
    void showEvent(QShowEvent* event) override;

private:
    /// Shows the entries, rebuilding the list only when they differ from the shown ones.
    void show_entries(const std::vector<catalog::HistoryEntry>& entries);
    /// Enables Undo and Redo from the shown entries.
    void update_buttons();

    /// Reads the history through the Application.
    ApplicationSession& session_;
    /// Library whose history is shown.
    std::optional<uuids::uuid> library_;
    /// Drops stale history deliveries.
    std::uint64_t request_ = 0;
    /// Reads started (see reads_started()).
    std::uint64_t reads_started_ = 0;
    /// The history may have changed while the panel was hidden: read when shown.
    bool stale_ = false;
    /// Spaces reads caused by changes made elsewhere.
    QTimer background_reload_timer_;
    /// Entries shown (newest first); empty when none could be read.
    std::vector<catalog::HistoryEntry> entries_;
    /// Widgets (owned through Qt's parent tree).
    QLabel* title_label_ = nullptr;
    QListWidget* list_ = nullptr;         ///< history entries, newest first
    QPushButton* undo_button_ = nullptr;  ///< undoes the cursor entry
    QPushButton* redo_button_ = nullptr;  ///< redoes the newest undone entry
};

}  // namespace mso::ui
