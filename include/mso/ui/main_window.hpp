#pragma once
/// @file
/// MainWindow: the desktop workspace — docks, central sample table, menus and keyboard commands —
/// wiring the panels and controllers to the ApplicationSession.
#include <QMainWindow>
#include <QString>
#include <QTimer>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mso/app/application.hpp"
#include "mso/catalog/edit_command.hpp"
#include "mso/ui/navigation_scope.hpp"
#include "mso/ui/sample_key.hpp"
#include "mso/ui/shortcut_catalog.hpp"

class QAction;
class QDockWidget;
class QLabel;
class QMenu;
class QStackedWidget;

namespace mso::ui {

class AnalysisController;
class ApplicationSession;
class DragPathResolver;
class EditController;
class EmptyStateWidget;
class ExportController;
class HistoryPanel;
class InspectorPanel;
class JobsPanel;
class LayoutController;
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
/// undo in EditController (optimistic, ordered per library, behind the Application's edit API),
/// library and folder commands in LibraryController, export and backup in ExportController,
/// analysis runs and the analysis pause in AnalysisController, the View menu's layouts in
/// LayoutController, the sample menu in SampleContextMenu. Catalog change events refresh only the
/// rows they name; the result is re-queried only on a full reload, the navigator rebuilt only on a
/// structure change, and the history read only when it can have changed. Commands that change
/// samples act on the whole selection or not at all: when some selected rows are not loaded they
/// refuse and say why, never acting on part of it. Single-key shortcuts (Space, Delete, F, 0–5 …,
/// and any command rebound to a single key) belong to the sample table; only modifier shortcuts
/// are window-wide.
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    /// A window over `session`, which must outlive it.
    explicit MainWindow(ApplicationSession& session, QWidget* parent = nullptr);
    /// Disconnects the panels from the window before they are destroyed (see the source).
    ~MainWindow() override;

    /// Applies the preferences the session preloaded (layout, shortcuts, preview volume). Call
    /// before show(), so the window appears in its saved place and shape without jumping.
    void restore_saved_state();

    /// The sample table (tests and the screenshot tool drive it).
    SampleTableView* sample_table() const { return table_; }
    /// The notification strip (tests read its message).
    NotificationBar* notification_bar() const { return notification_bar_; }
    /// The notification for libraries whose upgrade kept colliding names apart (key_conflicts > 0):
    /// title and details, or nullopt when every such library is already in `reported` (it is added
    /// there). One message covers every library of one call.
    static std::optional<std::pair<QString, QString>> key_conflict_notice(
        const std::vector<app::OpenLibraryInfo>& libraries, std::set<uuids::uuid>& reported);
    /// True while a re-read of the result window is scheduled for samples the table does not hold
    /// (tests check that changes to known rows never schedule one).
    bool window_refresh_pending() const { return window_refresh_timer_.isActive(); }
    /// The command with a ShortcutCatalog id (tests check where its shortcut lives), or null.
    QAction* command(const QString& id) const;
    /// Assigns key sequences to the commands. A command whose sequence is a single key without
    /// Ctrl, Alt or Meta is scoped to the sample table, whatever command it is.
    void apply_shortcuts(const ShortcutBindings& bindings);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    // ------------------------------------------------------------ construction
    /// Creates the search bar, sample table, onboarding page and notification strip.
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
    /// Creates a customizable command (its shortcut scope is set by apply_shortcuts()).
    QAction* add_command(const QString& id, const QString& text, void (MainWindow::*handler)());
    /// Creates a dock with an object name (required by saveState) and registers it for the View menu.
    QDockWidget* add_dock(const QString& object_name, const QString& title, QWidget* content, Qt::DockWidgetArea area);

    // ------------------------------------------------------------ session and query state
    /// Reads the analysis pause state once the Application has started.
    void on_session_ready();
    /// Switches between onboarding and browser and re-runs the query.
    void on_libraries_changed();
    /// Applies one merged catalog change: rows in place, full reload, or structure.
    void on_catalog_changed(const app::CatalogChangedEvent& event);
    /// Re-reads the result window (anchored refresh at the first visible row).
    void refresh_results();
    /// Runs the search box text in the navigator scope (skipped when unchanged).
    void apply_query();
    /// Feeds the selection to the inspector, preview, drag check and status bar.
    void on_selection_changed();
    /// True when rows [first, last] contain the current row or a selected row.
    bool rows_touch_selection(int first, int last) const;
    /// Refreshes the three status bar labels.
    void update_status_labels();

    // ------------------------------------------------------------ commands
    /// File ▸ New Library.
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
    /// Queues an analysis run of the whole selection (refused when part of it is not loaded).
    void analyze_selection(const std::string& profile, const QString& description);
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
    /// Keys of the whole selection, or nullopt (with the reason shown) when nothing is selected
    /// or some selected rows are not loaded: `action` then never runs on part of the selection.
    std::optional<std::vector<SampleKey>> complete_selection(const QString& action);
    /// Applies an edit to the whole selection (optimistically, through EditController).
    void edit_selection(catalog::EditCommand command, const QString& action, const QString& done_message);
    /// Library Undo/Redo act on (UndoRouter), or nullopt.
    std::optional<uuids::uuid> undo_library() const;
    /// Library for a library-level command: the selection's or navigator's, else the user picks.
    std::optional<uuids::uuid> choose_library(const QString& purpose, bool writable_only);
    /// Makes the model sort by the header's sort indicator.
    void sync_sort_with_header();
    /// Shows a failure prominently (notification bar), not only in the status bar.
    void show_failure(const QString& title, const QString& details);
    /// Shows a transient status bar message.
    void show_status(const QString& message);

    /// Gateway to the Application.
    ApplicationSession& session_;

    // Central browser (owned through Qt's parent tree).
    SampleTableModel* model_ = nullptr;            ///< paged result of the current query
    SampleTableView* table_ = nullptr;             ///< the sample table
    SearchBar* search_bar_ = nullptr;              ///< query box above the table
    NotificationBar* notification_bar_ = nullptr;  ///< failures that must not vanish
    QStackedWidget* central_stack_ = nullptr;      ///< onboarding page or browser
    EmptyStateWidget* empty_state_ = nullptr;      ///< onboarding page
    QWidget* browser_page_ = nullptr;              ///< search bar and table
    DragPathResolver* drag_resolver_ = nullptr;    ///< background file check of the selection
    RootLocations* root_locations_ = nullptr;      ///< root base paths for drag URLs

    // Dock panels (owned through Qt's parent tree).
    NavigatorPanel* navigator_ = nullptr;  ///< libraries, folders, tags, collections
    InspectorPanel* inspector_ = nullptr;  ///< details and fields of the selection
    PreviewPanel* preview_ = nullptr;      ///< transport and waveform
    JobsPanel* jobs_ = nullptr;            ///< imports, analysis runs, exports
    HistoryPanel* history_ = nullptr;      ///< undo history of one library

    // Controllers that hold the window's business logic (children of the window).
    EditController* edit_controller_ = nullptr;          ///< edits, undo and redo
    LibraryController* library_controller_ = nullptr;    ///< libraries and folders
    ExportController* export_controller_ = nullptr;      ///< exports and backups
    AnalysisController* analysis_controller_ = nullptr;  ///< analysis runs and pause
    LayoutController* layout_controller_ = nullptr;      ///< View menu layouts
    SampleContextMenu* context_menu_ = nullptr;          ///< the sample table's menu

    /// Docks, in the order the View menu lists them.
    std::vector<QDockWidget*> docks_;
    /// Analysis ▸ Pause All Analysis (checked while paused).
    QAction* pause_analysis_action_ = nullptr;
    QAction* analyze_waveforms_action_ = nullptr;   ///< Analysis ▸ Generate Waveforms
    QAction* analyze_musical_action_ = nullptr;     ///< Analysis ▸ Musical Analysis
    QAction* analyze_duplicates_action_ = nullptr;  ///< Analysis ▸ Find Duplicates

    // Status bar labels.
    QLabel* catalog_label_ = nullptr;    ///< open libraries and scope
    QLabel* count_label_ = nullptr;      ///< result count
    QLabel* selection_label_ = nullptr;  ///< selected count

    std::map<QString, QAction*> commands_;                      ///< customizable commands by ShortcutCatalog id
    ShortcutBindings shortcuts_ = ShortcutCatalog::defaults();  ///< bindings in effect
    /// Play a sample as soon as it becomes current.
    bool audition_on_selection_ = false;
    NavigationScope scope_;     ///< navigator selection the query runs in
    QString coverage_message_;  ///< libraries a search could not reach
    QString count_text_;        ///< result count label text

    QTimer selection_timer_;       ///< coalesces selection and row changes into one update per event-loop pass
    QTimer window_refresh_timer_;  ///< throttles re-reads for changes outside the loaded rows
    std::set<uuids::uuid> reported_key_conflicts_;  ///< libraries whose upgrade collisions were shown
    std::optional<SampleKey> previous_current_;     ///< audition-on-selection plays only on a change
};

}  // namespace mso::ui
