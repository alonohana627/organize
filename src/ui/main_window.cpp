#include "mso/ui/main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

#include "mso/ui/about_dialog.hpp"
#include "mso/ui/analysis_controller.hpp"
#include "mso/ui/application_session.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/drag_path_resolver.hpp"
#include "mso/ui/edit_controller.hpp"
#include "mso/ui/empty_state_widget.hpp"
#include "mso/ui/export_controller.hpp"
#include "mso/ui/history_panel.hpp"
#include "mso/ui/inspector_panel.hpp"
#include "mso/ui/jobs_panel.hpp"
#include "mso/ui/layout_controller.hpp"
#include "mso/ui/library_controller.hpp"
#include "mso/ui/navigator_panel.hpp"
#include "mso/ui/notification_bar.hpp"
#include "mso/ui/preferences_dialog.hpp"
#include "mso/ui/preview_panel.hpp"
#include "mso/ui/root_locations.hpp"
#include "mso/ui/sample_context_menu.hpp"
#include "mso/ui/sample_table_model.hpp"
#include "mso/ui/sample_table_view.hpp"
#include "mso/ui/search_bar.hpp"
#include "mso/ui/ui_preferences.hpp"
#include "mso/ui/undo_router.hpp"

namespace mso::ui {

namespace {

constexpr int kStatusMessageMilliseconds = 6000;
/// Throttle of window re-reads caused by changes to samples the table has not loaded.
constexpr int kWindowRefreshMilliseconds = 500;
constexpr int kDefaultWidth = 1400;
constexpr int kDefaultHeight = 860;

}  // namespace

MainWindow::MainWindow(ApplicationSession& session, QWidget* parent) : QMainWindow(parent), session_(session) {
    setWindowTitle(tr("Music Sample Organizer"));
    setObjectName(QStringLiteral("MainWindow"));
    setDockNestingEnabled(true);
    resize(kDefaultWidth, kDefaultHeight);  // until restore_saved_state() applies a saved geometry

    // Zero-delay: every selection, row and reset signal of one event-loop pass becomes one update.
    selection_timer_.setSingleShot(true);
    selection_timer_.setInterval(0);
    connect(&selection_timer_, &QTimer::timeout, this, &MainWindow::on_selection_changed);
    window_refresh_timer_.setSingleShot(true);
    window_refresh_timer_.setInterval(kWindowRefreshMilliseconds);
    connect(&window_refresh_timer_, &QTimer::timeout, this, &MainWindow::refresh_results);

    build_central();
    build_docks();
    build_actions();
    build_menus();
    build_status_bar();
    connect_controllers();
    connect_session();
    apply_shortcuts(shortcuts_);

    layout_controller_->capture_default();
    update_status_labels();
}

MainWindow::~MainWindow() {
    // ~QWidget destroys the panels and controllers after this window's own members (the timers
    // above all), and while they die they still emit (current item, selection, visibility). Cut
    // every connection into this window first, so no handler runs against a half-destroyed window.
    for (QObject* child : findChildren<QObject*>())
        QObject::disconnect(child, nullptr, this, nullptr);
    QObject::disconnect(&session_, nullptr, this, nullptr);
    selection_timer_.stop();
    window_refresh_timer_.stop();
}

void MainWindow::restore_saved_state() {
    shortcuts_ = ShortcutCatalog::from_json(session_.preference(ShortcutCatalog::kPreferenceKey));
    apply_shortcuts(shortcuts_);
    const nlohmann::json audition = session_.preference(UiPreferenceKeys::kAuditionOnSelection);
    audition_on_selection_ = audition.is_boolean() && audition.get<bool>();
    preview_->load_preferences();
    layout_controller_->restore_saved();
}

QAction* MainWindow::command(const QString& id) const {
    const auto found = commands_.find(id);
    return found == commands_.end() ? nullptr : found->second;
}

// ---------------------------------------------------------------- construction

void MainWindow::build_central() {
    model_ = new SampleTableModel(session_, this);
    table_ = new SampleTableView;
    table_->set_sample_model(model_);
    drag_resolver_ = new DragPathResolver(session_, this);
    root_locations_ = new RootLocations(session_, this);
    table_->set_drag_resolver(drag_resolver_);
    table_->set_root_locations(root_locations_);
    search_bar_ = new SearchBar;
    notification_bar_ = new NotificationBar;

    browser_page_ = new QWidget;
    auto* browser_layout = new QVBoxLayout(browser_page_);
    browser_layout->setContentsMargins(4, 4, 4, 0);
    browser_layout->addWidget(search_bar_);
    browser_layout->addWidget(table_, 1);

    empty_state_ = new EmptyStateWidget;
    central_stack_ = new QStackedWidget;
    central_stack_->addWidget(empty_state_);
    central_stack_->addWidget(browser_page_);
    // The notification strip sits above both pages: failures on the onboarding page show too.
    auto* central = new QWidget(this);
    auto* central_layout = new QVBoxLayout(central);
    central_layout->setContentsMargins(0, 0, 0, 0);
    central_layout->setSpacing(0);
    central_layout->addWidget(notification_bar_);
    central_layout->addWidget(central_stack_, 1);
    setCentralWidget(central);

    connect(search_bar_, &SearchBar::querySubmitted, this, [this] { apply_query(); });
    const auto schedule_selection_update = [this] { selection_timer_.start(); };
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this, schedule_selection_update);
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this, schedule_selection_update);
    // Rows patched by an edit or refreshed in place reach the inspector in the same pass; changes
    // to rows outside the selection (analysis results elsewhere) do not re-read the selection.
    connect(model_, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& top_left, const QModelIndex& bottom_right) {
                if (rows_touch_selection(top_left.row(), bottom_right.row())) selection_timer_.start();
            });
    connect(model_, &QAbstractItemModel::rowsRemoved, this, schedule_selection_update);
    connect(model_, &QAbstractItemModel::modelReset, this, schedule_selection_update);
    connect(table_, &QAbstractItemView::doubleClicked, this, [this] { preview_->play_selected(); });
    connect(table_, &SampleTableView::dragRefused, this, &MainWindow::show_status);
    connect(table_, &SampleTableView::mixedDragRefused, this,
            [this](const std::vector<SampleKey>& available, std::size_t unavailable) {
                // startDrag() is still on the stack: open the dialog afterwards. Only values are
                // used after the box's event loop.
                QTimer::singleShot(0, this, [this, available, unavailable] {
                    QMessageBox box(QMessageBox::Warning, tr("Some samples are unavailable"),
                                    tr("%n of the selected samples are offline or missing, so nothing was dragged.",
                                       nullptr, static_cast<int>(unavailable)),
                                    QMessageBox::Cancel, this);
                    QPushButton* select_available = available.empty()
                                                        ? nullptr
                                                        : box.addButton(tr("Select the %n available sample(s)", nullptr,
                                                                           static_cast<int>(available.size())),
                                                                        QMessageBox::AcceptRole);
                    box.exec();
                    if (select_available && box.clickedButton() == select_available) {
                        table_->select_keys(available, table_->current_key());
                        show_status(tr("Selected the available samples; drag them again."));
                    }
                });
            });

    connect(model_, &SampleTableModel::countUpdating, this, [this] {
        count_text_ = tr("%n result(s) so far (counting…)", nullptr, model_->rowCount());
        update_status_labels();
    });
    connect(model_, &SampleTableModel::countChanged, this, [this](qint64 total, bool complete) {
        count_text_ = complete
                          ? tr("%1 results (exact)").arg(DisplayFormat::count(total))
                          : tr("%1 results (partial: some libraries unavailable)").arg(DisplayFormat::count(total));
        update_status_labels();
    });
    connect(model_, &SampleTableModel::coverageChanged, this, [this](const QString& message) {
        coverage_message_ = message;
        update_status_labels();
    });
    connect(model_, &SampleTableModel::queryFailed, this, &MainWindow::show_status);
    // Persistent indexes already moved the selection with its samples: only re-read it.
    connect(model_, &SampleTableModel::refreshed, this, schedule_selection_update);
}

QDockWidget* MainWindow::add_dock(const QString& object_name, const QString& title, QWidget* content,
                                  Qt::DockWidgetArea area) {
    auto* dock = new QDockWidget(title, this);
    dock->setObjectName(object_name);  // required by saveState()/restoreState()
    dock->setWidget(content);
    dock->setAccessibleName(title);
    addDockWidget(area, dock);
    docks_.push_back(dock);
    return dock;
}

void MainWindow::build_docks() {
    navigator_ = new NavigatorPanel(session_);
    inspector_ = new InspectorPanel(session_);
    preview_ = new PreviewPanel(session_);
    jobs_ = new JobsPanel;
    history_ = new HistoryPanel(session_);

    add_dock(QStringLiteral("NavigatorDock"), tr("Navigator"), navigator_, Qt::LeftDockWidgetArea);
    QDockWidget* inspector_dock =
        add_dock(QStringLiteral("InspectorDock"), tr("Inspector"), inspector_, Qt::RightDockWidgetArea);
    QDockWidget* history_dock =
        add_dock(QStringLiteral("HistoryDock"), tr("History"), history_, Qt::RightDockWidgetArea);
    QDockWidget* preview_dock =
        add_dock(QStringLiteral("PreviewDock"), tr("Preview"), preview_, Qt::BottomDockWidgetArea);
    QDockWidget* jobs_dock = add_dock(QStringLiteral("JobsDock"), tr("Jobs"), jobs_, Qt::BottomDockWidgetArea);
    tabifyDockWidget(inspector_dock, history_dock);
    inspector_dock->raise();
    resizeDocks({inspector_dock}, {420}, Qt::Horizontal);
    splitDockWidget(preview_dock, jobs_dock, Qt::Horizontal);
    resizeDocks({preview_dock, jobs_dock}, {700, 500}, Qt::Horizontal);
}

QAction* MainWindow::add_command(const QString& id, const QString& text, void (MainWindow::*handler)()) {
    auto* action = new QAction(text, this);
    connect(action, &QAction::triggered, this, handler);
    commands_[id] = action;
    return action;
}

void MainWindow::build_actions() {
    add_command(QStringLiteral("play_pause"), tr("Play / Pause"), &MainWindow::play_pause);
    add_command(QStringLiteral("stop"), tr("Stop"), &MainWindow::stop_preview);
    add_command(QStringLiteral("undo"), tr("&Undo"), &MainWindow::undo);
    add_command(QStringLiteral("redo"), tr("&Redo"), &MainWindow::redo);
    add_command(QStringLiteral("focus_search"), tr("Find…"), &MainWindow::focus_search);
    add_command(QStringLiteral("remove_from_library"), tr("Remove from Library"),
                &MainWindow::remove_selection_from_library);
    add_command(QStringLiteral("toggle_favorite"), tr("Toggle Favorite"), &MainWindow::toggle_favorite);
    add_command(QStringLiteral("new_library"), tr("&New Library…"), &MainWindow::new_library);
    add_command(QStringLiteral("open_library"), tr("&Open Library…"), &MainWindow::open_library);
    add_command(QStringLiteral("add_folder"), tr("&Add Folder…"), &MainWindow::add_folder);
    add_command(QStringLiteral("preferences"), tr("&Preferences…"), &MainWindow::show_preferences);
    add_command(QStringLiteral("quit"), tr("&Quit"), &MainWindow::quit);
    for (int rating = 0; rating <= 5; ++rating) {
        auto* action = new QAction(rating == 0 ? tr("Clear Rating") : tr("Rate %n Star(s)", nullptr, rating), this);
        connect(action, &QAction::triggered, this, [this, rating] { set_rating(rating); });
        commands_[QStringLiteral("rating_%1").arg(rating)] = action;
    }
    analyze_waveforms_action_ = new QAction(tr("Generate &Waveforms for Selection"), this);
    analyze_musical_action_ = new QAction(tr("&Musical Analysis (BPM, Key) for Selection"), this);
    analyze_duplicates_action_ = new QAction(tr("Find &Duplicates for Selection"), this);
    connect(analyze_waveforms_action_, &QAction::triggered, this, &MainWindow::analyze_waveforms);
    connect(analyze_musical_action_, &QAction::triggered, this, &MainWindow::analyze_musical);
    connect(analyze_duplicates_action_, &QAction::triggered, this, &MainWindow::analyze_duplicates);
}

void MainWindow::build_menus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(commands_.at(QStringLiteral("new_library")));
    file->addAction(commands_.at(QStringLiteral("open_library")));
    file->addAction(tr("&Close Library…"), this, &MainWindow::close_library);
    file->addSeparator();
    file->addAction(commands_.at(QStringLiteral("add_folder")));
    file->addAction(tr("&Rescan Folder"), this, &MainWindow::rescan_root);
    file->addSeparator();
    file->addAction(tr("Export Metadata as &CSV…"), this, &MainWindow::export_csv);
    file->addAction(tr("Export Metadata as &JSON…"), this, &MainWindow::export_json);
    file->addAction(tr("&Backup Library…"), this, &MainWindow::backup_library);
    file->addSeparator();
    file->addAction(commands_.at(QStringLiteral("quit")));

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(commands_.at(QStringLiteral("undo")));
    edit->addAction(commands_.at(QStringLiteral("redo")));
    edit->addSeparator();
    edit->addAction(commands_.at(QStringLiteral("focus_search")));
    edit->addAction(commands_.at(QStringLiteral("toggle_favorite")));
    QMenu* rating = edit->addMenu(tr("Rating"));
    for (int stars = 0; stars <= 5; ++stars)
        rating->addAction(commands_.at(QStringLiteral("rating_%1").arg(stars)));
    edit->addAction(commands_.at(QStringLiteral("remove_from_library")));
    edit->addSeparator();
    edit->addAction(commands_.at(QStringLiteral("preferences")));

    QMenu* playback = menuBar()->addMenu(tr("&Playback"));
    playback->addAction(commands_.at(QStringLiteral("play_pause")));
    playback->addAction(commands_.at(QStringLiteral("stop")));

    QMenu* analysis = menuBar()->addMenu(tr("&Analysis"));
    analysis->addAction(analyze_waveforms_action_);
    analysis->addAction(analyze_musical_action_);
    analysis->addAction(analyze_duplicates_action_);
    analysis->addSeparator();
    pause_analysis_action_ = analysis->addAction(tr("&Pause All Analysis"));
    pause_analysis_action_->setCheckable(true);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    for (QDockWidget* dock : docks_)
        view->addAction(dock->toggleViewAction());
    view->addSeparator();
    layout_controller_ = new LayoutController(session_, *this, *table_->horizontalHeader());
    layout_controller_->add_menu_entries(*view);

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About Music Sample Organizer"), this, &MainWindow::show_about);
}

void MainWindow::build_status_bar() {
    catalog_label_ = new QLabel(this);
    catalog_label_->setAccessibleName(tr("Catalog state"));
    catalog_label_->setTextFormat(Qt::PlainText);  // scope labels carry folder and tag names
    count_label_ = new QLabel(this);
    count_label_->setAccessibleName(tr("Result count"));
    selection_label_ = new QLabel(this);
    selection_label_->setAccessibleName(tr("Selected count"));
    statusBar()->addPermanentWidget(catalog_label_, 1);
    statusBar()->addPermanentWidget(count_label_);
    statusBar()->addPermanentWidget(selection_label_);
}

void MainWindow::connect_controllers() {
    edit_controller_ = new EditController(session_, *model_, this);
    library_controller_ = new LibraryController(session_, this);
    export_controller_ = new ExportController(session_, *jobs_, this);
    analysis_controller_ = new AnalysisController(session_, this);
    SampleMenuCommands menu_commands;
    menu_commands.play = commands_.at(QStringLiteral("play_pause"));
    for (int stars = 0; stars <= 5; ++stars)
        menu_commands.ratings[static_cast<std::size_t>(stars)] = commands_.at(QStringLiteral("rating_%1").arg(stars));
    menu_commands.toggle_favorite = commands_.at(QStringLiteral("toggle_favorite"));
    menu_commands.remove_from_library = commands_.at(QStringLiteral("remove_from_library"));
    menu_commands.analyze_waveforms = analyze_waveforms_action_;
    menu_commands.analyze_musical = analyze_musical_action_;
    menu_commands.analyze_duplicates = analyze_duplicates_action_;
    context_menu_ = new SampleContextMenu(session_, menu_commands, this);

    // Edits: optimistic rows now, failures shown prominently, history and taxonomy follow.
    connect(edit_controller_, &EditController::succeeded, this, &MainWindow::show_status);
    connect(edit_controller_, &EditController::failed, this, &MainWindow::show_failure);
    connect(edit_controller_, &EditController::historyChanged, this, [this](const uuids::uuid& library) {
        if (history_->library() == library) history_->reload();
    });
    connect(edit_controller_, &EditController::structureChanged, this, [this](const uuids::uuid& library) {
        navigator_->reload_library(library);
        inspector_->library_structure_changed(library);
    });
    connect(edit_controller_, &EditController::reloadRequested, this, &MainWindow::refresh_results);
    connect(inspector_, &InspectorPanel::editRequested, this,
            [this](const catalog::EditCommand& command) { edit_selection(command, tr("Edit"), tr("Saved")); });
    connect(table_, &SampleTableView::ratingClicked, this, [this](const SampleKey& key, int rating) {
        catalog::EditCommand command;
        command.rating = rating;
        edit_controller_->apply({key}, command,
                                rating == 0 ? tr("Cleared rating") : tr("Rated %n star(s)", nullptr, rating));
    });
    connect(table_, &SampleTableView::sampleMenuRequested, this, [this](const QPoint& position, int row) {
        const std::vector<search::SampleRow> samples = table_->selected_samples();
        if (row < 0 || samples.empty()) return;  // empty space below the rows: nothing to act on
        const search::SampleRow* clicked = model_->loaded_row(row);
        context_menu_->show(position, samples, table_->selected_row_count(), clicked ? *clicked : samples.front());
    });
    connect(context_menu_, &SampleContextMenu::statusMessage, this, &MainWindow::show_status);
    connect(history_, &HistoryPanel::undoRequested, this, &MainWindow::undo);
    connect(history_, &HistoryPanel::redoRequested, this, &MainWindow::redo);

    // Navigator.
    connect(navigator_, &NavigatorPanel::scopeSelected, this, [this](const NavigationScope& scope) {
        scope_ = scope;
        apply_query();
    });
    connect(navigator_, &NavigatorPanel::taxonomyCommandRequested, edit_controller_, &EditController::apply_taxonomy);
    connect(navigator_, &NavigatorPanel::smartCollectionRequested, this, &MainWindow::create_smart_collection);
    connect(navigator_, &NavigatorPanel::addFolderRequested, this,
            [this](const uuids::uuid& library) { library_controller_->add_folder(library); });
    connect(navigator_, &NavigatorPanel::rescanRequested, library_controller_, &LibraryController::rescan);
    connect(navigator_, &NavigatorPanel::rebindRequested, library_controller_, &LibraryController::relocate);
    connect(navigator_, &NavigatorPanel::closeLibraryRequested, library_controller_, &LibraryController::close_library);
    connect(navigator_, &NavigatorPanel::backupRequested, export_controller_, &ExportController::backup);
    connect(navigator_, &NavigatorPanel::statusMessage, this, &MainWindow::show_status);

    // Library commands.
    connect(empty_state_, &EmptyStateWidget::addFolderRequested, this, &MainWindow::add_folder);
    connect(empty_state_, &EmptyStateWidget::openLibraryRequested, this, &MainWindow::open_library);
    connect(library_controller_, &LibraryController::importStarted, jobs_, &JobsPanel::track_import);
    connect(library_controller_, &LibraryController::libraryClosing, this, [this] {
        preview_->stop();
        preview_->select_sample(std::nullopt, {});
    });
    connect(library_controller_, &LibraryController::statusMessage, this, &MainWindow::show_status);
    connect(library_controller_, &LibraryController::failed, this, &MainWindow::show_failure);
    connect(library_controller_, &LibraryController::libraryOpenFailed, this, [this](const QString& reason) {
        std::vector<uuids::uuid> open;
        for (const auto& info : session_.libraries())
            open.push_back(info.uuid);
        jobs_->fail_unopened_library_opens(open, reason);
    });
    connect(export_controller_, &ExportController::statusMessage, this, &MainWindow::show_status);
    connect(export_controller_, &ExportController::failed, this, &MainWindow::show_failure);
    connect(preview_, &PreviewPanel::statusMessage, this, &MainWindow::show_status);

    // Analysis and jobs.
    connect(analysis_controller_, &AnalysisController::pausedChanged, this, [this](bool paused) {
        pause_analysis_action_->setChecked(paused);
        jobs_->set_analysis_paused(paused);
    });
    connect(analysis_controller_, &AnalysisController::statusMessage, this, &MainWindow::show_status);
    connect(analysis_controller_, &AnalysisController::failed, this, &MainWindow::show_failure);
    connect(pause_analysis_action_, &QAction::triggered, analysis_controller_, &AnalysisController::set_paused);
    connect(jobs_, &JobsPanel::analysisPauseRequested, analysis_controller_, &AnalysisController::set_paused);
    connect(jobs_, &JobsPanel::analysisControlRequested, analysis_controller_, &AnalysisController::control_run);
    connect(jobs_, &JobsPanel::cancelOperationRequested, this, [this](app::OperationId operation) {
        session_.run_detached([operation](app::Application& application) { application.cancel_operation(operation); });
    });

    // Layout.
    connect(layout_controller_, &LayoutController::headerRestored, this, &MainWindow::sync_sort_with_header);
    connect(layout_controller_, &LayoutController::statusMessage, this, &MainWindow::show_status);
}

void MainWindow::connect_session() {
    connect(&session_, &ApplicationSession::ready, this, &MainWindow::on_session_ready);
    connect(&session_, &ApplicationSession::startFailed, this, [this](const QString& message) {
        empty_state_->show_error(tr("The workspace could not be opened: %1").arg(message));
        catalog_label_->setText(tr("Workspace unavailable"));
    });
    connect(&session_, &ApplicationSession::librariesChanged, this, &MainWindow::on_libraries_changed);
    connect(&session_, &ApplicationSession::catalogChanged, this, &MainWindow::on_catalog_changed);
    connect(&session_, &ApplicationSession::importProgressed, jobs_, &JobsPanel::update_import);
    connect(&session_, &ApplicationSession::libraryOpenProgressed, jobs_, &JobsPanel::update_library_open);
    connect(&session_, &ApplicationSession::operationFinished, this, [this](const app::OperationFinishedEvent& event) {
        jobs_->finish_operation(event);
        const QString description = DisplayFormat::from_utf8(event.description);
        if (event.error && event.error->code != ErrorCode::Cancelled)
            show_failure(tr("%1 failed").arg(description), DisplayFormat::error(*event.error));
        else
            show_status(tr("Finished: %1").arg(description));
    });
    connect(&session_, &ApplicationSession::analysisProgressed, this, [this](const app::AnalysisProgressEvent& event) {
        jobs_->update_analysis(event, session_.library_name(event.library));
    });
    connect(&session_, &ApplicationSession::backgroundError, this, [this](const app::BackgroundErrorEvent& event) {
        const QString context = DisplayFormat::from_utf8(event.context);
        const QString details =
            event.library ? tr("%1: %2").arg(session_.library_name(*event.library), DisplayFormat::error(event.error))
                          : DisplayFormat::error(event.error);
        show_failure(tr("Problem while %1").arg(context), details);
        // A library that failed to become writable (its upgrade included) stops showing as upgrading.
        if (event.library) jobs_->fail_library_open(*event.library, DisplayFormat::error(event.error));
    });
    connect(&session_, &ApplicationSession::libraryAccessChanged, this,
            [this](const app::LibraryAccessChangedEvent& event) {
                const QString name = session_.library_name(event.library);
                show_status(event.read_only
                                ? (event.in_use_elsewhere ? tr("%1 is read-only: another program is using it").arg(name)
                                                          : tr("%1 is read-only now").arg(name))
                                : tr("%1 can be edited again (the other program released it)").arg(name));
            });
    connect(&session_, &ApplicationSession::rootAvailabilityChanged, this,
            [this](const app::RootAvailabilityEvent& event) {
                // The rows' availability arrives as a catalog change; here only the labels and paths.
                navigator_->set_root_availability(event.library, event.root_id, event.availability);
                root_locations_->reload();
            });
}

// ---------------------------------------------------------------- session and query state

void MainWindow::on_session_ready() {
    analysis_controller_->load_paused_state();
}

void MainWindow::on_libraries_changed() {
    // Until some library has a sample folder there is nothing to browse: show the onboarding page.
    const auto& libraries = session_.libraries();
    const bool any_open =
        std::ranges::any_of(libraries, [](const app::OpenLibraryInfo& info) { return info.root_count > 0; });
    central_stack_->setCurrentWidget(any_open ? browser_page_ : empty_state_);
    if (!any_open) {
        empty_state_->show_welcome();
        model_->clear();
        preview_->select_sample(std::nullopt, {});
    }
    std::vector<uuids::uuid> open;
    for (const auto& info : libraries)
        open.push_back(info.uuid);
    jobs_->retain_libraries(open);                // runs of a closed library are no longer cancellable
    jobs_->finish_completed_library_opens(open);  // an upgraded library is listed once it is open
    if (const auto notice = key_conflict_notice(libraries, reported_key_conflicts_))
        show_failure(notice->first, notice->second);
    navigator_->reload();
    history_->reload();
    root_locations_->reload();
    // The open libraries changed, so even an identical query covers different data now.
    if (any_open) model_->run_query(DisplayFormat::to_utf8(search_bar_->text()), scope_);
    selection_timer_.start();  // a library that became writable re-enables the inspector's edits
    update_status_labels();
}

void MainWindow::on_catalog_changed(const app::CatalogChangedEvent& event) {
    if (event.full_reload) {
        refresh_results();
    } else if (!event.sample_ids.empty()) {
        std::vector<SampleKey> keys;
        keys.reserve(event.sample_ids.size());
        bool selected_sample_changed = false;
        const std::optional<SampleKey> current = table_->current_key();
        for (std::int64_t sample_id : event.sample_ids) {
            const SampleKey key{event.library, sample_id};
            keys.push_back(key);
            selected_sample_changed = selected_sample_changed || key == current;
        }
        const std::size_t known = model_->refresh_rows(keys);  // in place: no re-query, no scroll jump
        if (selected_sample_changed) inspector_->reload_detail();
        // Samples the table does not hold (loaded or evicted) may be new (an import) or may now match
        // the query: re-read the window, at most every kWindowRefreshMilliseconds (the index answers
        // in ms; the model runs one refresh at a time and owes at most one more).
        if (known < keys.size() && !window_refresh_timer_.isActive()) window_refresh_timer_.start();
    }
    if (event.structure_changed) {
        navigator_->reload_library(event.library);
        inspector_->library_structure_changed(event.library);  // ignored unless it is the selection's
        root_locations_->reload();
    }
    // History made by this window is reloaded when its edit settles (EditController); this covers
    // history made elsewhere, read at most every few seconds and only while the panel is visible.
    history_->catalog_changed(event.library);
}

std::optional<std::pair<QString, QString>> MainWindow::key_conflict_notice(
    const std::vector<app::OpenLibraryInfo>& libraries, std::set<uuids::uuid>& reported) {
    QStringList lines;
    for (const auto& info : libraries) {
        if (info.key_conflicts <= 0 || !reported.insert(info.uuid).second) continue;
        QString line = tr("%1: %n name(s) or path(s) became identical under the new comparison rules and were kept "
                          "apart (they are listed for review).",
                          nullptr, static_cast<int>(info.key_conflicts))
                           .arg(DisplayFormat::from_utf8(info.name));
        if (info.pre_rekey_backup)
            line = tr("%1 A backup from before the upgrade is at %2.")
                       .arg(line, DisplayFormat::from_path(*info.pre_rekey_backup));
        lines << line;
    }
    if (lines.isEmpty()) return std::nullopt;
    return std::pair{tr("Some names collided after the library upgrade"), lines.join(QLatin1Char('\n'))};
}

void MainWindow::refresh_results() {
    model_->refresh(table_->first_visible_row());
}

void MainWindow::apply_query() {
    if (session_.libraries().empty()) return;
    model_->set_query(DisplayFormat::to_utf8(search_bar_->text()), scope_);
    update_status_labels();
}

bool MainWindow::rows_touch_selection(int first, int last) const {
    const QItemSelectionModel* selection = table_->selectionModel();
    if (!selection) return false;
    const int current = selection->currentIndex().row();
    if (current >= first && current <= last) return true;
    return std::ranges::any_of(selection->selection(), [first, last](const QItemSelectionRange& range) {
        return range.top() <= last && range.bottom() >= first;
    });
}

void MainWindow::on_selection_changed() {
    const std::vector<search::SampleRow> samples = table_->selected_samples();
    const std::size_t selected = table_->selected_row_count();
    std::vector<SampleKey> keys;
    keys.reserve(samples.size());
    std::set<uuids::uuid> libraries;
    for (const auto& sample : samples) {
        keys.push_back({sample.library, sample.sample_id});
        libraries.insert(sample.library);
    }
    // Edits act on the whole selection: when part of it is evicted, its libraries come from the
    // keys every page keeps (only then: that walks every selected row).
    const bool partly_loaded = samples.size() < selected;
    if (partly_loaded) {
        libraries.clear();
        for (const SampleKey& key : table_->selected_keys())
            libraries.insert(key.library);
    }
    // One pass over the open libraries, not one lookup (and copy) per selected sample.
    const bool read_only = std::ranges::any_of(session_.libraries(), [&libraries](const app::OpenLibraryInfo& info) {
        return info.read_only && libraries.contains(info.uuid);
    });
    drag_resolver_->resolve(keys);  // the loaded rows: drags need their paths
    inspector_->show_selection(samples, selected, partly_loaded ? std::optional(libraries) : std::nullopt);
    inspector_->set_read_only(read_only);

    const std::optional<SampleKey> current = table_->current_key();
    QString current_name;
    if (current) {
        const auto found = std::ranges::find_if(samples, [&current](const search::SampleRow& sample) {
            return SampleKey{sample.library, sample.sample_id} == *current;
        });
        if (found != samples.end())
            current_name =
                DisplayFormat::from_utf8(found->display_name.empty() ? found->file_name : found->display_name);
        else
            current_name = table_->model()->index(table_->currentIndex().row(), 0).data().toString();
    }
    preview_->select_sample(current, current_name);
    if (audition_on_selection_ && current && current != previous_current_) preview_->play_selected();
    previous_current_ = current;

    if (auto library = undo_library()) history_->show_library(library);
    update_status_labels();
}

void MainWindow::update_status_labels() {
    const auto& libraries = session_.libraries();
    QString catalog;
    if (!session_.is_ready()) {
        catalog = tr("Opening workspace…");
    } else if (libraries.empty()) {
        catalog = tr("No library open");
    } else {
        catalog = tr("%n library(s) open", nullptr, static_cast<int>(libraries.size()));
        if (std::ranges::any_of(libraries, [](const auto& info) { return info.read_only; }))
            catalog = tr("%1 (read-only library open)").arg(catalog);
        if (!scope_.label.isEmpty()) catalog = tr("%1 · %2").arg(catalog, scope_.label);
    }
    if (!coverage_message_.isEmpty()) catalog = tr("%1 · %2").arg(catalog, coverage_message_);
    catalog_label_->setText(catalog);
    count_label_->setText(libraries.empty() ? QString() : count_text_);
    const auto selected = static_cast<int>(table_->selected_row_count());
    selection_label_->setText(selected == 0 ? QString() : tr("%n selected", nullptr, selected));
}

// ---------------------------------------------------------------- commands

void MainWindow::new_library() {
    library_controller_->new_library();
}

void MainWindow::open_library() {
    library_controller_->open_library();
}

void MainWindow::close_library() {
    if (auto library = choose_library(tr("Close which library?"), false)) library_controller_->close_library(*library);
}

void MainWindow::add_folder() {
    library_controller_->add_folder(std::nullopt, {}, navigator_->current_library());
}

void MainWindow::rescan_root() {
    const auto library = navigator_->current_library();
    const auto root = navigator_->current_root();
    if (!library || !root) {
        show_failure(tr("Rescan folder"), tr("Select a folder (or one of its subfolders) in the Navigator first."));
        return;
    }
    library_controller_->rescan(*library, *root);
}

void MainWindow::export_csv() {
    if (auto library = choose_library(tr("Export which library?"), false))
        export_controller_->export_metadata(*library, false, search_bar_->text());
}

void MainWindow::export_json() {
    if (auto library = choose_library(tr("Export which library?"), false))
        export_controller_->export_metadata(*library, true, search_bar_->text());
}

void MainWindow::backup_library() {
    if (auto library = choose_library(tr("Back up which library?"), false)) export_controller_->backup(*library);
}

std::optional<uuids::uuid> MainWindow::undo_library() const {
    UndoContext context;
    if (auto current = table_->current_key()) context.current_row_library = current->library;
    context.history_library = history_->library();
    context.navigator_library = navigator_->current_library();
    context.last_edited_library = edit_controller_->last_edited_library();
    for (const auto& info : session_.libraries())
        context.open_libraries.push_back(info.uuid);
    return UndoRouter::choose(context);
}

void MainWindow::undo() {
    if (auto library = undo_library())
        edit_controller_->undo(*library);
    else
        show_status(tr("Select a sample or library to undo in."));
}

void MainWindow::redo() {
    if (auto library = undo_library())
        edit_controller_->redo(*library);
    else
        show_status(tr("Select a sample or library to redo in."));
}

std::optional<std::vector<SampleKey>> MainWindow::complete_selection(const QString& action) {
    const std::size_t selected = table_->selected_row_count();
    if (selected == 0) {
        show_status(tr("Select one or more samples first."));
        return std::nullopt;
    }
    // Every page keeps its rows' keys, evicted or not, so this covers the whole selection.
    std::vector<SampleKey> keys = table_->selected_keys();
    if (keys.size() < selected) {
        // Only possible while the rows themselves are still arriving: acting on part of the
        // selection would change fewer samples than the status bar says are selected.
        show_failure(tr("%1: nothing was changed").arg(action),
                     tr("%1 of the %2 selected rows are still loading. Try again in a moment.")
                         .arg(DisplayFormat::count(static_cast<std::int64_t>(selected - keys.size())),
                              DisplayFormat::count(static_cast<std::int64_t>(selected))));
        return std::nullopt;
    }
    return keys;
}

void MainWindow::edit_selection(catalog::EditCommand command, const QString& action, const QString& done_message) {
    if (auto keys = complete_selection(action)) edit_controller_->apply(*keys, std::move(command), done_message);
}

void MainWindow::remove_selection_from_library() {
    catalog::EditCommand command;
    command.removed = true;
    edit_selection(command, tr("Remove from library"),
                   tr("Removed from the library; files untouched; undo to restore"));
}

void MainWindow::toggle_favorite() {
    const auto keys = complete_selection(tr("Toggle favorite"));
    if (!keys) return;
    // Decided from the rows in memory (an evicted row's flag is not known without reading it): when
    // every loaded selected row is a favorite, the command unmarks the whole selection.
    const auto samples = table_->selected_samples();
    const bool all_favorite =
        !samples.empty() && std::ranges::all_of(samples, [](const auto& sample) { return sample.favorite; });
    catalog::EditCommand command;
    command.favorite = !all_favorite;
    edit_controller_->apply(*keys, command, all_favorite ? tr("Unmarked favorite") : tr("Marked favorite"));
}

void MainWindow::set_rating(int rating) {
    catalog::EditCommand command;
    command.rating = rating;
    edit_selection(command, tr("Rating"), rating == 0 ? tr("Cleared rating") : tr("Rated %n star(s)", nullptr, rating));
}

void MainWindow::create_smart_collection(const uuids::uuid& library, const QString& name) {
    const QString query = search_bar_->text().trimmed();
    if (query.isEmpty()) {
        show_failure(tr("New smart collection"), tr("Type a search first; the smart collection keeps that query."));
        return;
    }
    // Refuse a query that does not parse (the error names the position), instead of storing it and
    // failing every later search of the collection.
    if (auto valid = search::SearchService().validate(DisplayFormat::to_utf8(query)); !valid) {
        show_failure(tr("New smart collection"), QString::fromStdString(valid.error().safe_detail));
        return;
    }
    catalog::TaxonomyCommand command;
    command.kind = catalog::TaxonomyCommand::Kind::CreateCollection;
    command.new_name = DisplayFormat::to_utf8(name);
    command.smart_query = DisplayFormat::to_utf8(query);
    edit_controller_->apply_taxonomy(library, command);
}

void MainWindow::quit() {
    close();
}

void MainWindow::play_pause() {
    preview_->toggle_play_pause();
}

void MainWindow::stop_preview() {
    preview_->stop();
}

void MainWindow::focus_search() {
    search_bar_->focus_and_select();
}

void MainWindow::analyze_waveforms() {
    analyze_selection("Waveforms", tr("Generate waveforms"));
}

void MainWindow::analyze_musical() {
    analyze_selection("Musical", tr("Musical analysis (BPM, key)"));
}

void MainWindow::analyze_duplicates() {
    analyze_selection("Duplicates", tr("Duplicate detection"));
}

void MainWindow::analyze_selection(const std::string& profile, const QString& description) {
    if (auto keys = complete_selection(description)) analysis_controller_->start(*keys, profile, description);
}

// ---------------------------------------------------------------- preferences and layout

void MainWindow::show_preferences() {
    PreferencesDialog dialog(shortcuts_, audition_on_selection_, this);
    if (dialog.exec() != QDialog::Accepted) return;
    shortcuts_ = dialog.bindings();
    audition_on_selection_ = dialog.audition_on_selection();
    apply_shortcuts(shortcuts_);
    session_.set_preference(ShortcutCatalog::kPreferenceKey, ShortcutCatalog::to_json(shortcuts_));
    session_.set_preference(UiPreferenceKeys::kAuditionOnSelection, audition_on_selection_);
}

void MainWindow::apply_shortcuts(const ShortcutBindings& bindings) {
    for (const auto& [id, action] : commands_) {
        const auto found = bindings.find(id);
        const QKeySequence sequence = found != bindings.end() ? found->second : QKeySequence();
        action->setShortcut(sequence);
        // Space, Delete, F, digits — and any command the user rebound to a single key — act only
        // while the table has focus, never while the user types in the navigator, inspector or
        // search box. Only modifier shortcuts are window-wide.
        const bool table_scoped =
            ShortcutCatalog::scope(id) == ShortcutScope::SampleTable || ShortcutCatalog::is_single_key(sequence);
        if (table_scoped) {
            removeAction(action);
            table_->addAction(action);
        } else {
            table_->removeAction(action);
            addAction(action);  // keyboard-only commands work without a menu entry
        }
        action->setShortcutContext(table_scoped ? Qt::WidgetWithChildrenShortcut : Qt::WindowShortcut);
    }
}

void MainWindow::sync_sort_with_header() {
    // A restored header shows a sort indicator; make the model query with that sort.
    const QHeaderView* header = table_->horizontalHeader();
    model_->sort(header->sortIndicatorSection(), header->sortIndicatorOrder());
}

void MainWindow::show_about() {
    AboutDialog dialog(this);
    dialog.exec();
}

// ---------------------------------------------------------------- helpers

std::optional<uuids::uuid> MainWindow::choose_library(const QString& purpose, bool writable_only) {
    if (session_.libraries().empty()) {
        show_failure(purpose, tr("No library is open."));
        return std::nullopt;
    }
    std::optional<uuids::uuid> preferred =
        table_->current_key() ? std::optional(table_->current_key()->library) : navigator_->current_library();
    return LibraryController::choose_library(this, session_.libraries(), purpose, writable_only, preferred);
}

void MainWindow::show_failure(const QString& title, const QString& details) {
    notification_bar_->show_error(title, details);
    show_status(title);
}

void MainWindow::show_status(const QString& message) {
    statusBar()->showMessage(message, kStatusMessageMilliseconds);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    export_controller_->cancel_all();
    preview_->stop();
    layout_controller_->save_current();
    session_.flush_preferences();
    event->accept();
}

}  // namespace mso::ui
