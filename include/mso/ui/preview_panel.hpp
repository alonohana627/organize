#pragma once
/// @file
/// PreviewPanel: the preview transport dock — play/pause/stop, seek bar, waveform, volume, loop
/// and speed — driving one audio::PreviewEngine, plus the lazy waveform of the selected sample.
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

#include "mso/audio/preview_engine.hpp"
#include "mso/ui/sample_key.hpp"
#include "mso/ui/waveform_view.hpp"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QToolButton;

namespace mso::ui {

class ApplicationSession;

/// Preview dock. Only the selected sample is ever decoded for a waveform (the preview policy
/// exception of docs/01_REQUIREMENTS.md §5.2), and only after the user plays it or asks for it;
/// the job is cancelled as soon as the selection moves elsewhere.
///
/// The engine is created lazily on the UI thread (its control thread) at the first preview, so
/// startup never waits for the audio device. Set MSO_AUDIO_BACKEND=null to use the silent null
/// device (headless runs and tests).
class PreviewPanel final : public QWidget {
    Q_OBJECT

public:
    explicit PreviewPanel(ApplicationSession& session, QWidget* parent = nullptr);
    ~PreviewPanel() override;

    /// The table's current sample changed. Does not play anything by itself.
    void select_sample(const std::optional<SampleKey>& key, const QString& display_name);
    /// Space: toggles the loaded sample, or opens and plays the selected one if it differs.
    void toggle_play_pause();
    /// Opens the selected sample and starts playing (audition on selection, double-click).
    void play_selected();
    /// Escape: stops and rewinds.
    void stop();
    /// Applies the persisted volume once preferences are available.
    void load_preferences();

signals:
    /// One-line status for the main window's status bar.
    void statusMessage(const QString& message);

private:
    /// Resolves the selected sample's path on a worker, then opens it in the engine.
    void open_selected(bool autoplay);
    /// Opens a resolved file in the engine (or queues it until the engine exists).
    void on_path_resolved(const SampleKey& key, const std::filesystem::path& path, bool autoplay);
    /// Starts creating the engine on the engine pool (once); the device opens off the UI thread.
    void request_engine();
    /// Sends a transport command and starts polling.
    void post(audio::TransportCommand command);
    /// Reads a snapshot and updates every transport widget (driven by poll_timer_).
    void poll_engine();
    /// Decodes the selected sample's waveform on the waveform pool (cancellable).
    void start_waveform(const SampleKey& key, const std::filesystem::path& path);
    /// Cancels the running waveform decode.
    void cancel_waveform();
    /// Shows the selected sample's cached waveform, or its state.
    void show_cached_waveform_or_placeholder();
    /// Loops the whole sample, or stops looping.
    void apply_loop();
    /// Enables the transport controls that make sense now.
    void update_controls_enabled();

    /// Resolves paths through the Application.
    ApplicationSession& session_;
    /// A preview requested while the engine was still being created.
    struct PendingOpen {
        /// Sample to open.
        SampleKey key;
        /// Its resolved file.
        std::filesystem::path path;
        /// Start playing once opened.
        bool autoplay = false;
    };

    /// Null until created on engine_pool_.
    std::unique_ptr<audio::PreviewEngine> engine_;
    bool engine_requested_ = false;            ///< creation is running on engine_pool_
    bool engine_unavailable_ = false;          ///< creation failed; previews are disabled
    std::optional<PendingOpen> pending_open_;  ///< opened when the engine arrives
    bool showing_pause_ = false;               ///< the play button currently shows "Pause"
    QThreadPool engine_pool_;                  ///< one thread: engine creation
    QTimer poll_timer_;

    /// The table's current sample.
    std::optional<SampleKey> selected_key_;
    /// Its display name.
    QString selected_name_;
    std::optional<SampleKey> loaded_key_;                 ///< sample currently opened in the engine
    std::uint64_t loaded_generation_ = 0;                 ///< engine snapshot generation of loaded_key_
    std::optional<std::filesystem::path> selected_path_;  ///< resolved path of the selected sample
    std::uint64_t open_request_ = 0;                      ///< discards stale path resolutions
    std::optional<std::int64_t> total_frames_;
    std::optional<std::uint64_t> loop_generation_;  ///< engine generation the loop region was set for

    /// One thread: waveform decodes.
    QThreadPool waveform_pool_;
    /// Cancels the running waveform decode.
    std::stop_source waveform_stop_;
    std::optional<SampleKey> waveform_key_;  ///< sample whose waveform is being generated
    std::uint64_t waveform_request_ = 0;
    /// Small most-recently-used cache so re-selecting a sample does not decode it again.
    std::list<std::pair<SampleKey, WaveformBins>> waveform_cache_;

    /// Widgets (owned through Qt's parent tree).
    QLabel* title_label_ = nullptr;
    QToolButton* play_button_ = nullptr;
    QToolButton* stop_button_ = nullptr;
    QSlider* seek_slider_ = nullptr;
    QLabel* time_label_ = nullptr;
    WaveformView* waveform_view_ = nullptr;
    QPushButton* generate_button_ = nullptr;
    QSlider* volume_slider_ = nullptr;
    QCheckBox* loop_check_ = nullptr;
    QCheckBox* seam_fades_check_ = nullptr;  ///< opt-in seam fades (they alter audition)
    QLabel* loop_label_ = nullptr;           ///< the loop region the engine reports
    QDoubleSpinBox* speed_spin_ = nullptr;
    QLabel* status_label_ = nullptr;
};

}  // namespace mso::ui
