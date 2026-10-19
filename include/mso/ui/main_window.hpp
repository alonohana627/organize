#pragma once
/// @file
/// MainWindow: the desktop workspace — docks, central sample table, menus and keyboard commands —
/// wiring the panels and controllers to the ApplicationSession.
#include <QByteArray>
#include <QMainWindow>
#include <QString>
#include <QTimer>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mso/app/application.hpp"
#include "mso/catalog/edit_command.hpp"
#include "mso/ui/layout_preset_store.hpp"
#include "mso/ui/navigation_scope.hpp"
#include "mso/ui/sample_key.hpp"
#include "mso/ui/shortcut_catalog.hpp"

class QAction;
class QDockWidget;
class QLabel;
class QMenu;
class QStackedWidget;

namespace mso::ui {

class ApplicationSession;
class DragPathResolver;
class EditController;
class EmptyStateWidget;
class ExportController;
class HistoryPanel;
class InspectorPanel;
class JobsPanel;
class LibraryController;
class NavigatorPanel;
class NotificationBar;
class PreviewPanel;
class RootLocations;
class SampleContextMenu;
class SampleTableModel;
class SampleTableView;
class SearchBar;

/// Main window of Music Sample Organizer.
///
/// The window builds the widgets and routes signals; the work lives in controllers: edits and
/// undo in EditController (optimistic, behind the Application's edit API), library and folder
/// commands in LibraryController, export and backup in ExportController, the sample menu in
/// SampleContextMenu, layout presets in LayoutPresetStore. Catalog change events refresh only the
/// rows they name; the result is re-queried only on a full reload and the navigator rebuilt only on
/// a structure change. Single-key shortcuts (Space, Delete, F, 0–5 …) belong to the sample table;
/// only modifier shortcuts are window-wide.
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    /// A window over `session`, which must outlive it.
    explicit MainWindow(ApplicationSession& session, QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Applies the preferences the session preloaded (layout, shortcuts, preview volume). Call
    /// before show(), so the window appears in its saved place and shape without jumping.
    void restore_saved_state();

    /// The sample table (tests and the screenshot tool drive it).
    SampleTableView* sample_table() const { return table_; }
    /// The notification strip (tests read its message).
    NotificationBar* notification_bar() const { return notification_bar_; }

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    // ------------------------------------------------------------ construction
    void build_central();
    /// Creates the panels and their docks.
    void build_docks();
    /// Creates the commands (customizable shortcuts, analysis actions).
    void build_actions();
    /// Fills the menu bar from the commands.
    void build_menus();
    /// Adds the catalog, result count and selection labels.
    void build_status_bar();
    /// Creates the controllers and routes panel signals to them.
    void connect_controllers();
    /// Routes session signals (events, start-up, libraries) to the panels.
    void connect_session();
    /// Creates a customizable command; single-key commands are scoped to the sample table.
    QAction* add_command(const QString& id, const QString& text, void (MainWindow::*handler)());
    /// Creates a dock with an object name (required by saveState) and registers it for the View menu.
    QDockWidget* add_dock(const QString& object_name, const QString& title, QWidget* content, Qt::DockWidgetArea area);

    // ------------------------------------------------------------ session and query state
    void on_session_ready();
    /// Switches between onboarding and browser and re-runs the query.
    void on_libraries_changed();
    /// Applies one merged catalog change: rows in place, full reload, or structure.
    void on_catalog_changed(const app::CatalogChangedEvent& event);
    /// Re-reads the result window (full reload), keeping the selection by key.
    void refresh_results();
    /// Runs the search box text in the navigator scope (skipped when unchanged).
    void apply_query();
    /// Feeds the selection to the inspector, preview, drag check and status bar.
    void on_selection_changed();
    /// Refreshes the three status bar labels.
    void update_status_labels();

    // ------------------------------------------------------------ commands
    void new_library();
    /// File ▸ Open Library.
    void open_library();
    /// File ▸ Close Library (asks which one when several are open).
    void close_library();
    /// File ▸ Add Folder.
    void add_folder();
    /// File ▸ Rescan Folder (the navigator's root).
    void rescan_root();
    /// File ▸ Export Metadata as CSV.
    void export_csv();
    /// File ▸ Export Metadata as JSON.
    void export_json();
    /// File ▸ Backup Library.
    void backup_library();
    /// Edit ▸ Undo in the routed library.
    void undo();
    /// Edit ▸ Redo in the routed library.
    void redo();
    /// Edit ▸ Preferences; applies and persists the result.
    void show_preferences();
    /// Analysis ▸ Generate Waveforms for the selection.
    void analyze_waveforms();
    /// Analysis ▸ Musical Analysis for the selection.
    void analyze_musical();
    /// Analysis ▸ Find Duplicates for the selection.
    void analyze_duplicates();
    /// Queues an analysis run of the selection after a confirmation.
    void start_analysis(const std::string& profile, const QString& description);
    /// Pauses or resumes all background analysis.
    void set_analysis_paused(bool paused);
    /// View ▸ Reset Layout.
    void reset_layout();
    /// View ▸ Save Layout Preset.
    void save_layout_preset();
    /// Lists the presets in the View menu.
    void rebuild_layout_presets_menu();
    /// Help ▸ About.
    void show_about();
    /// File ▸ Quit.
    void quit();
    /// Space: toggles the preview.
    void play_pause();
    /// Escape: stops the preview.
    void stop_preview();
    /// Ctrl+F: focuses the search box.
    void focus_search();
    /// Delete: removes the selection from the library (undoable).
    void remove_selection_from_library();
    /// F: favorite on, or off when every selected sample is a favorite.
    void toggle_favorite();
    /// 0–5: rates the selection.
    void set_rating(int rating);
    /// Creates a smart collection from the current search text.
    void create_smart_collection(const uuids::uuid& library, const QString& name);

    // ------------------------------------------------------------ helpers
    /// Applies an edit to the selected samples (optimistically, through EditController).
    void edit_selection(catalog::EditCommand command, const QString& done_message);
    /// Library Undo/Redo act on (UndoRouter), or nullopt.
    std::optional<uuids::uuid> undo_library() const;
    /// Library for a library-level command: the selection's or navigator's, else the user picks.
    std::optional<uuids::uuid> choose_library(const QString& purpose, bool writable_only);
    /// Applies a layout and re-queries with the sort its header shows.
    bool restore_layout(const nlohmann::json& layout);
    /// Makes the model sort by the header's sort indicator.
    void sync_sort_with_header();
    /// Assigns key sequences to the commands.
    void apply_shortcuts(const ShortcutBindings& bindings);
    /// Shows a failure prominently (notification bar), not only in the status bar.
    void show_failure(const QString& title, const QString& details);
    /// Shows a transient status bar message.
    void show_status(const QString& message);

    /// Gateway to the Application.
    ApplicationSession& session_;

    /// Central browser (owned through Qt's parent tree).
    SampleTableModel* model_ = nullptr;
    SampleTableView* table_ = nullptr;
    SearchBar* search_bar_ = nullptr;
    NotificationBar* notification_bar_ = nullptr;
    QStackedWidget* central_stack_ = nullptr;  ///< onboarding page or browser
    EmptyStateWidget* empty_state_ = nullptr;
    QWidget* browser_page_ = nullptr;
    DragPathResolver* drag_resolver_ = nullptr;
    RootLocations* root_locations_ = nullptr;

    /// Dock panels.
    NavigatorPanel* navigator_ = nullptr;
    InspectorPanel* inspector_ = nullptr;
    PreviewPanel* preview_ = nullptr;
    JobsPanel* jobs_ = nullptr;
    HistoryPanel* history_ = nullptr;

    /// Controllers that hold the window's business logic.
    EditController* edit_controller_ = nullptr;
    LibraryController* library_controller_ = nullptr;
    ExportController* export_controller_ = nullptr;
    SampleContextMenu* context_menu_ = nullptr;
    LayoutPresetStore layout_presets_;

    /// Docks, menus and actions the window updates later.
    std::vector<QDockWidget*> docks_;
    QMenu* layout_presets_menu_ = nullptr;
    QMenu* delete_presets_menu_ = nullptr;
    QAction* pause_analysis_action_ = nullptr;
    QAction* analyze_waveforms_action_ = nullptr;
    QAction* analyze_musical_action_ = nullptr;
    QAction* analyze_duplicates_action_ = nullptr;

    /// Status bar labels.
    QLabel* catalog_label_ = nullptr;
    QLabel* count_label_ = nullptr;
    QLabel* selection_label_ = nullptr;

    std::map<QString, QAction*> commands_;  ///< customizable commands by ShortcutCatalog id
    ShortcutBindings shortcuts_ = ShortcutCatalog::defaults();
    /// Play a sample as soon as it becomes current.
    bool audition_on_selection_ = false;
    NavigationScope scope_;     ///< navigator selection the query runs in
    QString coverage_message_;  ///< libraries a search could not reach
    QString count_text_;        ///< result count label text

    QTimer selection_timer_;       ///< coalesces selection and row changes into one update per event-loop pass
    QTimer window_refresh_timer_;  ///< throttles re-reads for changes outside the loaded rows
    std::vector<SampleKey> saved_selection_;  ///< selection kept across a full reload
    std::optional<SampleKey> saved_current_;
    std::optional<SampleKey> previous_current_;  ///< audition-on-selection plays only on a change
    QByteArray default_state_;                   ///< dock layout for Reset Layout
    QByteArray default_header_state_;            ///< table header for Reset Layout
};

}  // namespace mso::ui
