#pragma once
/// @file
/// AnalysisController: explicit analysis runs of the selection, per-run control and the global
/// pause of background analysis.
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "mso/catalog/library.hpp"
#include "mso/domain/core.hpp"
#include "mso/ui/sample_key.hpp"

class QWidget;

namespace mso::ui {

class ApplicationSession;

/// Runs the Analysis menu and the Jobs panel's analysis buttons. start() asks before queueing (a
/// run decodes every selected file once) and fans the selection out into one run per library;
/// each library's outcome is reported on its own. The pause state is the Application's.
class AnalysisController final : public QObject {
    Q_OBJECT

public:
    /// Asks the user a yes/no question: (title, question) → true for yes.
    using Confirmation = std::function<bool(const QString& title, const QString& question)>;

    /// Dialogs open over `dialog_parent`; `session` must outlive the controller.
    AnalysisController(ApplicationSession& session, QWidget* dialog_parent);

    /// Replaces the confirmation question (by default a message box over the dialog parent).
    void set_confirmation(Confirmation confirmation);
    /// Queues `profile` ("Waveforms", "Musical", "Duplicates", "Full") for `keys` after asking;
    /// `description` names the run in the question and the outcome, e.g. "Generate waveforms".
    void start(const std::vector<SampleKey>& keys, const std::string& profile, const QString& description);
    /// Pauses or resumes all background analysis (every library).
    void set_paused(bool paused);
    /// Pauses, resumes or cancels one run.
    void control_run(const uuids::uuid& library, std::int64_t run_id, catalog::Library::RunControl control);
    /// Reads the Application's pause state (once the session is ready); pausedChanged() follows.
    void load_paused_state();
    /// The pause state last set or read.
    bool paused() const { return paused_; }

signals:
    /// Background analysis was paused or resumed.
    void pausedChanged(bool paused);
    /// One-line feedback for the status bar.
    void statusMessage(const QString& message);
    /// A request failed; shown prominently.
    void failed(const QString& title, const QString& details);

private:
    /// Runs the analysis requests.
    ApplicationSession& session_;
    /// Asks before a run is queued.
    Confirmation confirmation_;
    /// The pause state last set or read.
    bool paused_ = false;
};

}  // namespace mso::ui
