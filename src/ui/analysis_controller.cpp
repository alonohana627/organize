#include "mso/ui/analysis_controller.hpp"

#include <QMessageBox>
#include <QPointer>
#include <QWidget>

#include <map>

#include "mso/ui/application_session.hpp"
#include "mso/ui/display_format.hpp"

namespace mso::ui {

AnalysisController::AnalysisController(ApplicationSession& session, QWidget* dialog_parent)
    : QObject(dialog_parent), session_(session) {
    const QPointer<QWidget> parent(dialog_parent);
    confirmation_ = [parent](const QString& title, const QString& question) {
        return QMessageBox::question(parent, title, question) == QMessageBox::Yes;
    };
}

void AnalysisController::set_confirmation(Confirmation confirmation) {
    confirmation_ = std::move(confirmation);
}

void AnalysisController::start(const std::vector<SampleKey>& keys, const std::string& profile,
                               const QString& description) {
    if (!session_.is_ready()) {
        emit statusMessage(tr("The workspace is still opening; try again in a moment."));
        return;
    }
    if (keys.empty()) {
        emit statusMessage(tr("Select the samples to analyze first."));
        return;
    }
    // The keys are copied values: the question's event loop may change the selection meanwhile.
    const QString question =
        tr("%1 for %n selected sample(s)?\n\nEach file is decoded once in the background. Samples that already "
           "have current results are skipped. You can pause or cancel the run in the Jobs panel.",
           nullptr, static_cast<int>(keys.size()))
            .arg(description);
    if (!confirmation_ || !confirmation_(description, question)) return;
    std::map<uuids::uuid, std::vector<std::int64_t>> groups;
    for (const SampleKey& key : keys)
        groups[key.library].push_back(key.sample_id);
    for (const auto& [library, sample_ids] : groups) {
        session_.run(
            this,
            [library, profile, sample_ids](app::Application& application) {
                return application.start_analysis(library, profile, sample_ids);
            },
            [this, library, description](Result<catalog::Library::AnalysisRunStatus> run) {
                if (!run) {
                    emit failed(tr("Could not start the analysis"),
                                tr("%1: %2").arg(session_.library_name(library), DisplayFormat::error(run.error())));
                    return;
                }
                emit statusMessage(tr("%1 queued in %2").arg(description, session_.library_name(library)));
            });
    }
}

void AnalysisController::set_paused(bool paused) {
    paused_ = paused;
    emit pausedChanged(paused);
    if (session_.is_ready())
        session_.run_detached([paused](app::Application& application) { application.set_analysis_paused(paused); });
    emit statusMessage(paused ? tr("Background analysis paused") : tr("Background analysis resumed"));
}

void AnalysisController::control_run(const uuids::uuid& library, std::int64_t run_id,
                                     catalog::Library::RunControl control) {
    if (!session_.is_ready()) return;
    session_.run(
        this,
        [library, run_id, control](app::Application& application) {
            return application.control_analysis(library, run_id, control);
        },
        [this](Result<void> result) {
            if (!result) emit failed(tr("Analysis control failed"), DisplayFormat::error(result.error()));
        });
}

void AnalysisController::load_paused_state() {
    if (!session_.is_ready()) return;
    session_.run(
        this, [](app::Application& application) { return application.analysis_paused(); },
        [this](bool paused) {
            paused_ = paused;
            emit pausedChanged(paused);
        });
}

}  // namespace mso::ui
