#pragma once
/// @file
/// PreviewPanel: the preview transport dock — play/pause/stop, seek bar, waveform, volume, loop,
/// speed and output device — driving one audio::PreviewEngine, plus the lazy waveform of the
/// selected sample.
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
class QMenu;
class QPushButton;
class QSlider;
class QToolButton;

namespace mso::ui {

class ApplicationSession;

/// Preview dock. Only the selected sample is ever decoded for a waveform (the preview policy
/// exception of docs/01_REQUIREMENTS.md §5.2), and only after the user plays it or asks for it;
/// the job is cancelled as soon as the selection moves elsewhere. "Generate waveform" decodes the
/// file for the waveform only: it never replaces what is playing.
///
/// Engine thread: opening the output device can take hundreds of milliseconds, so the engine is
/// created on engine_pool_ (one thread); every other call, destruction included, is made from the
/// UI thread (the engine keeps COM on its own output thread, so no apartment is touched here).
/// When creating the engine fails, the next Play tries again. Set MSO_AUDIO_BACKEND=null to use
/// the silent null device (headless runs and tests).
///
/// Device loss: while the engine reports output_device_lost (headphones unplugged, no device at
/// start), the panel shows a notice with "Retry" (PreviewEngine::restart_output_device()); the
/// engine also retries by itself, and Play retries first. The output device can be chosen from the Output menu and is
/// remembered (UiPreferenceKeys::kPreviewOutputDevice).
class PreviewPanel final : public QWidget {
    Q_OBJECT

public:
    /// A panel resolving and previewing samples through `session`, which must outlive it.
    explicit PreviewPanel(ApplicationSession& session, QWidget* parent = nullptr);
    /// Stops the waveform job and destroys the engine.
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
    /// Retries the output device now (the notice's Retry button).
    void retry_output_device();
    /// True while the device-loss notice is shown.
    bool device_notice_visible() const;
    /// The engine (null until created; tests drive it).
    audio::PreviewEngine* engine() const { return engine_.get(); }

signals:
    /// One-line status for the main window's status bar.
    void statusMessage(const QString& message);

private:
    /// What a resolved path of the selected sample is for.
    enum class PathUse {
        Play,          ///< open in the engine and play
        WaveformOnly,  ///< decode for the waveform; the engine keeps what it is playing
    };

    /// Builds the widgets.
    void build_ui();
    /// Resolves the selected sample's path on a worker, then plays it or draws its waveform.
    void resolve_selected(PathUse use);
    /// Opens a resolved file in the engine (or queues it until the engine exists).
    void on_path_resolved(const SampleKey& key, const std::filesystem::path& path, bool autoplay);
    /// Starts creating the engine on the engine pool (once at a time; again after a failure).
    void request_engine();
    /// Sends a transport command and starts polling.
    void post(audio::TransportCommand command);
    /// Reads a snapshot and updates every transport widget (driven by poll_timer_).
    void poll_engine();
    /// Before playing: re-opens a lost output device (Play doubles as Retry).
    void recover_device_before_play();
    /// Shows or hides the device-loss notice from a snapshot.
    void update_device_notice(const audio::PreviewSnapshot& snapshot);
    /// Lists the output devices in the Output menu (on opening it).
    void fill_device_menu();
    /// Switches to `device` and remembers it.
    void choose_output_device(const audio::OutputDeviceId& device);
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
    QString engine_failure_;                   ///< why the last creation failed (empty: none failed)
    std::optional<PendingOpen> pending_open_;  ///< opened when the engine arrives
    bool showing_pause_ = false;               ///< the play button currently shows "Pause"
    QThreadPool engine_pool_;                  ///< one thread: engine creation
    QTimer poll_timer_;                        ///< fast transport polling while playing or loading
    QTimer device_timer_;                      ///< slow device-state polling while idle

    /// The table's current sample.
    std::optional<SampleKey> selected_key_;
    /// Its display name.
    QString selected_name_;
    std::optional<SampleKey> loaded_key_;                 ///< sample currently opened in the engine
    std::uint64_t loaded_generation_ = 0;                 ///< engine snapshot generation of loaded_key_
    std::optional<std::filesystem::path> selected_path_;  ///< resolved path of the selected sample
    std::uint64_t open_request_ = 0;                      ///< discards stale path resolutions
    std::optional<std::int64_t> total_frames_;            ///< length of the loaded source
    std::optional<std::uint64_t> loop_generation_;        ///< engine generation the loop region was set for

    /// One thread: waveform decodes.
    QThreadPool waveform_pool_;
    /// Cancels the running waveform decode.
    std::stop_source waveform_stop_;
    std::optional<SampleKey> waveform_key_;  ///< sample whose waveform is being generated
    std::uint64_t waveform_request_ = 0;     ///< discards stale waveform deliveries
    /// Small most-recently-used cache so re-selecting a sample does not decode it again.
    std::list<std::pair<SampleKey, WaveformBins>> waveform_cache_;

    // Widgets (owned through Qt's parent tree).
    QLabel* title_label_ = nullptr;           ///< previewed sample's name
    QToolButton* play_button_ = nullptr;      ///< play / pause
    QToolButton* stop_button_ = nullptr;      ///< stop and rewind
    QSlider* seek_slider_ = nullptr;          ///< position
    QLabel* time_label_ = nullptr;            ///< position and length
    WaveformView* waveform_view_ = nullptr;   ///< waveform with playhead
    QPushButton* generate_button_ = nullptr;  ///< decodes the waveform only
    QSlider* volume_slider_ = nullptr;        ///< volume in dB
    QCheckBox* loop_check_ = nullptr;         ///< loops the whole sample
    QCheckBox* seam_fades_check_ = nullptr;   ///< opt-in seam fades (they alter audition)
    QLabel* loop_label_ = nullptr;            ///< the loop region the engine reports
    QDoubleSpinBox* speed_spin_ = nullptr;    ///< varispeed ratio
    QToolButton* device_button_ = nullptr;    ///< Output device menu
    QMenu* device_menu_ = nullptr;            ///< endpoints, filled when opened
    QWidget* device_notice_ = nullptr;        ///< "audio device lost" with Retry
    QLabel* device_notice_label_ = nullptr;   ///< the notice's text
    QLabel* status_label_ = nullptr;          ///< errors and progress
};

}  // namespace mso::ui
