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
    explicit JobsPanel(QWidget* parent = nullptr);

    /// Adds an import operation that the Application started.
    void track_import(app::OperationId operation, const QString& description);
    void update_import(const app::ImportProgressEvent& event);
    /// Marks an Application operation (import) finished with its outcome.
    void finish_operation(const app::OperationFinishedEvent& event);
    /// Replaces the analysis runs shown for one library.
    void update_analysis(const app::AnalysisProgressEvent& event, const QString& library_name);
    /// Adds a task run by the UI itself (export, backup). `cancel` may be empty.
    std::uint64_t start_local_task(const QString& description, std::function<void()> cancel);
    void finish_local_task(std::uint64_t task, const QString& summary, bool succeeded);
    /// Reflects the global analysis pause state on the toggle button.
    void set_analysis_paused(bool paused);

signals:
    void cancelOperationRequested(mso::app::OperationId operation);
    void analysisControlRequested(const uuids::uuid& library, std::int64_t run_id,
                                  mso::catalog::Library::RunControl control);
    void analysisPauseRequested(bool paused);

private:
    void update_buttons();
    QTreeWidgetItem* make_task_item(const QString& description);
    static void set_progress(QTreeWidgetItem* item, std::int64_t done, std::int64_t total);

    QTreeWidget* tree_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QPushButton* pause_button_ = nullptr;
    QPushButton* resume_button_ = nullptr;
    QPushButton* pause_all_button_ = nullptr;
    QPushButton* clear_button_ = nullptr;
    QLabel* worker_label_ = nullptr;
    bool analysis_paused_ = false;

    std::map<app::OperationId, QTreeWidgetItem*> imports_;
    std::map<std::uint64_t, QTreeWidgetItem*> local_tasks_;
    std::map<std::uint64_t, std::function<void()>> local_cancellers_;
    std::uint64_t next_local_task_ = 1;
    std::map<QString, QString> worker_status_;  ///< library name → worker status line
};

}  // namespace mso::ui
