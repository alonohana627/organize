#include "mso/ui/export_controller.hpp"

#include <QFileDialog>
#include <QInputDialog>
#include <QStringList>
#include <QWidget>

#include "mso/app/application.hpp"
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
    const QString destination = QFileDialog::getSaveFileName(
        dialog_parent_, json ? tr("Export metadata as JSON") : tr("Export metadata as CSV"),
        json ? tr("%1.json").arg(session_.library_name(library)) : tr("%1.csv").arg(session_.library_name(library)),
        json ? tr("JSON files (*.json)") : tr("CSV files (*.csv)"));
    if (destination.isEmpty()) return;

    app::ExportOptions options;
    options.format = json ? app::ExportFormat::Json : app::ExportFormat::Csv;
    if (scope_index == 1) options.query = DisplayFormat::to_utf8(query);
    auto stop = std::make_shared<std::stop_source>();
    const std::uint64_t task = jobs_.start_local_task(
        tr("Export %1 to %2").arg(session_.library_name(library), destination), [stop] { stop->request_stop(); });
    running_exports_[task] = stop;
    session_.run(
        this,
        [library, path = DisplayFormat::to_path(destination), options, stop](app::Application& application) {
            return application.export_metadata(library, path, options, stop->get_token());
        },
        [this, task](Result<app::ExportSummary> summary) {
            running_exports_.erase(task);
            if (!summary) {
                const bool cancelled = summary.error().code == ErrorCode::Cancelled;
                jobs_.finish_local_task(
                    task, cancelled ? tr("cancelled; no file was written") : DisplayFormat::error(summary.error()),
                    false);
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
    session_.run_with_library(
        this, library, [](catalog::Library& opened) { return opened.create_backup("manual"); },
        [this, task](Result<std::filesystem::path> backup) {
            if (!backup) {
                jobs_.finish_local_task(task, DisplayFormat::error(backup.error()), false);
                return;
            }
            jobs_.finish_local_task(task, tr("verified backup at %1").arg(DisplayFormat::from_path(*backup)), true);
        });
}

void ExportController::cancel_all() {
    for (const auto& [task, stop] : running_exports_)
        stop->request_stop();
}

}  // namespace mso::ui
