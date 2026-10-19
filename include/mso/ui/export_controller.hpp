#pragma once
/// @file
/// ExportController: metadata export (CSV/JSON) and library backup as cancellable jobs.
#include <QObject>
#include <QString>

#include <cstdint>
#include <map>
#include <memory>
#include <stop_token>

#include "mso/domain/core.hpp"

class QWidget;

namespace mso::ui {

class ApplicationSession;
class JobsPanel;

/// Asks where to export, runs the export or backup off the UI thread as a Jobs-panel task with a
/// result summary, and cancels running exports when the window closes.
class ExportController final : public QObject {
    Q_OBJECT

public:
    /// Dialogs open over `dialog_parent`; `session` and `jobs` must outlive the controller.
    ExportController(ApplicationSession& session, JobsPanel& jobs, QWidget* dialog_parent);

    /// Exports `library` as JSON or CSV; offers to restrict it to `current_query` when non-empty.
    void export_metadata(const uuids::uuid& library, bool json, const QString& current_query);
    /// Writes a verified backup of `library` into its backups folder.
    void backup(const uuids::uuid& library);
    /// Requests cancellation of every running export (application shutdown).
    void cancel_all();

signals:
    /// One-line feedback for the status bar.
    void statusMessage(const QString& message);

private:
    /// Runs the exports and backups.
    ApplicationSession& session_;
    /// Shows them as tasks.
    JobsPanel& jobs_;
    /// Parent of the file and choice dialogs.
    QWidget* dialog_parent_;
    std::map<std::uint64_t, std::shared_ptr<std::stop_source>> running_exports_;  ///< jobs task → stop
};

}  // namespace mso::ui
