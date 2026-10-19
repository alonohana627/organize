#pragma once
/// @file
/// MainWindow: the desktop workspace — docks, central sample table, menus, keyboard commands and
/// the wiring between the panels and the ApplicationSession.
#include <QMainWindow>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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

class ApplicationSession;
class DragPathResolver;
class EmptyStateWidget;
class HistoryPanel;
class InspectorPanel;
class JobsPanel;
class NavigatorPanel;
class PreviewPanel;
class SampleTableModel;
class SampleTableView;
class SearchBar;

/// Main window of Music Sample Organizer.
///
/// Every catalog operation runs through ApplicationSession on worker threads; the window only
/// starts work and presents results. Undo/Redo act on one library: the library of the current
/// table row, else the one shown in the History dock, else the navigator's, else the library
/// edited last, else the only open library.
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(ApplicationSession& session, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    // ------------------------------------------------------------ construction
    void build_actions();
    void build_menus();
    void build_central();
    void build_docks();
    void build_status_bar();
    void connect_session();
    QAction* add_command(const QString& id, const QString& text, void (MainWindow::*handler)());
    QDockWidget* add_dock(const QString& object_name, const QString& title, QWidget* content, Qt::DockWidgetArea area);

    // ------------------------------------------------------------ session and query state
    void on_session_ready();
    void on_libraries_changed();
    void on_catalog_changed();
    void refresh_results();
    void apply_query();
    void on_selection_changed();
    void update_status_labels();

    // ------------------------------------------------------------ commands
    void new_library();
    void open_library();
    void close_library();
    void close_library_by_id(const uuids::uuid& library);
    void add_folder();
    /// Adds a sample folder (asking for one when `folder` is empty) to `library`, or to the only or
    /// default library; the folder is then scanned and, in automatic mode, analyzed.
    void add_folder_to(const std::optional<uuids::uuid>& library, const QString& folder = {});
    void rescan_root();
    void start_import(const uuids::uuid& library, std::int64_t root_id, const QString& description);
    void rebind_root(const uuids::uuid& library, std::int64_t root_id);
    void export_csv();
    void export_json();
    void export_metadata(bool json);
    void backup_library();
    void backup_library_by_id(const uuids::uuid& library);
    void undo();
    void redo();
    void undo_or_redo(bool redo);
    void show_preferences();
    void analyze_waveforms();
    void analyze_musical();
    void analyze_duplicates();
    void start_analysis(const std::string& profile, const QString& description);
    void set_analysis_paused(bool paused);
    void reset_layout();
    /// Applies a persisted layout and re-queries with the sort its header shows.
    bool restore_layout(const nlohmann::json& layout);
    void sync_sort_with_header();
    void save_layout_preset();
    void rebuild_layout_presets_menu();
    void show_about();

    void quit();
    void play_pause();
    void stop_preview();
    void focus_search();
    void remove_selection_from_library();
    void toggle_favorite();
    void set_rating(int rating);

    // ------------------------------------------------------------ helpers
    /// Applies `command` to the selection, one independent edit per library, and reports
    /// partial success per library (no cross-library atomicity, docs/01 §3).
    void apply_edit_to_selection(catalog::EditCommand command, const QString& done_message);
    /// Same as apply_edit_to_selection() for explicit samples (e.g. a star clicked in one row).
    void apply_edit_to_keys(const std::vector<SampleKey>& keys, catalog::EditCommand command,
                            const QString& done_message);
    /// Context menu of the sample table: play, Show in Explorer, copy path/file, rating, favorite,
    /// analysis, remove. Path actions are disabled when a targeted sample is unavailable.
    void show_sample_menu(const QPoint& global_position, int row);
    /// Resolves playable paths off the UI thread; `use` runs only when every sample resolved, so
    /// nothing is ever done for part of the request without saying so.
    void resolve_paths(const std::vector<SampleKey>& keys, std::function<void(std::vector<std::filesystem::path>)> use);
    void reveal_sample(const SampleKey& key);
    void copy_paths(const std::vector<SampleKey>& keys);
    void copy_files(const std::vector<SampleKey>& keys);
    void apply_taxonomy(const uuids::uuid& library, const catalog::TaxonomyCommand& command);
    void create_smart_collection(const uuids::uuid& library, const QString& name);
    /// Picks a library for an action: selection, navigator, the only one, or asks.
    std::optional<uuids::uuid> choose_library(const QString& purpose, bool writable_only);
    std::optional<uuids::uuid> undo_library() const;
    void apply_shortcuts(const ShortcutBindings& bindings);
    void show_error(const QString& title, const QString& message);
    void show_status(const QString& message);

    ApplicationSession& session_;

    SampleTableModel* model_ = nullptr;
    SampleTableView* table_ = nullptr;
    SearchBar* search_bar_ = nullptr;
    QStackedWidget* central_stack_ = nullptr;
    EmptyStateWidget* empty_state_ = nullptr;
    QWidget* browser_page_ = nullptr;
    DragPathResolver* drag_resolver_ = nullptr;

    NavigatorPanel* navigator_ = nullptr;
    InspectorPanel* inspector_ = nullptr;
    PreviewPanel* preview_ = nullptr;
    JobsPanel* jobs_ = nullptr;
    HistoryPanel* history_ = nullptr;
    std::vector<QDockWidget*> docks_;
    QMenu* view_menu_ = nullptr;
    QMenu* layout_presets_menu_ = nullptr;
    QMenu* delete_presets_menu_ = nullptr;
    QAction* pause_analysis_action_ = nullptr;

    QLabel* catalog_label_ = nullptr;
    QLabel* count_label_ = nullptr;
    QLabel* selection_label_ = nullptr;

    std::map<QString, QAction*> commands_;
    ShortcutBindings shortcuts_ = ShortcutCatalog::defaults();
    bool audition_on_selection_ = false;
    NavigationScope scope_;
    QString coverage_message_;
    QString count_text_;

    QTimer refresh_timer_;    ///< throttles result refreshes after catalog changes
    QTimer navigator_timer_;  ///< throttles navigator reloads (tag counts change during import)
    QTimer selection_timer_;  ///< coalesces selection changes
    std::vector<SampleKey> saved_selection_;
    std::optional<SampleKey> saved_current_;
    std::optional<SampleKey> previous_current_;
    std::optional<uuids::uuid> last_edited_library_;
    QByteArray default_state_;
    QByteArray default_header_state_;
    std::map<std::uint64_t, std::shared_ptr<std::stop_source>> running_exports_;
};

}  // namespace mso::ui
