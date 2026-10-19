#pragma once
/// @file
/// ExportController: metadata export (CSV/JSON) and library backup as cancellable jobs.
#include <QObject>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <stop_token>

#include "mso/app/application.hpp"
#include "mso/domain/core.hpp"

class QWidget;

namespace mso::ui {

class ApplicationSession;
class JobsPanel;

/// Asks where to export, runs the export or backup off the UI thread as a Jobs-panel task with a
/// result summary, and cancels running exports when the window closes.
///
/// Both run on the session's long-task pool (ApplicationSession::run_long_task), so a long export
/// or backup never delays page loads and edits. Exports stop on Cancel, on cancel_all() and on
/// session shutdown. A backup cannot be stopped half-way (the catalog writes and verifies it in
/// one step), so it shows no Cancel button and the session waits for it at exit.
class ExportController final : public QObject {
    Q_OBJECT

public:
    /// Dialogs open over `dialog_parent`; `session` and `jobs` must outlive the controller.
    ExportController(ApplicationSession& session, JobsPanel& jobs, QWidget* dialog_parent);

    /// Asks what and where to export, then exports `library` as JSON or CSV (start_export());
    /// offers to restrict it to `current_query` when non-empty.
    void export_metadata(const uuids::uuid& library, bool json, const QString& current_query);
    /// Exports `library` to `destination` with `options` as a Jobs-panel task (no dialog).
    void start_export(const uuids::uuid& library, const std::filesystem::path& destination,
                      const app::ExportOptions& options);
    /// Writes a verified backup of `library` into its backups folder.
    void backup(const uuids::uuid& library);
    /// Requests cancellation of every running export (application shutdown).
    void cancel_all();
    /// Exports still running.
    std::size_t running_exports() const { return running_exports_.size(); }

signals:
    /// One-line feedback for the status bar.
    void statusMessage(const QString& message);
    /// An export or backup failed; shown prominently.
    void failed(const QString& title, const QString& details);

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
