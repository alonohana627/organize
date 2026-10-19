#pragma once
/// @file
/// SampleContextMenu: the right-click menu of the sample table and its file actions (show in the
/// file manager, copy paths, copy files).
#include <QObject>
#include <QPoint>
#include <QString>

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include "mso/search/search_service.hpp"
#include "mso/ui/sample_key.hpp"

class QAction;
class QMenu;
class QWidget;

namespace mso::ui {

class ApplicationSession;

/// Window commands the menu reuses, so it shows their shortcuts and runs the same code.
struct SampleMenuCommands {
    /// Play / pause.
    QAction* play = nullptr;
    std::array<QAction*, 6> ratings{};  ///< "Clear rating" and 1–5 stars
    QAction* toggle_favorite = nullptr;
    /// Remove from library.
    QAction* remove_from_library = nullptr;
    /// Analysis commands.
    QAction* analyze_waveforms = nullptr;
    QAction* analyze_musical = nullptr;
    QAction* analyze_duplicates = nullptr;
};

/// Builds and runs the sample context menu. File actions resolve playable paths off the UI thread
/// and act only when every targeted sample resolved, so nothing is done for part of a selection
/// without saying so (docs/01 §5.6); they are disabled up front for samples known to be unavailable
/// and when some selected rows are not loaded. The menu's actions capture keys (values) only, so
/// the menu's event loop may change the table freely.
class SampleContextMenu final : public QObject {
    Q_OBJECT

public:
    /// Menus open over `parent_widget`; `session` and the command actions must outlive this object.
    SampleContextMenu(ApplicationSession& session, SampleMenuCommands commands, QWidget* parent_widget);

    /// Shows the menu for `selection` (the loaded selected rows; `total_selected` counts loaded and
    /// unloaded ones) at `global_position`; `clicked` is the row under the pointer (or the current
    /// row, from the keyboard).
    void show(const QPoint& global_position, const std::vector<search::SampleRow>& selection,
              std::size_t total_selected, search::SampleRow clicked);
    /// The menu show() runs, built for the same arguments (tests inspect it without opening it).
    std::unique_ptr<QMenu> build_menu(const std::vector<search::SampleRow>& selection, std::size_t total_selected,
                                      const search::SampleRow& clicked);

    /// Opens the file manager at `key`'s file.
    void reveal(const SampleKey& key);
    /// Puts the native paths of `keys` on the clipboard, one per line.
    void copy_paths(const std::vector<SampleKey>& keys);
    /// Puts the files of `keys` on the clipboard (pasting in Explorer or a DAW copies the files).
    void copy_files(const std::vector<SampleKey>& keys);

signals:
    /// One-line feedback for the status bar.
    void statusMessage(const QString& message);

private:
    /// Resolves `keys` off the UI thread; `use` runs only when every one resolved.
    void resolve_paths(const std::vector<SampleKey>& keys, std::function<void(std::vector<std::filesystem::path>)> use);

    /// Resolves paths through the Application.
    ApplicationSession& session_;
    /// Window commands shown in the menu.
    SampleMenuCommands commands_;
    /// Parent of the menu.
    QWidget* parent_widget_;
};

}  // namespace mso::ui
