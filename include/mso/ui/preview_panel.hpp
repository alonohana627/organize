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
    void on_path_resolved(const SampleKey& key, const std::filesystem::path& path, bool autoplay);
    /// Creates the engine on first use; false (with a visible message) when audio is unavailable.
    bool ensure_engine();
    void post(audio::TransportCommand command);
    /// Reads a snapshot and updates every transport widget (driven by poll_timer_).
    void poll_engine();
    void start_waveform(const SampleKey& key, const std::filesystem::path& path);
    void cancel_waveform();
    void show_cached_waveform_or_placeholder();
    void apply_loop();
    void update_controls_enabled();

    ApplicationSession& session_;
    std::unique_ptr<audio::PreviewEngine> engine_;
    bool engine_unavailable_ = false;
    QTimer poll_timer_;

    std::optional<SampleKey> selected_key_;
    QString selected_name_;
    std::optional<SampleKey> loaded_key_;                 ///< sample currently opened in the engine
    std::uint64_t loaded_generation_ = 0;                 ///< engine snapshot generation of loaded_key_
    std::optional<std::filesystem::path> selected_path_;  ///< resolved path of the selected sample
    std::uint64_t open_request_ = 0;                      ///< discards stale path resolutions
    std::optional<std::int64_t> total_frames_;
    std::optional<std::uint64_t> loop_generation_;  ///< engine generation the loop region was set for

    QThreadPool waveform_pool_;
    std::stop_source waveform_stop_;
    std::optional<SampleKey> waveform_key_;  ///< sample whose waveform is being generated
    std::uint64_t waveform_request_ = 0;
    /// Small most-recently-used cache so re-selecting a sample does not decode it again.
    std::list<std::pair<SampleKey, WaveformBins>> waveform_cache_;

    QLabel* title_label_ = nullptr;
    QToolButton* play_button_ = nullptr;
    QToolButton* stop_button_ = nullptr;
    QSlider* seek_slider_ = nullptr;
    QLabel* time_label_ = nullptr;
    WaveformView* waveform_view_ = nullptr;
    QPushButton* generate_button_ = nullptr;
    QSlider* volume_slider_ = nullptr;
    QCheckBox* loop_check_ = nullptr;
    QDoubleSpinBox* speed_spin_ = nullptr;
    QLabel* status_label_ = nullptr;
};

}  // namespace mso::ui
