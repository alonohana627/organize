#include "mso/ui/main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

#include "mso/app/application.hpp"
#include "mso/ui/add_folder_dialog.hpp"
#include "mso/ui/application_session.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/drag_path_resolver.hpp"
#include "mso/ui/empty_state_widget.hpp"
#include "mso/ui/file_manager.hpp"
#include "mso/ui/history_panel.hpp"
#include "mso/ui/inspector_panel.hpp"
#include "mso/ui/jobs_panel.hpp"
#include "mso/ui/layout_store.hpp"
#include "mso/ui/navigator_panel.hpp"
#include "mso/ui/preferences_dialog.hpp"
#include "mso/ui/preview_panel.hpp"
#include "mso/ui/sample_table_model.hpp"
#include "mso/ui/sample_table_view.hpp"
#include "mso/ui/search_bar.hpp"

namespace mso::ui {

namespace {

constexpr int kRefreshThrottleMilliseconds = 750;
constexpr int kNavigatorThrottleMilliseconds = 2000;
constexpr int kSelectionCoalesceMilliseconds = 40;
constexpr int kStatusMessageMilliseconds = 6000;
constexpr const char* kAuditionPreference = "preview.audition_on_selection";

/// Groups keys by library, preserving each library's selection order.
std::map<uuids::uuid, std::vector<std::int64_t>> group_by_library(const std::vector<SampleKey>& keys) {
    std::map<uuids::uuid, std::vector<std::int64_t>> groups;
    for (const SampleKey& key : keys)
        groups[key.library].push_back(key.sample_id);
    return groups;
}

/// Accumulates the per-library outcomes of one user action that spans several libraries.
struct MultiLibraryOutcome {
    std::size_t remaining = 0;
    std::int64_t applied = 0;
    std::int64_t conflicted = 0;
    QStringList failures;
};

}  // namespace

MainWindow::MainWindow(ApplicationSession& session, QWidget* parent) : QMainWindow(parent), session_(session) {
    setWindowTitle(tr("Music Sample Organizer"));
    setObjectName(QStringLiteral("MainWindow"));
    setDockNestingEnabled(true);
    resize(1400, 860);

    refresh_timer_.setSingleShot(true);
    refresh_timer_.setInterval(kRefreshThrottleMilliseconds);
    connect(&refresh_timer_, &QTimer::timeout, this, &MainWindow::refresh_results);
    navigator_timer_.setSingleShot(true);
    navigator_timer_.setInterval(kNavigatorThrottleMilliseconds);
    connect(&navigator_timer_, &QTimer::timeout, this, [this] {
        navigator_->reload();
        history_->reload();
    });
    selection_timer_.setSingleShot(true);
    selection_timer_.setInterval(kSelectionCoalesceMilliseconds);
    connect(&selection_timer_, &QTimer::timeout, this, &MainWindow::on_selection_changed);

    build_central();
    build_docks();
    build_actions();
    build_menus();
    build_status_bar();
    connect_session();
    apply_shortcuts(shortcuts_);

    default_state_ = saveState(LayoutStore::kSchemaVersion);
    default_header_state_ = table_->horizontalHeader()->saveState();
    update_status_labels();
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------- construction

void MainWindow::build_central() {
    model_ = new SampleTableModel(session_, this);
    table_ = new SampleTableView;
    table_->set_sample_model(model_);
    drag_resolver_ = new DragPathResolver(session_, this);
    table_->set_drag_resolver(drag_resolver_);
    search_bar_ = new SearchBar;

    browser_page_ = new QWidget;
    auto* browser_layout = new QVBoxLayout(browser_page_);
    browser_layout->setContentsMargins(4, 4, 4, 0);
    browser_layout->addWidget(search_bar_);
    browser_layout->addWidget(table_, 1);

    empty_state_ = new EmptyStateWidget;
    central_stack_ = new QStackedWidget(this);
    central_stack_->addWidget(empty_state_);
    central_stack_->addWidget(browser_page_);
    setCentralWidget(central_stack_);

    connect(empty_state_, &EmptyStateWidget::addFolderRequested, this, &MainWindow::add_folder);
    connect(empty_state_, &EmptyStateWidget::openLibraryRequested, this, &MainWindow::open_library);
    connect(search_bar_, &SearchBar::querySubmitted, this, [this] { apply_query(); });

    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { selection_timer_.start(); });
    connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { selection_timer_.start(); });
    connect(table_, &QAbstractItemView::doubleClicked, this, [this] { preview_->play_selected(); });
    connect(table_, &SampleTableView::dragRefused, this, &MainWindow::show_status);
    connect(table_, &SampleTableView::sampleMenuRequested, this, &MainWindow::show_sample_menu);
    connect(table_, &SampleTableView::ratingClicked, this, [this](const SampleKey& key, int rating) {
        catalog::EditCommand command;
        command.rating = rating;
        apply_edit_to_keys({key}, command, rating == 0 ? tr("Cleared rating") : tr("Rated %1").arg(rating));
    });
    connect(table_, &SampleTableView::mixedDragRefused, this, [this](std::size_t available, std::size_t unavailable) {
        QMessageBox box(QMessageBox::Warning, tr("Some samples are unavailable"),
                        tr("%1 of the selected samples are unavailable (offline or missing), so nothing was dragged.")
                            .arg(DisplayFormat::count(static_cast<std::int64_t>(unavailable))),
                        QMessageBox::Cancel, this);
        QPushButton* select_available =
            available > 0 ? box.addButton(tr("Select the %1 available samples")
                                              .arg(DisplayFormat::count(static_cast<std::int64_t>(available))),
                                          QMessageBox::AcceptRole)
                          : nullptr;
        box.exec();
        if (select_available && box.clickedButton() == select_available) {
            table_->select_keys(drag_resolver_->available_keys(), table_->current_key());
            show_status(tr("Selected the available samples; drag them again."));
        }
    });

    connect(model_, &SampleTableModel::countUpdating, this, [this] {
        count_text_ = tr("%1 results so far (counting…)").arg(DisplayFormat::count(model_->rowCount()));
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
    connect(model_, &SampleTableModel::refreshed, this, [this] {
        table_->select_keys(saved_selection_, saved_current_);
        selection_timer_.start();
    });
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

    connect(navigator_, &NavigatorPanel::scopeSelected, this, [this](const NavigationScope& scope) {
        scope_ = scope;
        apply_query();
    });
    connect(navigator_, &NavigatorPanel::taxonomyCommandRequested, this, &MainWindow::apply_taxonomy);
    connect(navigator_, &NavigatorPanel::smartCollectionRequested, this, &MainWindow::create_smart_collection);
    connect(navigator_, &NavigatorPanel::addFolderRequested, this,
            [this](const uuids::uuid& library) { add_folder_to(library); });
    connect(navigator_, &NavigatorPanel::rescanRequested, this,
            [this](const uuids::uuid& library, std::int64_t root_id) { start_import(library, root_id, tr("Rescan")); });
    connect(navigator_, &NavigatorPanel::rebindRequested, this, &MainWindow::rebind_root);
    connect(navigator_, &NavigatorPanel::closeLibraryRequested, this, &MainWindow::close_library_by_id);
    connect(navigator_, &NavigatorPanel::backupRequested, this, &MainWindow::backup_library_by_id);
    connect(navigator_, &NavigatorPanel::statusMessage, this, &MainWindow::show_status);

    connect(inspector_, &InspectorPanel::editRequested, this,
            [this](const catalog::EditCommand& command) { apply_edit_to_selection(command, tr("Saved")); });
    connect(preview_, &PreviewPanel::statusMessage, this, &MainWindow::show_status);
    connect(history_, &HistoryPanel::undoRequested, this, &MainWindow::undo);
    connect(history_, &HistoryPanel::redoRequested, this, &MainWindow::redo);

    connect(jobs_, &JobsPanel::cancelOperationRequested, this, [this](app::OperationId operation) {
        session_.run_detached([operation](app::Application& application) { application.cancel_operation(operation); });
    });
    connect(jobs_, &JobsPanel::analysisControlRequested, this,
            [this](const uuids::uuid& library, std::int64_t run_id, catalog::Library::RunControl control) {
                session_.run(
                    this,
                    [library, run_id, control](app::Application& application) {
                        return application.control_analysis(library, run_id, control);
                    },
                    [this](Result<void> result) {
                        if (!result)
                            show_status(tr("Analysis control failed: %1").arg(DisplayFormat::error(result.error())));
                    });
            });
    connect(jobs_, &JobsPanel::analysisPauseRequested, this, &MainWindow::set_analysis_paused);
}

QAction* MainWindow::add_command(const QString& id, const QString& text, void (MainWindow::*handler)()) {
    auto* action = new QAction(text, this);
    action->setShortcutContext(Qt::WindowShortcut);
    connect(action, &QAction::triggered, this, handler);
    addAction(action);  // keyboard-only commands work without a menu entry
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
        auto* action = new QAction(rating == 0 ? tr("Clear Rating") : tr("Rate %1").arg(rating), this);
        action->setShortcutContext(Qt::WindowShortcut);
        connect(action, &QAction::triggered, this, [this, rating] { set_rating(rating); });
        addAction(action);
        commands_[QStringLiteral("rating_%1").arg(rating)] = action;
    }
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
    analysis->addAction(tr("Generate &Waveforms for Selection"), this, &MainWindow::analyze_waveforms);
    analysis->addAction(tr("&Musical Analysis (BPM, Key) for Selection"), this, &MainWindow::analyze_musical);
    analysis->addAction(tr("Find &Duplicates for Selection"), this, &MainWindow::analyze_duplicates);
    analysis->addSeparator();
    pause_analysis_action_ = analysis->addAction(tr("&Pause All Analysis"));
    pause_analysis_action_->setCheckable(true);
    connect(pause_analysis_action_, &QAction::triggered, this, &MainWindow::set_analysis_paused);

    view_menu_ = menuBar()->addMenu(tr("&View"));
    for (QDockWidget* dock : docks_)
        view_menu_->addAction(dock->toggleViewAction());
    view_menu_->addSeparator();
    view_menu_->addAction(tr("&Reset Layout"), this, &MainWindow::reset_layout);
    view_menu_->addAction(tr("&Save Layout Preset…"), this, &MainWindow::save_layout_preset);
    layout_presets_menu_ = view_menu_->addMenu(tr("Layout &Presets"));
    delete_presets_menu_ = view_menu_->addMenu(tr("&Delete Layout Preset"));
    rebuild_layout_presets_menu();

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About Music Sample Organizer"), this, &MainWindow::show_about);
}

void MainWindow::build_status_bar() {
    catalog_label_ = new QLabel(this);
    catalog_label_->setAccessibleName(tr("Catalog state"));
    count_label_ = new QLabel(this);
    count_label_->setAccessibleName(tr("Result count"));
    selection_label_ = new QLabel(this);
    selection_label_->setAccessibleName(tr("Selected count"));
    statusBar()->addPermanentWidget(catalog_label_, 1);
    statusBar()->addPermanentWidget(count_label_);
    statusBar()->addPermanentWidget(selection_label_);
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
    connect(&session_, &ApplicationSession::operationFinished, this, [this](const app::OperationFinishedEvent& event) {
        jobs_->finish_operation(event);
        show_status(event.error ? tr("%1: %2").arg(DisplayFormat::from_utf8(event.description),
                                                   DisplayFormat::error(*event.error))
                                : tr("Finished: %1").arg(DisplayFormat::from_utf8(event.description)));
        navigator_timer_.start();
    });
    connect(&session_, &ApplicationSession::analysisProgressed, this, [this](const app::AnalysisProgressEvent& event) {
        jobs_->update_analysis(event, session_.library_name(event.library));
    });
    connect(&session_, &ApplicationSession::rootAvailabilityChanged, this,
            [this](const app::RootAvailabilityEvent& event) {
                navigator_->set_root_availability(event.library, event.root_id, event.availability);
                on_catalog_changed();  // availability badges in the table change
            });
}

// ---------------------------------------------------------------- session and query state

void MainWindow::on_session_ready() {
    shortcuts_ = ShortcutCatalog::from_json(session_.preference(ShortcutCatalog::kPreferenceKey));
    apply_shortcuts(shortcuts_);
    audition_on_selection_ =
        session_.preference(kAuditionPreference).is_boolean() && session_.preference(kAuditionPreference).get<bool>();
    preview_->load_preferences();
    restore_layout(session_.preference(LayoutStore::kCurrentLayoutKey));
    rebuild_layout_presets_menu();
    session_.run(
        this, [](app::Application& application) { return application.analysis_paused(); },
        [this](bool paused) {
            pause_analysis_action_->setChecked(paused);
            jobs_->set_analysis_paused(paused);
        });
}

void MainWindow::on_libraries_changed() {
    // Until some library has a sample folder there is nothing to browse: show the onboarding page.
    const auto libraries = session_.libraries();
    const bool any_open =
        std::ranges::any_of(libraries, [](const app::OpenLibraryInfo& info) { return info.root_count > 0; });
    central_stack_->setCurrentWidget(any_open ? browser_page_ : empty_state_);
    if (!any_open) {
        empty_state_->show_welcome();
        model_->clear();
        preview_->select_sample(std::nullopt, {});
    }
    navigator_->reload();
    history_->reload();
    // The open libraries changed, so even an identical query covers different data now.
    if (any_open) model_->run_query(DisplayFormat::to_utf8(search_bar_->text()), scope_);
    update_status_labels();
}

void MainWindow::on_catalog_changed() {
    // Imports commit in batches; refresh at most every kRefreshThrottleMilliseconds.
    if (!refresh_timer_.isActive()) refresh_timer_.start();
    if (!navigator_timer_.isActive()) navigator_timer_.start();
}

void MainWindow::refresh_results() {
    saved_selection_ = table_->selected_keys();
    saved_current_ = table_->current_key();
    model_->refresh(table_->first_visible_row());
}

void MainWindow::apply_query() {
    if (session_.libraries().empty()) return;
    model_->set_query(DisplayFormat::to_utf8(search_bar_->text()), scope_);
    update_status_labels();
}

void MainWindow::on_selection_changed() {
    const std::vector<search::SampleRow> samples = table_->selected_samples();
    const std::size_t selected = table_->selected_row_count();
    std::vector<SampleKey> keys;
    bool read_only = false;
    for (const auto& sample : samples) {
        keys.push_back({sample.library, sample.sample_id});
        if (auto info = session_.library_info(sample.library); info && info->read_only) read_only = true;
    }
    drag_resolver_->resolve(keys);
    inspector_->show_selection(samples, selected);
    inspector_->set_read_only(read_only);

    const std::optional<SampleKey> current = table_->current_key();
    QString current_name;
    if (current) {
        for (const auto& sample : samples) {
            if (sample.sample_id == current->sample_id && sample.library == current->library)
                current_name =
                    DisplayFormat::from_utf8(sample.display_name.empty() ? sample.file_name : sample.display_name);
        }
        if (current_name.isEmpty())
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
    if (!session_.is_ready())
        catalog = tr("Opening workspace…");
    else if (libraries.empty())
        catalog = tr("No library open");
    else {
        catalog = libraries.size() == 1 ? tr("1 library open") : tr("%1 libraries open").arg(libraries.size());
        if (std::any_of(libraries.begin(), libraries.end(), [](const auto& info) { return info.read_only; }))
            catalog += tr(" (read-only library open)");
        if (!scope_.label.isEmpty()) catalog += tr(" · %1").arg(scope_.label);
    }
    if (!coverage_message_.isEmpty()) catalog += tr(" · %1").arg(coverage_message_);
    catalog_label_->setText(catalog);
    count_label_->setText(libraries.empty() ? QString() : count_text_);
    const std::size_t selected = table_->selected_row_count();
    selection_label_->setText(
        selected == 0 ? QString() : tr("%1 selected").arg(DisplayFormat::count(static_cast<std::int64_t>(selected))));
}

// ---------------------------------------------------------------- library commands

void MainWindow::new_library() {
    if (!session_.is_ready()) return;
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("New library"), tr("Library name:"), QLineEdit::Normal,
                                               tr("My Samples"), &accepted)
                             .trimmed();
    if (!accepted || name.isEmpty()) return;
    show_status(tr("Creating library…"));
    session_.run(
        this,
        [name = DisplayFormat::to_utf8(name)](app::Application& application) {
            return application.create_library_in_workspace(name);
        },
        [this](Result<app::OpenLibraryInfo> created) {
            if (!created) {
                show_error(tr("Could not create the library"), DisplayFormat::error(created.error()));
                return;
            }
            session_.refresh_libraries();
            add_folder_to(created->uuid);  // a library is only useful with samples in it
        });
}

void MainWindow::open_library() {
    if (!session_.is_ready()) return;
    const QString directory =
        QFileDialog::getExistingDirectory(this, tr("Open library folder"), {}, QFileDialog::ShowDirsOnly);
    if (directory.isEmpty()) return;
    if (!app::Application::is_library_folder(DisplayFormat::to_path(directory))) {
        // Most people pick their sample folder here: offer to add it instead of failing.
        const auto answer = QMessageBox::question(
            this, tr("Not a library"),
            tr("%1 is a folder of samples, not a library.\n\nAdd it as a sample folder? It will be scanned and "
               "analyzed automatically.")
                .arg(QDir::toNativeSeparators(directory)));
        if (answer == QMessageBox::Yes) add_folder_to(std::nullopt, directory);
        return;
    }
    show_status(tr("Opening library…"));
    session_.run(
        this,
        [path = DisplayFormat::to_path(directory)](app::Application& application) {
            return application.open_library(path);
        },
        [this](Result<app::OpenLibraryInfo> opened) {
            if (!opened) {
                show_error(tr("Could not open the library"), DisplayFormat::error(opened.error()));
                return;
            }
            show_status(opened->read_only ? tr("Opened %1 read-only").arg(DisplayFormat::from_utf8(opened->name))
                                          : tr("Opened %1").arg(DisplayFormat::from_utf8(opened->name)));
            session_.refresh_libraries();
        });
}

void MainWindow::close_library() {
    if (auto library = choose_library(tr("Close which library?"), false)) close_library_by_id(*library);
}

void MainWindow::close_library_by_id(const uuids::uuid& library) {
    preview_->stop();
    preview_->select_sample(std::nullopt, {});
    session_.run(
        this, [library](app::Application& application) { return application.close_library(library); },
        [this](Result<void> closed) {
            if (!closed) show_error(tr("Could not close the library"), DisplayFormat::error(closed.error()));
            session_.refresh_libraries();
        });
}

void MainWindow::add_folder() {
    add_folder_to(std::nullopt);
}

void MainWindow::add_folder_to(const std::optional<uuids::uuid>& preselected, const QString& chosen_folder) {
    if (!session_.is_ready()) return;
    QString folder = chosen_folder;
    if (folder.isEmpty())
        folder =
            QFileDialog::getExistingDirectory(this, tr("Choose a folder of samples"), {}, QFileDialog::ShowDirsOnly);
    if (folder.isEmpty()) return;

    std::vector<std::pair<QString, uuids::uuid>> writable;
    for (const auto& info : session_.libraries())
        if (!info.read_only) writable.emplace_back(DisplayFormat::from_utf8(info.name), info.uuid);
    if (writable.empty() && !preselected) {
        // No library yet: create the default one in the workspace, then continue with this folder.
        session_.run(
            this, [](app::Application& application) { return application.default_library(); },
            [this, folder](Result<app::OpenLibraryInfo> library) {
                if (!library) {
                    show_error(tr("Could not create a library"), DisplayFormat::error(library.error()));
                    return;
                }
                session_.refresh_libraries();
                add_folder_to(library->uuid, folder);
            });
        return;
    }

    std::optional<uuids::uuid> library = preselected;
    if (!library && writable.size() == 1) library = writable.front().second;
    if (!library) {
        AddFolderDialog dialog(QDir::toNativeSeparators(folder), writable, navigator_->current_library(), this);
        if (dialog.exec() != QDialog::Accepted) return;
        library = dialog.library();
        if (!library) return;
    }
    session_.run(
        this,
        [library = *library, path = DisplayFormat::to_path(folder),
         name = DisplayFormat::to_utf8(QFileInfo(folder).fileName())](app::Application& application) {
            return application.add_root(library, path, name);
        },
        [this, library = *library, folder](Result<catalog::RootRecord> root) {
            if (!root) {
                show_error(tr("Could not add the folder"), DisplayFormat::error(root.error()));
                return;
            }
            session_.refresh_libraries();  // switches from the onboarding page to the browser
            start_import(library, root->id, tr("Import %1").arg(QDir::toNativeSeparators(folder)));
        });
}

void MainWindow::rescan_root() {
    const auto library = navigator_->current_library();
    const auto root = navigator_->current_root();
    if (!library || !root) {
        show_error(tr("Rescan folder"), tr("Select a folder (or one of its subfolders) in the Navigator first."));
        return;
    }
    start_import(*library, *root, tr("Rescan"));
}

void MainWindow::start_import(const uuids::uuid& library, std::int64_t root_id, const QString& description) {
    session_.run(
        this, [library, root_id](app::Application& application) { return application.start_import(library, root_id); },
        [this, description](Result<app::OperationId> operation) {
            if (!operation) {
                show_error(tr("Could not start the import"), DisplayFormat::error(operation.error()));
                return;
            }
            jobs_->track_import(*operation, description);
            show_status(tr("%1 started; results appear while it runs.").arg(description));
        });
}

void MainWindow::rebind_root(const uuids::uuid& library, std::int64_t root_id) {
    const QString folder =
        QFileDialog::getExistingDirectory(this, tr("Locate the moved folder"), {}, QFileDialog::ShowDirsOnly);
    if (folder.isEmpty()) return;
    session_.run(
        this,
        [library, root_id, path = DisplayFormat::to_path(folder)](app::Application& application) {
            return application.rebind_root(library, root_id, path);
        },
        [this, library, root_id](Result<void> rebound) {
            if (!rebound) {
                show_error(tr("Could not locate the folder"), DisplayFormat::error(rebound.error()));
                return;
            }
            start_import(library, root_id, tr("Rescan after relocating"));
        });
}

void MainWindow::export_csv() {
    export_metadata(false);
}
void MainWindow::export_json() {
    export_metadata(true);
}

void MainWindow::export_metadata(bool json) {
    const auto library = choose_library(tr("Export which library?"), false);
    if (!library) return;
    QStringList scopes{tr("All samples")};
    const QString query = search_bar_->text().trimmed();
    if (!query.isEmpty()) scopes << tr("Only samples matching “%1”").arg(query);
    bool accepted = true;
    const QString scope = scopes.size() > 1
                              ? QInputDialog::getItem(this, tr("Export"), tr("Export:"), scopes, 0, false, &accepted)
                              : scopes.front();
    if (!accepted) return;
    const QString extension = json ? QStringLiteral("json") : QStringLiteral("csv");
    const QString destination =
        QFileDialog::getSaveFileName(this, json ? tr("Export metadata as JSON") : tr("Export metadata as CSV"),
                                     session_.library_name(*library) + QLatin1Char('.') + extension,
                                     json ? tr("JSON files (*.json)") : tr("CSV files (*.csv)"));
    if (destination.isEmpty()) return;

    app::ExportOptions options;
    options.format = json ? app::ExportFormat::Json : app::ExportFormat::Csv;
    if (scope != scopes.front()) options.query = DisplayFormat::to_utf8(query);
    auto stop = std::make_shared<std::stop_source>();
    const std::uint64_t task = jobs_->start_local_task(
        tr("Export %1 to %2").arg(session_.library_name(*library), destination), [stop] { stop->request_stop(); });
    running_exports_[task] = stop;
    session_.run(
        this,
        [library = *library, path = DisplayFormat::to_path(destination), options, stop](app::Application& application) {
            return application.export_metadata(library, path, options, stop->get_token());
        },
        [this, task](Result<app::ExportSummary> summary) {
            running_exports_.erase(task);
            if (!summary) {
                const bool cancelled = summary.error().code == ErrorCode::Cancelled;
                jobs_->finish_local_task(
                    task, cancelled ? tr("cancelled; no file was written") : DisplayFormat::error(summary.error()),
                    false);
                return;
            }
            const QString text = tr("%1 samples written to %2")
                                     .arg(DisplayFormat::count(summary->rows), DisplayFormat::from_path(summary->file));
            jobs_->finish_local_task(task, text, true);
            show_status(tr("Export finished: %1").arg(text));
        });
}

void MainWindow::backup_library() {
    if (auto library = choose_library(tr("Back up which library?"), false)) backup_library_by_id(*library);
}

void MainWindow::backup_library_by_id(const uuids::uuid& library) {
    const std::uint64_t task = jobs_->start_local_task(tr("Backup of %1").arg(session_.library_name(library)), {});
    session_.run(
        this,
        [library](app::Application& application) -> Result<std::filesystem::path> {
            catalog::Library* opened = application.library(library);
            if (!opened) return fail(ErrorCode::NotFound, "the library is no longer open");
            return opened->create_backup("manual");
        },
        [this, task](Result<std::filesystem::path> backup) {
            if (!backup) {
                jobs_->finish_local_task(task, DisplayFormat::error(backup.error()), false);
                return;
            }
            jobs_->finish_local_task(task, tr("verified backup at %1").arg(DisplayFormat::from_path(*backup)), true);
        });
}

// ---------------------------------------------------------------- edits and history

void MainWindow::show_sample_menu(const QPoint& global_position, int row) {
    if (row < 0) return;  // empty space below the rows: nothing to act on
    const std::vector<search::SampleRow> samples = table_->selected_samples();
    if (samples.empty()) return;
    std::vector<SampleKey> keys;
    bool all_available = true;
    for (const auto& sample : samples) {
        keys.push_back({sample.library, sample.sample_id});
        all_available = all_available && sample.availability == search::RowAvailability::Available;
    }
    // The clicked row is the current one (the view made it current before asking for the menu).
    const search::SampleRow* clicked = model_->loaded_row(row);
    if (!clicked) clicked = &samples.front();
    const SampleKey clicked_key{clicked->library, clicked->sample_id};
    const bool clicked_available = clicked->availability == search::RowAvailability::Available;
    const QString unavailable_reason = tr("Unavailable: the source folder is offline or the file is missing");

    QMenu menu(this);
    menu.setToolTipsVisible(true);
    QAction* play = menu.addAction(tr("Play"), this, [this] { preview_->play_selected(); });
    play->setEnabled(clicked_available);
    menu.addSeparator();
    QAction* reveal = menu.addAction(tr("Show in Explorer"), this, [this, clicked_key] { reveal_sample(clicked_key); });
    reveal->setEnabled(clicked_available);
    const bool single = samples.size() == 1;
    const QString count = DisplayFormat::count(static_cast<std::int64_t>(samples.size()));
    QAction* copy_path = menu.addAction(single ? tr("Copy path") : tr("Copy %1 paths").arg(count), this,
                                        [this, keys] { copy_paths(keys); });
    QAction* copy_file = menu.addAction(single ? tr("Copy file") : tr("Copy %1 files").arg(count), this,
                                        [this, keys] { copy_files(keys); });
    for (QAction* path_action : {reveal, copy_path, copy_file, play}) {
        const bool enabled = path_action == reveal || path_action == play ? clicked_available : all_available;
        path_action->setEnabled(enabled);
        if (!enabled) path_action->setToolTip(unavailable_reason);
    }
    menu.addSeparator();
    QMenu* rating = menu.addMenu(tr("Rating"));
    for (int stars = 0; stars <= 5; ++stars)
        rating->addAction(commands_.at(QStringLiteral("rating_%1").arg(stars)));
    menu.addAction(commands_.at(QStringLiteral("toggle_favorite")));
    QMenu* analysis = menu.addMenu(tr("Analyze"));
    analysis->addAction(tr("Generate Waveforms"), this, &MainWindow::analyze_waveforms);
    analysis->addAction(tr("Musical Analysis (BPM, Key)"), this, &MainWindow::analyze_musical);
    analysis->addAction(tr("Find Duplicates"), this, &MainWindow::analyze_duplicates);
    menu.addSeparator();
    menu.addAction(commands_.at(QStringLiteral("remove_from_library")));
    menu.exec(global_position);
}

void MainWindow::resolve_paths(const std::vector<SampleKey>& keys,
                               std::function<void(std::vector<std::filesystem::path>)> use) {
    session_.run(
        this,
        [keys](app::Application& application) {
            std::vector<Result<std::filesystem::path>> results;
            results.reserve(keys.size());
            for (const SampleKey& key : keys)
                results.push_back(application.resolve_playable_path(key.library, key.sample_id));
            return results;
        },
        [this, use = std::move(use)](std::vector<Result<std::filesystem::path>> results) {
            std::vector<std::filesystem::path> paths;
            std::size_t unavailable = 0;
            const Error* first_error = nullptr;
            for (const auto& result : results) {
                if (result) {
                    paths.push_back(*result);
                } else {
                    ++unavailable;
                    if (!first_error) first_error = &result.error();
                }
            }
            // Never act on part of the request silently (docs/01 §5.6).
            if (first_error) {
                show_status(tr("Nothing done: %1 of the samples are unavailable (%2).")
                                .arg(DisplayFormat::count(static_cast<std::int64_t>(unavailable)),
                                     DisplayFormat::error(*first_error)));
                return;
            }
            use(std::move(paths));
        });
}

void MainWindow::reveal_sample(const SampleKey& key) {
    resolve_paths({key}, [this](std::vector<std::filesystem::path> paths) {
        if (!paths.empty() && !FileManager::reveal_file(paths.front()))
            show_status(tr("Could not start the file manager."));
    });
}

void MainWindow::copy_paths(const std::vector<SampleKey>& keys) {
    resolve_paths(keys, [this](std::vector<std::filesystem::path> paths) {
        QStringList lines;
        for (const auto& path : paths)
            lines << DisplayFormat::from_path(path);
        QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
        show_status(lines.size() == 1 ? tr("Copied the path") : tr("Copied %1 paths").arg(lines.size()));
    });
}

void MainWindow::copy_files(const std::vector<SampleKey>& keys) {
    resolve_paths(keys, [this](std::vector<std::filesystem::path> paths) {
        QList<QUrl> urls;
        for (const auto& path : paths)
            urls << QUrl::fromLocalFile(QString::fromStdU16String(path.generic_u16string()));
        auto* data = new QMimeData;
        data->setUrls(urls);  // pasting in Explorer or a DAW copies the files themselves
        QGuiApplication::clipboard()->setMimeData(data);
        show_status(urls.size() == 1 ? tr("Copied the file") : tr("Copied %1 files").arg(urls.size()));
    });
}

void MainWindow::apply_edit_to_selection(catalog::EditCommand command, const QString& done_message) {
    const std::vector<SampleKey> keys = table_->selected_keys();
    if (keys.empty()) {
        show_status(tr("Select one or more loaded samples first."));
        return;
    }
    apply_edit_to_keys(keys, std::move(command), done_message);
}

void MainWindow::apply_edit_to_keys(const std::vector<SampleKey>& keys, catalog::EditCommand command,
                                    const QString& done_message) {
    const auto groups = group_by_library(keys);
    auto outcome = std::make_shared<MultiLibraryOutcome>();
    outcome->remaining = groups.size();
    for (const auto& [library, sample_ids] : groups) {
        catalog::EditCommand per_library = command;
        per_library.operation = new_uuid();  // independent, idempotent operation per library
        per_library.sample_ids = sample_ids;
        last_edited_library_ = library;
        session_.run(
            this,
            [library, per_library](app::Application& application) -> Result<catalog::MutationReceipt> {
                catalog::Library* opened = application.library(library);
                if (!opened) return fail(ErrorCode::NotFound, "the library is no longer open");
                return opened->apply_edit(per_library);
            },
            [this, outcome, library, done_message](Result<catalog::MutationReceipt> receipt) {
                if (receipt) {
                    outcome->applied += receipt->applied;
                    outcome->conflicted += receipt->conflicted;
                } else {
                    outcome->failures << tr("%1: %2").arg(session_.library_name(library),
                                                          DisplayFormat::error(receipt.error()));
                }
                if (--outcome->remaining > 0) return;
                QString message = tr("%1 (%2 samples)").arg(done_message, DisplayFormat::count(outcome->applied));
                if (outcome->conflicted > 0)
                    message += tr("; %1 skipped because of conflicts").arg(DisplayFormat::count(outcome->conflicted));
                if (!outcome->failures.isEmpty())
                    message += tr("; failed in %1").arg(outcome->failures.join(QStringLiteral("; ")));
                show_status(message);
                on_catalog_changed();
                history_->reload();
            });
    }
}

void MainWindow::apply_taxonomy(const uuids::uuid& library, const catalog::TaxonomyCommand& command) {
    last_edited_library_ = library;
    session_.run(
        this,
        [library, command](app::Application& application) -> Result<catalog::MutationReceipt> {
            catalog::Library* opened = application.library(library);
            if (!opened) return fail(ErrorCode::NotFound, "the library is no longer open");
            return opened->apply_taxonomy(command);
        },
        [this](Result<catalog::MutationReceipt> receipt) {
            if (!receipt) {
                show_error(tr("The change was not applied"), DisplayFormat::error(receipt.error()));
                return;
            }
            show_status(tr("Saved. Undo with %1.")
                            .arg(commands_.at(QStringLiteral("undo"))->shortcut().toString(QKeySequence::NativeText)));
            navigator_->reload();
            history_->reload();
            on_catalog_changed();
        });
}

void MainWindow::create_smart_collection(const uuids::uuid& library, const QString& name) {
    const QString query = search_bar_->text().trimmed();
    if (query.isEmpty()) {
        show_error(tr("New smart collection"), tr("Type a search first; the smart collection keeps that query."));
        return;
    }
    catalog::TaxonomyCommand command;
    command.kind = catalog::TaxonomyCommand::Kind::CreateCollection;
    command.new_name = DisplayFormat::to_utf8(name);
    command.smart_query = DisplayFormat::to_utf8(query);
    apply_taxonomy(library, command);
}

std::optional<uuids::uuid> MainWindow::undo_library() const {
    if (auto current = table_->current_key()) return current->library;
    if (history_->library()) return history_->library();
    if (auto library = navigator_->current_library()) return library;
    if (last_edited_library_ && session_.library_info(*last_edited_library_)) return last_edited_library_;
    if (session_.libraries().size() == 1) return session_.libraries().front().uuid;
    return std::nullopt;
}

void MainWindow::undo() {
    undo_or_redo(false);
}
void MainWindow::redo() {
    undo_or_redo(true);
}

void MainWindow::undo_or_redo(bool redo) {
    const auto library = undo_library();
    if (!library) {
        show_status(tr("Select a sample or library to undo in."));
        return;
    }
    session_.run(
        this,
        [library = *library, redo](app::Application& application) -> Result<catalog::MutationReceipt> {
            catalog::Library* opened = application.library(library);
            if (!opened) return fail(ErrorCode::NotFound, "the library is no longer open");
            return redo ? opened->redo() : opened->undo();
        },
        [this, redo, library = *library](Result<catalog::MutationReceipt> receipt) {
            if (!receipt) {
                show_status(receipt.error().code == ErrorCode::HistoryConflict
                                ? tr("Not undone: a later edit changed the same values (recorded for review).")
                                : tr("%1 failed: %2")
                                      .arg(redo ? tr("Redo") : tr("Undo"), DisplayFormat::error(receipt.error())));
            } else {
                show_status(redo ? tr("Redone in %1").arg(session_.library_name(library))
                                 : tr("Undone in %1").arg(session_.library_name(library)));
            }
            history_->reload();
            navigator_timer_.start();
            on_catalog_changed();
        });
}

void MainWindow::remove_selection_from_library() {
    const std::size_t count = table_->selected_keys().size();
    if (count == 0) return;
    catalog::EditCommand command;
    command.removed = true;
    apply_edit_to_selection(command, tr("Removed from the library; files untouched; undo to restore"));
}

void MainWindow::toggle_favorite() {
    const auto samples = table_->selected_samples();
    if (samples.empty()) return;
    const bool all_favorite =
        std::all_of(samples.begin(), samples.end(), [](const auto& sample) { return sample.favorite; });
    catalog::EditCommand command;
    command.favorite = !all_favorite;
    apply_edit_to_selection(command, all_favorite ? tr("Unmarked favorite") : tr("Marked favorite"));
}

void MainWindow::set_rating(int rating) {
    catalog::EditCommand command;
    command.rating = rating;
    apply_edit_to_selection(command, rating == 0 ? tr("Cleared rating") : tr("Rated %1").arg(rating));
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

// ---------------------------------------------------------------- analysis

void MainWindow::analyze_waveforms() {
    start_analysis("Waveforms", tr("Generate waveforms"));
}
void MainWindow::analyze_musical() {
    start_analysis("Musical", tr("Musical analysis (BPM, key)"));
}
void MainWindow::analyze_duplicates() {
    start_analysis("Duplicates", tr("Duplicate detection"));
}

void MainWindow::start_analysis(const std::string& profile, const QString& description) {
    const std::vector<SampleKey> keys = table_->selected_keys();
    if (keys.empty()) {
        show_status(tr("Select the samples to analyze first."));
        return;
    }
    const auto answer = QMessageBox::question(
        this, description,
        tr("%1 for %2 selected samples?\n\nEach file is decoded once in the background. Samples that already have "
           "current results are skipped. You can pause or cancel the run in the Jobs panel.")
            .arg(description, DisplayFormat::count(static_cast<std::int64_t>(keys.size()))));
    if (answer != QMessageBox::Yes) return;
    for (const auto& [library, sample_ids] : group_by_library(keys)) {
        session_.run(
            this,
            [library, profile, sample_ids](app::Application& application) {
                return application.start_analysis(library, profile, sample_ids);
            },
            [this, library, description](Result<catalog::Library::AnalysisRunStatus> run) {
                if (!run) {
                    show_error(tr("Could not start the analysis"),
                               tr("%1: %2").arg(session_.library_name(library), DisplayFormat::error(run.error())));
                    return;
                }
                show_status(tr("%1 queued in %2").arg(description, session_.library_name(library)));
            });
    }
}

void MainWindow::set_analysis_paused(bool paused) {
    pause_analysis_action_->setChecked(paused);
    jobs_->set_analysis_paused(paused);
    session_.run_detached([paused](app::Application& application) { application.set_analysis_paused(paused); });
    show_status(paused ? tr("Background analysis paused") : tr("Background analysis resumed"));
}

// ---------------------------------------------------------------- preferences and layout

void MainWindow::show_preferences() {
    PreferencesDialog dialog(shortcuts_, audition_on_selection_, this);
    if (dialog.exec() != QDialog::Accepted) return;
    shortcuts_ = dialog.bindings();
    audition_on_selection_ = dialog.audition_on_selection();
    apply_shortcuts(shortcuts_);
    session_.set_preference(ShortcutCatalog::kPreferenceKey, ShortcutCatalog::to_json(shortcuts_));
    session_.set_preference(kAuditionPreference, audition_on_selection_);
}

void MainWindow::apply_shortcuts(const ShortcutBindings& bindings) {
    for (const auto& [id, action] : commands_) {
        const auto found = bindings.find(id);
        action->setShortcut(found != bindings.end() ? found->second : QKeySequence());
    }
}

bool MainWindow::restore_layout(const nlohmann::json& layout) {
    if (!LayoutStore::restore(*this, *table_->horizontalHeader(), layout)) return false;
    sync_sort_with_header();
    return true;
}

void MainWindow::sync_sort_with_header() {
    // A restored header shows a sort indicator; make the model query with that sort.
    const QHeaderView* header = table_->horizontalHeader();
    model_->sort(header->sortIndicatorSection(), header->sortIndicatorOrder());
}

void MainWindow::reset_layout() {
    restoreState(default_state_, LayoutStore::kSchemaVersion);
    table_->horizontalHeader()->restoreState(default_header_state_);
    sync_sort_with_header();
    for (QDockWidget* dock : docks_)
        dock->show();
    show_status(tr("Layout reset"));
}

void MainWindow::save_layout_preset() {
    bool accepted = false;
    const QString name =
        QInputDialog::getText(this, tr("Save layout preset"), tr("Preset name:"), QLineEdit::Normal, {}, &accepted)
            .trimmed();
    if (!accepted || name.isEmpty()) return;
    nlohmann::json presets = session_.preference(LayoutStore::kPresetsKey);
    if (!presets.is_object()) presets = nlohmann::json::object();
    presets[DisplayFormat::to_utf8(name)] = LayoutStore::capture(*this, *table_->horizontalHeader());
    session_.set_preference(LayoutStore::kPresetsKey, presets);
    rebuild_layout_presets_menu();
    show_status(tr("Saved layout preset “%1”").arg(name));
}

void MainWindow::rebuild_layout_presets_menu() {
    layout_presets_menu_->clear();
    delete_presets_menu_->clear();
    const nlohmann::json presets = session_.preference(LayoutStore::kPresetsKey);
    if (!presets.is_object() || presets.empty()) {
        layout_presets_menu_->addAction(tr("(none saved)"))->setEnabled(false);
        delete_presets_menu_->addAction(tr("(none saved)"))->setEnabled(false);
        return;
    }
    for (const auto& [key, layout] : presets.items()) {
        const QString name = DisplayFormat::from_utf8(key);
        layout_presets_menu_->addAction(name, this, [this, layout] {
            if (!restore_layout(layout))
                show_status(tr("This preset was saved by an incompatible version and was not applied."));
        });
        delete_presets_menu_->addAction(name, this, [this, key] {
            nlohmann::json remaining = session_.preference(LayoutStore::kPresetsKey);
            if (remaining.is_object()) remaining.erase(key);
            session_.set_preference(LayoutStore::kPresetsKey, remaining);
            rebuild_layout_presets_menu();
        });
    }
}

void MainWindow::show_about() {
    QMessageBox::about(this, tr("About Music Sample Organizer"),
                       tr("<h3>Music Sample Organizer %1</h3>"
                          "<p>An offline sample library browser for music producers.</p>"
                          "<p>Licensed under the GNU General Public License, version 3 (GPL-3.0). "
                          "This program comes with ABSOLUTELY NO WARRANTY.</p>"
                          "<p><b>Third-party components</b></p><ul>"
                          "<li>Qt %2 — LGPL-3.0</li>"
                          "<li>SQLite 3.53.4 — public domain</li>"
                          "<li>nlohmann/json 3.12.0 — MIT</li>"
                          "<li>utf8proc 2.12.0 — MIT</li>"
                          "<li>stduuid 1.2.3 — MIT</li>"
                          "<li>PicoSHA2 1.0.1 — MIT</li>"
                          "<li>miniaudio 0.11.25 — public domain / MIT-0</li>"
                          "<li>pocketfft (cpp) — BSD-3-Clause</li>"
                          "<li>BS::thread_pool 5.1.0 — MIT</li>"
                          "<li>readerwriterqueue 1.0.7 — BSD-2-Clause</li>"
                          "<li>FFmpeg (when available) — LGPL</li></ul>")
                           .arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion())));
}

// ---------------------------------------------------------------- helpers

std::optional<uuids::uuid> MainWindow::choose_library(const QString& purpose, bool writable_only) {
    std::vector<app::OpenLibraryInfo> candidates;
    for (const auto& info : session_.libraries())
        if (!writable_only || !info.read_only) candidates.push_back(info);
    if (candidates.empty()) {
        show_error(purpose, tr("No library is open."));
        return std::nullopt;
    }
    if (candidates.size() == 1) return candidates.front().uuid;
    std::optional<uuids::uuid> preferred =
        table_->current_key() ? std::optional(table_->current_key()->library) : navigator_->current_library();
    QStringList names;
    int preferred_index = 0;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        names << DisplayFormat::from_utf8(candidates[index].name);
        if (preferred && candidates[index].uuid == *preferred) preferred_index = static_cast<int>(index);
    }
    bool accepted = false;
    const QString choice =
        QInputDialog::getItem(this, tr("Choose library"), purpose, names, preferred_index, false, &accepted);
    if (!accepted) return std::nullopt;
    return candidates[static_cast<std::size_t>(names.indexOf(choice))].uuid;
}

void MainWindow::show_error(const QString& title, const QString& message) {
    QMessageBox::warning(this, title, message);
}

void MainWindow::show_status(const QString& message) {
    statusBar()->showMessage(message, kStatusMessageMilliseconds);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    for (const auto& [task, stop] : running_exports_)
        stop->request_stop();
    preview_->stop();
    if (session_.is_ready())
        session_.set_preference(LayoutStore::kCurrentLayoutKey,
                                LayoutStore::capture(*this, *table_->horizontalHeader()));
    event->accept();
}

}  // namespace mso::ui
