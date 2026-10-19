#include "mso/ui/jobs_panel.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <string>

#include "mso/ui/display_format.hpp"

namespace mso::ui {

namespace {

enum Column { TaskColumn = 0, ProgressColumn = 1, StatusColumn = 2 };

/// What a row represents, stored in TaskColumn's item data.
enum TaskKind { ImportTask = 1, AnalysisTask, LocalTask, LibraryOpenTask };

enum ItemRole {
    KindRole = Qt::UserRole + 1,
    OperationRole,  ///< import: OperationId; local task: task number
    LibraryRole,    ///< analysis: library UUID text
    RunRole,        ///< analysis: run id
    StateRole,      ///< analysis: run state text; others: "running" / "finished"
    StepEndedRole,  ///< library open: the last reported step has ended (bool)
};

bool is_finished_state(const QString& state) {
    return state == QLatin1String("finished") || state == QLatin1String("completed") ||
           state == QLatin1String("completed_with_errors") || state == QLatin1String("cancelled") ||
           state == QLatin1String("failed");
}

}  // namespace

JobsPanel::JobsPanel(QWidget* parent) : QWidget(parent) {
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("Task"), tr("Progress"), tr("Status")});
    tree_->setRootIsDecorated(false);
    tree_->setAccessibleName(tr("Background jobs"));
    tree_->header()->setSectionResizeMode(TaskColumn, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(ProgressColumn, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(StatusColumn, QHeaderView::Stretch);
    connect(tree_, &QTreeWidget::currentItemChanged, this, &JobsPanel::update_buttons);

    cancel_button_ = new QPushButton(tr("Cancel"), this);
    cancel_button_->setAccessibleName(tr("Cancel selected job"));
    pause_button_ = new QPushButton(tr("Pause"), this);
    pause_button_->setAccessibleName(tr("Pause selected analysis run"));
    resume_button_ = new QPushButton(tr("Resume"), this);
    resume_button_->setAccessibleName(tr("Resume selected analysis run"));
    pause_all_button_ = new QPushButton(tr("Pause all analysis"), this);
    clear_button_ = new QPushButton(tr("Clear finished"), this);
    worker_label_ = new QLabel(this);
    worker_label_->setWordWrap(true);
    worker_label_->setTextFormat(Qt::PlainText);  // library names and file names are user text
    worker_label_->setAccessibleName(tr("Analysis worker status"));

    connect(cancel_button_, &QPushButton::clicked, this, [this] {
        QTreeWidgetItem* item = tree_->currentItem();
        if (!item) return;
        const int kind = item->data(TaskColumn, KindRole).toInt();
        if (kind == ImportTask) {
            emit cancelOperationRequested(item->data(TaskColumn, OperationRole).toULongLong());
        } else if (kind == LocalTask) {
            const auto found = local_cancellers_.find(item->data(TaskColumn, OperationRole).toULongLong());
            if (found != local_cancellers_.end() && found->second) found->second();
        } else if (kind == AnalysisTask) {
            if (auto library = DisplayFormat::to_uuid(item->data(TaskColumn, LibraryRole).toString()))
                emit analysisControlRequested(*library, item->data(TaskColumn, RunRole).toLongLong(),
                                              catalog::Library::RunControl::Cancel);
        }
        item->setText(StatusColumn, tr("Cancelling…"));
    });
    for (auto [button, control] : {std::pair{pause_button_, catalog::Library::RunControl::Pause},
                                   std::pair{resume_button_, catalog::Library::RunControl::Resume}}) {
        connect(button, &QPushButton::clicked, this, [this, control] {
            QTreeWidgetItem* item = tree_->currentItem();
            if (!item || item->data(TaskColumn, KindRole).toInt() != AnalysisTask) return;
            if (auto library = DisplayFormat::to_uuid(item->data(TaskColumn, LibraryRole).toString()))
                emit analysisControlRequested(*library, item->data(TaskColumn, RunRole).toLongLong(), control);
        });
    }
    connect(pause_all_button_, &QPushButton::clicked, this, [this] { emit analysisPauseRequested(!analysis_paused_); });
    connect(clear_button_, &QPushButton::clicked, this, [this] {
        for (int index = tree_->topLevelItemCount() - 1; index >= 0; --index) {
            QTreeWidgetItem* item = tree_->topLevelItem(index);
            if (!is_finished_state(item->data(TaskColumn, StateRole).toString())) continue;
            forget(item);
            delete item;
        }
        update_buttons();
    });

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(cancel_button_);
    buttons->addWidget(pause_button_);
    buttons->addWidget(resume_button_);
    buttons->addStretch(1);
    buttons->addWidget(pause_all_button_);
    buttons->addWidget(clear_button_);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tree_, 1);
    layout->addLayout(buttons);
    layout->addWidget(worker_label_);
    update_buttons();
}

QTreeWidgetItem* JobsPanel::make_task_item(const QString& description) {
    auto* item = new QTreeWidgetItem(tree_, QStringList{description, QString(), tr("Starting…")});
    item->setData(TaskColumn, StateRole, QStringLiteral("running"));
    item->setToolTip(TaskColumn, DisplayFormat::plain_tooltip(description));
    return item;
}

void JobsPanel::set_progress(QTreeWidgetItem* item, std::int64_t done, std::int64_t total) {
    if (total <= 0) {
        item->setText(ProgressColumn, DisplayFormat::count(done));
        return;
    }
    const auto percent = static_cast<int>(std::clamp<std::int64_t>(done * 100 / total, 0, 100));
    item->setText(ProgressColumn,
                  tr("%1% (%2 / %3)").arg(percent).arg(DisplayFormat::count(done), DisplayFormat::count(total)));
}

void JobsPanel::track_import(app::OperationId operation, const QString& description) {
    if (imports_.contains(operation)) return;
    QTreeWidgetItem* item = make_task_item(description);
    item->setData(TaskColumn, KindRole, ImportTask);
    item->setData(TaskColumn, OperationRole, static_cast<qulonglong>(operation));
    imports_[operation] = item;
    update_buttons();
}

void JobsPanel::update_import(const app::ImportProgressEvent& event) {
    auto found = imports_.find(event.operation);
    if (found == imports_.end()) {
        track_import(event.operation, tr("Import"));
        found = imports_.find(event.operation);
    }
    QTreeWidgetItem* item = found->second;
    const files::ImportProgress& progress = event.progress;
    // Discovery continues while committing, so the total grows; it is labeled "found so far".
    set_progress(item, progress.files_committed, progress.files_discovered);
    // Each part is a whole phrase for translators; the locale joins them into one list.
    QStringList parts{tr("%1 imported").arg(DisplayFormat::count(progress.files_committed)),
                      tr("%1 found so far").arg(DisplayFormat::count(progress.files_discovered)),
                      tr("%1 directories").arg(DisplayFormat::count(progress.directories_scanned))};
    if (progress.probe_failures > 0) parts << tr("%1 unreadable").arg(DisplayFormat::count(progress.probe_failures));
    if (progress.inaccessible_directories > 0)
        parts << tr("%1 inaccessible folders").arg(DisplayFormat::count(progress.inaccessible_directories));
    item->setText(StatusColumn, QLocale().createSeparatedList(parts));
    item->setToolTip(StatusColumn, DisplayFormat::plain_tooltip(DisplayFormat::from_utf8(progress.current_directory)));
}

void JobsPanel::finish_operation(const app::OperationFinishedEvent& event) {
    auto found = imports_.find(event.operation);
    QTreeWidgetItem* item = found != imports_.end() ? found->second : nullptr;
    if (!item) {
        item = make_task_item(DisplayFormat::from_utf8(event.description));
        item->setData(TaskColumn, KindRole, ImportTask);
        item->setData(TaskColumn, OperationRole, static_cast<qulonglong>(event.operation));
        imports_[event.operation] = item;
    }
    item->setData(TaskColumn, StateRole, QStringLiteral("finished"));
    QString summary = DisplayFormat::from_utf8(event.description);
    if (!event.error)
        summary = tr("Done. %1").arg(summary);
    else if (event.error->code == ErrorCode::Cancelled)
        summary = tr("Cancelled; work done so far is kept. %1").arg(summary);
    else
        summary = tr("Failed: %1").arg(DisplayFormat::error(*event.error));
    item->setText(StatusColumn, summary);
    item->setToolTip(StatusColumn, DisplayFormat::plain_tooltip(summary));
    prune_finished_rows();
    update_buttons();
}

void JobsPanel::update_analysis(const app::AnalysisProgressEvent& event, const QString& library_name) {
    const QString library_text = DisplayFormat::from_uuid(event.library);
    std::set<std::int64_t> present;
    for (const auto& run : event.runs) {
        present.insert(run.id);
        const RunKey key{event.library, run.id};
        if (dismissed_runs_.contains(key)) continue;  // cleared by the user: do not bring it back
        QTreeWidgetItem*& item = analysis_runs_[key];
        if (!item) {
            item =
                make_task_item(tr("Analysis “%1” in %2").arg(DisplayFormat::from_utf8(run.profile_name), library_name));
            item->setData(TaskColumn, KindRole, AnalysisTask);
            item->setData(TaskColumn, LibraryRole, library_text);
            item->setData(TaskColumn, RunRole, static_cast<qlonglong>(run.id));
        }
        const std::int64_t done = run.completed + run.failed + run.skipped + run.cancelled;
        const std::int64_t total = done + run.queued + run.running + run.blocked + run.paused;
        set_progress(item, done, total);
        const QString state = DisplayFormat::from_utf8(run.state);
        item->setData(TaskColumn, StateRole, state);
        QStringList counts{tr("%1 done").arg(DisplayFormat::count(run.completed)),
                           tr("%1 failed").arg(DisplayFormat::count(run.failed)),
                           tr("%1 skipped").arg(DisplayFormat::count(run.skipped)),
                           tr("%1 queued").arg(DisplayFormat::count(run.queued)),
                           tr("%1 running").arg(DisplayFormat::count(run.running))};
        if (run.paused > 0) counts << tr("%1 paused").arg(DisplayFormat::count(run.paused));
        if (run.blocked > 0) counts << tr("%1 waiting for their files").arg(DisplayFormat::count(run.blocked));
        item->setText(StatusColumn,
                      tr("%1 — %2").arg(DisplayFormat::run_state(run.state), QLocale().createSeparatedList(counts)));
    }
    // Runs the Application no longer reports have ended; dismissals of such runs can be forgotten.
    for (auto& [key, item] : analysis_runs_) {
        if (key.first != event.library || present.contains(key.second)) continue;
        if (!is_finished_state(item->data(TaskColumn, StateRole).toString())) {
            item->setData(TaskColumn, StateRole, QStringLiteral("finished"));
            item->setText(StatusColumn, tr("Finished"));
        }
    }
    std::erase_if(dismissed_runs_, [&event, &present](const RunKey& key) {
        return key.first == event.library && !present.contains(key.second);
    });

    worker_status_[event.library] = {library_name, DisplayFormat::from_utf8(event.worker_status)};
    update_worker_label();
    prune_finished_rows();
    update_buttons();
}

void JobsPanel::update_worker_label() {
    QStringList lines;
    for (const auto& [library, status] : worker_status_)
        if (!status.second.isEmpty()) lines << tr("%1: %2").arg(status.first, status.second);
    worker_label_->setText(lines.join(QLatin1Char('\n')));
}

void JobsPanel::prune_finished_rows() {
    int finished = 0;
    for (int index = 0; index < tree_->topLevelItemCount(); ++index)
        if (is_finished_state(tree_->topLevelItem(index)->data(TaskColumn, StateRole).toString())) ++finished;
    // The oldest finished rows go first; running jobs are never removed.
    for (int index = 0; index < tree_->topLevelItemCount() && finished > kMaxFinishedRows;) {
        QTreeWidgetItem* item = tree_->topLevelItem(index);
        if (!is_finished_state(item->data(TaskColumn, StateRole).toString())) {
            ++index;
            continue;
        }
        forget(item);
        delete item;
        --finished;
    }
}

void JobsPanel::forget(QTreeWidgetItem* item) {
    const int kind = item->data(TaskColumn, KindRole).toInt();
    const auto number = item->data(TaskColumn, OperationRole).toULongLong();
    if (kind == ImportTask) imports_.erase(number);
    if (kind == LocalTask) {
        local_tasks_.erase(number);
        local_cancellers_.erase(number);
    }
    if (kind == AnalysisTask) {
        if (const auto library = DisplayFormat::to_uuid(item->data(TaskColumn, LibraryRole).toString())) {
            const RunKey key{*library, item->data(TaskColumn, RunRole).toLongLong()};
            analysis_runs_.erase(key);
            dismissed_runs_.insert(key);
        }
    }
    if (kind == LibraryOpenTask) {
        if (const auto library = DisplayFormat::to_uuid(item->data(TaskColumn, LibraryRole).toString()))
            library_opens_.erase(*library);
    }
}

std::uint64_t JobsPanel::start_local_task(const QString& description, std::function<void()> cancel) {
    const std::uint64_t task = next_local_task_++;
    QTreeWidgetItem* item = make_task_item(description);
    item->setData(TaskColumn, KindRole, LocalTask);
    item->setData(TaskColumn, OperationRole, static_cast<qulonglong>(task));
    item->setText(StatusColumn, tr("Running…"));
    local_tasks_[task] = item;
    local_cancellers_[task] = std::move(cancel);
    update_buttons();
    return task;
}

void JobsPanel::finish_local_task(std::uint64_t task, const QString& summary, bool succeeded) {
    const auto found = local_tasks_.find(task);
    if (found == local_tasks_.end()) return;
    found->second->setData(TaskColumn, StateRole, QStringLiteral("finished"));
    found->second->setText(StatusColumn, succeeded ? tr("Done. %1").arg(summary) : tr("Failed: %1").arg(summary));
    found->second->setToolTip(StatusColumn, DisplayFormat::plain_tooltip(summary));
    local_cancellers_.erase(task);
    prune_finished_rows();
    update_buttons();
}

void JobsPanel::retain_libraries(const std::vector<uuids::uuid>& open) {
    const auto is_open = [&open](const uuids::uuid& library) { return std::ranges::find(open, library) != open.end(); };
    for (auto& [key, item] : analysis_runs_) {
        if (is_open(key.first) || is_finished_state(item->data(TaskColumn, StateRole).toString())) continue;
        item->setData(TaskColumn, StateRole, QStringLiteral("finished"));
        item->setText(StatusColumn, tr("Library closed"));
    }
    std::erase_if(dismissed_runs_, [&is_open](const RunKey& key) { return !is_open(key.first); });
    std::erase_if(worker_status_, [&is_open](const auto& entry) { return !is_open(entry.first); });
    update_worker_label();
    prune_finished_rows();
    update_buttons();
}

void JobsPanel::update_library_open(const app::LibraryOpenProgressEvent& event) {
    QTreeWidgetItem*& item = library_opens_[event.library];
    if (!item) {
        // The library is not in the open list yet: its folder names it.
        item = make_task_item(tr("Upgrading library %1").arg(DisplayFormat::from_path(event.directory.filename())));
        item->setData(TaskColumn, KindRole, LibraryOpenTask);
        item->setData(TaskColumn, LibraryRole, DisplayFormat::from_uuid(event.library));
        item->setToolTip(TaskColumn, DisplayFormat::plain_tooltip(DisplayFormat::from_path(event.directory)));
    }
    item->setData(TaskColumn, StateRole, QStringLiteral("running"));  // a later open of the same library
    item->setData(TaskColumn, StepEndedRole, event.completed >= event.total);
    set_progress(item, event.completed, event.total);
    item->setText(StatusColumn, tr("%1…").arg(DisplayFormat::open_step(event.step)));
    update_buttons();
}

void JobsPanel::finish_completed_library_opens(const std::vector<uuids::uuid>& open) {
    for (const auto& [library, item] : library_opens_) {
        if (is_finished_state(item->data(TaskColumn, StateRole).toString())) continue;
        if (std::ranges::find(open, library) == open.end() || !item->data(TaskColumn, StepEndedRole).toBool())
            continue;  // still opening, or between two steps
        item->setData(TaskColumn, StateRole, QStringLiteral("finished"));
        item->setText(StatusColumn, tr("Done. The library is ready."));
    }
    prune_finished_rows();
    update_buttons();
}

void JobsPanel::fail_library_open(const uuids::uuid& library, const QString& reason) {
    const auto found = library_opens_.find(library);
    if (found == library_opens_.end() || is_finished_state(found->second->data(TaskColumn, StateRole).toString()))
        return;
    found->second->setData(TaskColumn, StateRole, QStringLiteral("failed"));
    found->second->setText(StatusColumn, tr("Failed: %1").arg(reason));
    found->second->setToolTip(StatusColumn, DisplayFormat::plain_tooltip(reason));
    prune_finished_rows();
    update_buttons();
}

void JobsPanel::fail_unopened_library_opens(const std::vector<uuids::uuid>& open, const QString& reason) {
    std::vector<uuids::uuid> failed;
    for (const auto& [library, item] : library_opens_)
        if (std::ranges::find(open, library) == open.end()) failed.push_back(library);
    for (const uuids::uuid& library : failed)
        fail_library_open(library, reason);
}

int JobsPanel::row_count() const {
    return tree_->topLevelItemCount();
}

void JobsPanel::set_analysis_paused(bool paused) {
    analysis_paused_ = paused;
    pause_all_button_->setText(paused ? tr("Resume all analysis") : tr("Pause all analysis"));
    pause_all_button_->setAccessibleName(pause_all_button_->text());
}

void JobsPanel::update_buttons() {
    const QTreeWidgetItem* item = tree_->currentItem();
    const int kind = item ? item->data(TaskColumn, KindRole).toInt() : 0;
    const QString state = item ? item->data(TaskColumn, StateRole).toString() : QString();
    const bool running = item && !is_finished_state(state);
    bool cancellable = running && (kind == ImportTask || kind == AnalysisTask);  // an open cannot stop
    if (running && kind == LocalTask) {
        const auto found = local_cancellers_.find(item->data(TaskColumn, OperationRole).toULongLong());
        cancellable = found != local_cancellers_.end() && static_cast<bool>(found->second);
    }
    cancel_button_->setEnabled(cancellable);
    pause_button_->setEnabled(running && kind == AnalysisTask && state != QLatin1String("paused"));
    resume_button_->setEnabled(running && kind == AnalysisTask && state == QLatin1String("paused"));
}

}  // namespace mso::ui
