#pragma once
/// @file
/// JobsPanel: imports, analysis runs and local long tasks (export, backup) with progress,
/// cancellation, pause/resume and a result summary that stays visible after completion (R18, R09).
#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "mso/app/application.hpp"

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace mso::ui {

/// Jobs dock. It only displays and forwards requests; the main window runs the actions.
class JobsPanel final : public QWidget {
    Q_OBJECT

public:
    /// Finished rows kept for their result summary; older ones are removed.
    static constexpr int kMaxFinishedRows = 50;

    explicit JobsPanel(QWidget* parent = nullptr);

    /// Adds an import operation that the Application started.
    void track_import(app::OperationId operation, const QString& description);
    /// Shows the latest progress of an import (adding its row if needed).
    void update_import(const app::ImportProgressEvent& event);
    /// Marks an Application operation (import) finished with its outcome.
    void finish_operation(const app::OperationFinishedEvent& event);
    /// Replaces the analysis runs shown for one library.
    void update_analysis(const app::AnalysisProgressEvent& event, const QString& library_name);
    /// Adds a task run by the UI itself (export, backup). `cancel` may be empty.
    std::uint64_t start_local_task(const QString& description, std::function<void()> cancel);
    /// Marks a local task finished with its outcome.
    void finish_local_task(std::uint64_t task, const QString& summary, bool succeeded);
    /// Number of rows shown (tests and accessibility summaries).
    int row_count() const;
    /// Reflects the global analysis pause state on the toggle button.
    void set_analysis_paused(bool paused);

signals:
    /// Cancel an import operation.
    void cancelOperationRequested(mso::app::OperationId operation);
    /// Pause, resume or cancel one analysis run.
    void analysisControlRequested(const uuids::uuid& library, std::int64_t run_id,
                                  mso::catalog::Library::RunControl control);
    /// Pause or resume all background analysis.
    void analysisPauseRequested(bool paused);

private:
    /// Identifies an analysis run: library and run id.
    using RunKey = std::pair<uuids::uuid, std::int64_t>;

    /// Enables Cancel/Pause/Resume for the selected row.
    void update_buttons();
    /// Appends a row for a new task.
    QTreeWidgetItem* make_task_item(const QString& description);
    /// Shows "done of total" progress in a row.
    static void set_progress(QTreeWidgetItem* item, std::int64_t done, std::int64_t total);
    /// Deletes the oldest finished rows beyond kMaxFinishedRows (invariant 13: bounded).
    void prune_finished_rows();
    /// Drops a row from the lookup maps before it is deleted (a cleared run is not re-added).
    void forget(QTreeWidgetItem* item);

    /// Widgets (owned through Qt's parent tree).
    QTreeWidget* tree_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QPushButton* pause_button_ = nullptr;
    QPushButton* resume_button_ = nullptr;
    QPushButton* pause_all_button_ = nullptr;
    QPushButton* clear_button_ = nullptr;
    QLabel* worker_label_ = nullptr;
    /// Whether all analysis is paused (toggle button state).
    bool analysis_paused_ = false;

    /// Rows of import operations.
    std::map<app::OperationId, QTreeWidgetItem*> imports_;
    /// Rows of UI tasks (export, backup).
    std::map<std::uint64_t, QTreeWidgetItem*> local_tasks_;
    /// Cancel callbacks of running UI tasks.
    std::map<std::uint64_t, std::function<void()>> local_cancellers_;
    /// Number of the next UI task.
    std::uint64_t next_local_task_ = 1;
    std::map<RunKey, QTreeWidgetItem*> analysis_runs_;  ///< rows of reported analysis runs
    std::set<RunKey> dismissed_runs_;                   ///< runs the user cleared (still reported)
    /// Library → (display name, worker status line).
    std::map<uuids::uuid, std::pair<QString, QString>> worker_status_;
};

}  // namespace mso::ui
