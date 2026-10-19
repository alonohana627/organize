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
/// or backup never delays page loads and edits. Both stop on their Cancel button, on cancel_all()
/// (the window closes) and on session shutdown: a stopped export writes no file, and a stopped
/// backup removes its partial copy (Library::create_backup's stop token), so exit never waits for
/// a backup of a large library to finish.
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
    /// Writes a verified backup of `library` into its backups folder as a cancellable Jobs task.
    void backup(const uuids::uuid& library);
    /// Requests cancellation of every running export and backup (the window closes).
    void cancel_all();
    /// Exports and backups still running.
    std::size_t running_tasks() const { return running_tasks_.size(); }

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
    /// Starts a Jobs-panel task whose Cancel button (and cancel_all()) requests `stop`.
    std::uint64_t start_task(const QString& description, const std::shared_ptr<std::stop_source>& stop);

    std::map<std::uint64_t, std::shared_ptr<std::stop_source>> running_tasks_;  ///< jobs task → stop
};

}  // namespace mso::ui
