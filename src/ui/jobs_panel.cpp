#include "mso/ui/jobs_panel.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
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
enum TaskKind { ImportTask = 1, AnalysisTask, LocalTask };

enum ItemRole {
    KindRole = Qt::UserRole + 1,
    OperationRole,  ///< import: OperationId; local task: task number
    LibraryRole,    ///< analysis: library UUID text
    RunRole,        ///< analysis: run id
    StateRole,      ///< analysis: run state text; others: "running" / "finished"
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
            const int kind = item->data(TaskColumn, KindRole).toInt();
            const auto number = item->data(TaskColumn, OperationRole).toULongLong();
            if (kind == ImportTask) imports_.erase(number);
            if (kind == LocalTask) {
                local_tasks_.erase(number);
                local_cancellers_.erase(number);
            }
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
    item->setToolTip(TaskColumn, description);
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
    QString status =
        tr("%1 imported, %2 found so far, %3 directories")
            .arg(DisplayFormat::count(progress.files_committed), DisplayFormat::count(progress.files_discovered),
                 DisplayFormat::count(progress.directories_scanned));
    if (progress.probe_failures > 0) status += tr(", %1 unreadable").arg(DisplayFormat::count(progress.probe_failures));
    if (progress.inaccessible_directories > 0)
        status += tr(", %1 inaccessible folders").arg(DisplayFormat::count(progress.inaccessible_directories));
    item->setText(StatusColumn, status);
    item->setToolTip(StatusColumn, DisplayFormat::from_utf8(progress.current_directory));
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
    item->setToolTip(StatusColumn, summary);
    update_buttons();
}

void JobsPanel::update_analysis(const app::AnalysisProgressEvent& event, const QString& library_name) {
    const QString library_text = DisplayFormat::from_uuid(event.library);
    std::set<std::int64_t> present;
    for (const auto& run : event.runs) {
        present.insert(run.id);
        QTreeWidgetItem* item = nullptr;
        for (int index = 0; index < tree_->topLevelItemCount() && !item; ++index) {
            QTreeWidgetItem* candidate = tree_->topLevelItem(index);
            if (candidate->data(TaskColumn, KindRole).toInt() == AnalysisTask &&
                candidate->data(TaskColumn, LibraryRole).toString() == library_text &&
                candidate->data(TaskColumn, RunRole).toLongLong() == run.id)
                item = candidate;
        }
        if (!item) {
            item =
                make_task_item(tr("Analysis “%1” in %2").arg(DisplayFormat::from_utf8(run.profile_name), library_name));
            item->setData(TaskColumn, KindRole, AnalysisTask);
            item->setData(TaskColumn, LibraryRole, library_text);
            item->setData(TaskColumn, RunRole, static_cast<qlonglong>(run.id));
        }
        const std::int64_t done = run.completed + run.failed + run.skipped + run.cancelled;
        const std::int64_t total = done + run.queued + run.running + run.blocked;
        set_progress(item, done, total);
        const QString state = DisplayFormat::from_utf8(run.state);
        item->setData(TaskColumn, StateRole, state);
        item->setText(StatusColumn, tr("%1 — %2 done, %3 failed, %4 skipped, %5 queued, %6 running")
                                        .arg(state, DisplayFormat::count(run.completed),
                                             DisplayFormat::count(run.failed), DisplayFormat::count(run.skipped),
                                             DisplayFormat::count(run.queued), DisplayFormat::count(run.running)));
    }
    worker_status_[library_name] = DisplayFormat::from_utf8(event.worker_status);
    QStringList lines;
    for (const auto& [library, status] : worker_status_)
        if (!status.isEmpty()) lines << tr("%1: %2").arg(library, status);
    worker_label_->setText(lines.join(QLatin1Char('\n')));
    update_buttons();
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
    found->second->setToolTip(StatusColumn, summary);
    local_cancellers_.erase(task);
    update_buttons();
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
    bool cancellable = running && kind != LocalTask;
    if (running && kind == LocalTask) {
        const auto found = local_cancellers_.find(item->data(TaskColumn, OperationRole).toULongLong());
        cancellable = found != local_cancellers_.end() && static_cast<bool>(found->second);
    }
    cancel_button_->setEnabled(cancellable);
    pause_button_->setEnabled(running && kind == AnalysisTask && state != QLatin1String("paused"));
    resume_button_->setEnabled(running && kind == AnalysisTask && state == QLatin1String("paused"));
}

}  // namespace mso::ui
