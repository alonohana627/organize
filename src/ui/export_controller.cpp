#include "mso/ui/export_controller.hpp"

#include <QFileDialog>
#include <QInputDialog>
#include <QStringList>
#include <QWidget>

#include <stop_token>

#include "mso/ui/application_session.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/jobs_panel.hpp"

namespace mso::ui {

ExportController::ExportController(ApplicationSession& session, JobsPanel& jobs, QWidget* dialog_parent)
    : QObject(dialog_parent), session_(session), jobs_(jobs), dialog_parent_(dialog_parent) {}

void ExportController::export_metadata(const uuids::uuid& library, bool json, const QString& current_query) {
    if (!session_.is_ready()) return;
    const QString query = current_query.trimmed();
    QStringList scopes{tr("All samples")};
    if (!query.isEmpty()) scopes << tr("Only samples matching “%1”").arg(query);
    int scope_index = 0;
    if (scopes.size() > 1) {
        QInputDialog dialog(dialog_parent_);
        dialog.setWindowTitle(tr("Export"));
        dialog.setLabelText(tr("Export:"));
        dialog.setComboBoxItems(scopes);
        dialog.setComboBoxEditable(false);
        if (dialog.exec() != QDialog::Accepted) return;
        scope_index = static_cast<int>(scopes.indexOf(dialog.textValue()));
    }
    const QString library_name = session_.library_name(library);
    const QString destination = QFileDialog::getSaveFileName(
        dialog_parent_, json ? tr("Export metadata as JSON") : tr("Export metadata as CSV"),
        json ? tr("%1.json").arg(library_name) : tr("%1.csv").arg(library_name),
        json ? tr("JSON files (*.json)") : tr("CSV files (*.csv)"));
    if (destination.isEmpty()) return;

    app::ExportOptions options;
    options.format = json ? app::ExportFormat::Json : app::ExportFormat::Csv;
    if (scope_index == 1) options.query = DisplayFormat::to_utf8(query);
    start_export(library, DisplayFormat::to_path(destination), options);
}

void ExportController::start_export(const uuids::uuid& library, const std::filesystem::path& destination,
                                    const app::ExportOptions& options) {
    if (!session_.is_ready()) return;
    auto stop = std::make_shared<std::stop_source>();
    const std::uint64_t task = jobs_.start_local_task(
        tr("Export %1 to %2").arg(session_.library_name(library), DisplayFormat::from_path(destination)),
        [stop] { stop->request_stop(); });
    running_exports_[task] = stop;
    session_.run_long_task(
        this,
        [library, destination, options, stop, shutdown = session_.shutdown_token()](app::Application& application) {
            // Cancel (the task's own stop) and application shutdown both end the export.
            std::stop_source combined;
            std::stop_callback on_cancel(stop->get_token(), [&combined] { combined.request_stop(); });
            std::stop_callback on_shutdown(shutdown, [&combined] { combined.request_stop(); });
            return application.export_metadata(library, destination, options, combined.get_token());
        },
        [this, task](Result<app::ExportSummary> summary) {
            running_exports_.erase(task);
            if (!summary) {
                const bool cancelled = summary.error().code == ErrorCode::Cancelled;
                const QString reason =
                    cancelled ? tr("cancelled; no file was written") : DisplayFormat::error(summary.error());
                jobs_.finish_local_task(task, reason, false);
                if (!cancelled) emit failed(tr("The export failed"), reason);
                return;
            }
            const QString text = tr("%n sample(s) written to %1", nullptr, static_cast<int>(summary->rows))
                                     .arg(DisplayFormat::from_path(summary->file));
            jobs_.finish_local_task(task, text, true);
            emit statusMessage(tr("Export finished: %1").arg(text));
        });
}

void ExportController::backup(const uuids::uuid& library) {
    if (!session_.is_ready()) return;
    const std::uint64_t task = jobs_.start_local_task(tr("Backup of %1").arg(session_.library_name(library)), {});
    session_.run_long_task(
        this,
        [library](app::Application& application) {
            return application.with_library(library,
                                            [](catalog::Library& opened) { return opened.create_backup("manual"); });
        },
        [this, task](Result<std::filesystem::path> backup) {
            if (!backup) {
                const QString reason = DisplayFormat::error(backup.error());
                jobs_.finish_local_task(task, reason, false);
                emit failed(tr("The backup failed"), reason);
                return;
            }
            const QString text = tr("verified backup at %1").arg(DisplayFormat::from_path(*backup));
            jobs_.finish_local_task(task, text, true);
            emit statusMessage(tr("Backup finished: %1").arg(text));
        });
}

void ExportController::cancel_all() {
    for (const auto& [task, stop] : running_exports_)
        stop->request_stop();
}

}  // namespace mso::ui
