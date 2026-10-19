#include "mso/ui/preview_panel.hpp"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTime>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <string_view>

#include "mso/analysis/waveform_preview.hpp"
#include "mso/ui/application_session.hpp"
#include "mso/ui/background_tasks.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/ui_preferences.hpp"

namespace mso::ui {

namespace {

constexpr int kPollIntervalMilliseconds = 30;
constexpr int kDeviceCheckMilliseconds = 1000;  ///< device-state polling while nothing plays
constexpr int kSeekResolution = 1000;           ///< slider steps across the whole sample
constexpr int kMinimumVolumeDecibels = -60;     ///< slider bottom = silence
constexpr int kWaveformBins = 1024;
constexpr std::size_t kWaveformCacheEntries = 8;
constexpr std::int64_t kMillisecondsPerHour = 3'600'000;

/// "1:02.3" (or "1:02:03.4" past an hour) from a frame position and rate, via QTime.
QString format_clock(std::int64_t frames, int sample_rate) {
    if (sample_rate <= 0) return PreviewPanel::tr("–:––");
    // Rounded to tenths, so QTime's "z" (milliseconds without trailing zeros) shows one digit.
    const std::int64_t milliseconds = (frames * 1000 / sample_rate + 50) / 100 * 100;
    const QTime time = QTime::fromMSecsSinceStartOfDay(static_cast<int>(milliseconds % (24 * kMillisecondsPerHour)));
    return time.toString(milliseconds >= kMillisecondsPerHour ? QStringLiteral("h:mm:ss.z") : QStringLiteral("m:ss.z"));
}

/// Backend chosen by MSO_AUDIO_BACKEND ("null" or "system"); the system device by default.
audio::OutputBackend backend_from_environment() {
    return qEnvironmentVariable("MSO_AUDIO_BACKEND") == QLatin1String("null") ? audio::OutputBackend::Null
                                                                              : audio::OutputBackend::System;
}

}  // namespace

PreviewPanel::PreviewPanel(ApplicationSession& session, QWidget* parent, EngineFactory factory)
    : QWidget(parent),
      session_(session),
      engine_factory_(factory ? std::move(factory) : EngineFactory(&audio::PreviewEngine::create)) {
    waveform_pool_.setMaxThreadCount(1);
    engine_pool_.setMaxThreadCount(1);
    engine_pool_.setObjectName(QStringLiteral("PreviewEngine"));
    // Open the output device in the background right away, so the first preview starts instantly.
    QTimer::singleShot(0, this, &PreviewPanel::request_engine);
    poll_timer_.setInterval(kPollIntervalMilliseconds);
    connect(&poll_timer_, &QTimer::timeout, this, &PreviewPanel::poll_engine);
    device_timer_.setInterval(kDeviceCheckMilliseconds);
    connect(&device_timer_, &QTimer::timeout, this, [this] {
        if (engine_) update_device_notice(engine_->snapshot());
    });
    build_ui();
    update_controls_enabled();
}

void PreviewPanel::build_ui() {
    title_label_ = new QLabel(tr("No sample selected"), this);
    title_label_->setTextFormat(Qt::PlainText);  // sample names are user text
    title_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    title_label_->setAccessibleName(tr("Previewed sample"));

    play_button_ = new QToolButton(this);
    play_button_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    play_button_->setAccessibleName(tr("Play"));
    play_button_->setToolTip(tr("Play / pause (Space)"));
    connect(play_button_, &QToolButton::clicked, this, &PreviewPanel::toggle_play_pause);

    stop_button_ = new QToolButton(this);
    stop_button_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    stop_button_->setAccessibleName(tr("Stop"));
    stop_button_->setToolTip(tr("Stop (Escape)"));
    connect(stop_button_, &QToolButton::clicked, this, &PreviewPanel::stop);

    seek_slider_ = new QSlider(Qt::Horizontal, this);
    seek_slider_->setRange(0, kSeekResolution);
    seek_slider_->setAccessibleName(tr("Seek position"));
    const auto seek_to_slider = [this] {
        post(audio::transport::SeekToFraction{static_cast<double>(seek_slider_->sliderPosition()) / kSeekResolution});
    };
    connect(seek_slider_, &QSlider::sliderReleased, this, seek_to_slider);
    connect(seek_slider_, &QSlider::actionTriggered, this, [seek_to_slider, this](int action) {
        if (action != QAbstractSlider::SliderMove)
            seek_to_slider();  // keyboard and page steps
        else if (seek_slider_->isSliderDown())
            seek_to_slider();  // live scrubbing
    });

    time_label_ = new QLabel(tr("0:00.0 / –:––"), this);
    time_label_->setAccessibleName(tr("Playback time"));

    waveform_view_ = new WaveformView(this);
    connect(waveform_view_, &WaveformView::seekRequested, this,
            [this](double fraction) { post(audio::transport::SeekToFraction{fraction}); });

    generate_button_ = new QPushButton(tr("Generate waveform"), this);
    generate_button_->setToolTip(tr("Decode the selected sample once to draw its waveform"));
    connect(generate_button_, &QPushButton::clicked, this, [this] {
        if (selected_key_ && selected_path_)
            start_waveform(*selected_key_, *selected_path_);
        else
            resolve_selected(PathUse::WaveformOnly);  // never replaces what the engine plays
    });

    volume_slider_ = new QSlider(Qt::Horizontal, this);
    volume_slider_->setRange(kMinimumVolumeDecibels, 0);
    volume_slider_->setValue(-6);
    volume_slider_->setAccessibleName(tr("Preview volume"));
    volume_slider_->setToolTip(tr("Preview volume (dB)"));
    connect(volume_slider_, &QSlider::valueChanged, this, [this](int decibels) {
        if (decibels <= kMinimumVolumeDecibels)
            post(audio::transport::SetVolumeLinear{0.0});
        else
            post(audio::transport::SetVolumeDecibels{static_cast<double>(decibels)});
        volume_slider_->setToolTip(tr("Preview volume: %1 dB").arg(decibels));
        // Only the session's cache changes here; its coalesced writer persists the final value.
        session_.set_preference(UiPreferenceKeys::kPreviewVolume, decibels);
    });

    loop_check_ = new QCheckBox(tr("Loop"), this);
    loop_check_->setAccessibleName(tr("Loop preview"));
    connect(loop_check_, &QCheckBox::toggled, this, &PreviewPanel::apply_loop);

    // Seam fades are opt-in: they hide a click at a loop seam but alter what is auditioned.
    seam_fades_check_ = new QCheckBox(tr("Seam fades"), this);
    seam_fades_check_->setAccessibleName(tr("Fade at loop seams"));
    seam_fades_check_->setToolTip(
        tr("Short fades at every loop seam hide clicks in loops that are not seamless, but "
           "they also dip the level and soften the first transient: what you hear is no "
           "longer exactly the file."));
    connect(seam_fades_check_, &QCheckBox::toggled, this,
            [this](bool enabled) { post(audio::transport::SetSeamFades{enabled}); });
    seam_fades_check_->setEnabled(false);  // seams exist only while looping
    connect(loop_check_, &QCheckBox::toggled, seam_fades_check_, &QCheckBox::setEnabled);
    loop_label_ = new QLabel(this);
    loop_label_->setAccessibleName(tr("Loop region"));

    speed_spin_ = new QDoubleSpinBox(this);
    speed_spin_->setRange(audio::kMinimumSpeedRatio, audio::kMaximumSpeedRatio);
    speed_spin_->setSingleStep(0.05);
    speed_spin_->setValue(1.0);
    speed_spin_->setSuffix(tr("×"));
    speed_spin_->setAccessibleName(tr("Playback speed"));
    speed_spin_->setToolTip(tr("Speed (pitch follows speed)"));
    connect(speed_spin_, &QDoubleSpinBox::valueChanged, this,
            [this](double ratio) { post(audio::transport::SetSpeed{ratio}); });

    device_menu_ = new QMenu(this);
    connect(device_menu_, &QMenu::aboutToShow, this, &PreviewPanel::fill_device_menu);
    device_button_ = new QToolButton(this);
    device_button_->setText(tr("Output"));
    device_button_->setToolTip(tr("Audio output device for previews"));
    device_button_->setAccessibleName(tr("Output device"));
    device_button_->setMenu(device_menu_);
    device_button_->setPopupMode(QToolButton::InstantPopup);

    device_notice_ = new QWidget(this);
    device_notice_label_ = new QLabel(device_notice_);
    device_notice_label_->setWordWrap(true);
    device_notice_label_->setTextFormat(Qt::PlainText);
    device_notice_label_->setAccessibleName(tr("Audio device notice"));
    auto* retry_button = new QPushButton(tr("Retry"), device_notice_);
    retry_button->setAccessibleName(tr("Retry audio device"));
    connect(retry_button, &QPushButton::clicked, this, &PreviewPanel::retry_output_device);
    auto* notice_layout = new QHBoxLayout(device_notice_);
    notice_layout->setContentsMargins(0, 0, 0, 0);
    notice_layout->addWidget(device_notice_label_, 1);
    notice_layout->addWidget(retry_button);
    device_notice_->hide();

    status_label_ = new QLabel(this);
    status_label_->setWordWrap(true);
    status_label_->setTextFormat(Qt::PlainText);  // error texts may quote file names
    status_label_->setAccessibleName(tr("Preview status"));

    auto* transport_row = new QHBoxLayout;
    transport_row->addWidget(play_button_);
    transport_row->addWidget(stop_button_);
    transport_row->addWidget(seek_slider_, 1);
    transport_row->addWidget(time_label_);

    auto* options_row = new QHBoxLayout;
    options_row->addWidget(new QLabel(tr("Volume"), this));
    options_row->addWidget(volume_slider_, 1);
    options_row->addWidget(loop_check_);
    options_row->addWidget(seam_fades_check_);
    options_row->addWidget(loop_label_);
    options_row->addWidget(new QLabel(tr("Speed"), this));
    options_row->addWidget(speed_spin_);
    options_row->addWidget(generate_button_);
    options_row->addWidget(device_button_);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(title_label_);
    layout->addLayout(transport_row);
    layout->addWidget(waveform_view_, 1);
    layout->addLayout(options_row);
    layout->addWidget(device_notice_);
    layout->addWidget(status_label_);
}

PreviewPanel::~PreviewPanel() {
    cancel_waveform();
    waveform_pool_.waitForDone();
    engine_pool_.waitForDone();  // a creation still running finishes first
    // The engine keeps COM on its own output thread, so destroying it here is safe; it closes the
    // device and joins its threads. Device requests still waiting answer CANCELLED meanwhile, while
    // this panel is whole: their queued deliveries are dropped with it.
    engine_.reset();
}

void PreviewPanel::load_preferences() {
    const auto stored = session_.preference(UiPreferenceKeys::kPreviewVolume);
    if (stored.is_number_integer()) {
        const QSignalBlocker blocker(volume_slider_);
        volume_slider_->setValue(std::clamp(stored.get<int>(), kMinimumVolumeDecibels, 0));
    }
}

void PreviewPanel::select_sample(const std::optional<SampleKey>& key, const QString& display_name) {
    if (key == selected_key_) return;
    selected_key_ = key;
    selected_name_ = display_name;
    selected_path_.reset();
    ++open_request_;
    if (waveform_key_ && waveform_key_ != key) cancel_waveform();
    title_label_->setText(key ? display_name : tr("No sample selected"));
    status_label_->clear();
    show_cached_waveform_or_placeholder();
    update_controls_enabled();
}

void PreviewPanel::toggle_play_pause() {
    if (engine_ && loaded_key_ && loaded_key_ == selected_key_) {
        // An explicit Pause or Play, never a toggle: after a device loss the restore already plays,
        // and a queued toggle would pause it again at once.
        if (engine_->snapshot().state == audio::PreviewState::Playing)
            post(audio::transport::Pause{});
        else
            play_loaded();
        return;
    }
    resolve_selected(PathUse::Play);
}

void PreviewPanel::play_selected() {
    if (engine_ && loaded_key_ && loaded_key_ == selected_key_) {
        play_loaded();
        return;
    }
    resolve_selected(PathUse::Play);
}

void PreviewPanel::play_loaded() {
    if (engine_ && engine_->snapshot().output_device_lost) {
        restart_output_device(true, false);  // plays once the device is back
        return;
    }
    post(audio::transport::Play{});
}

void PreviewPanel::stop() {
    post(audio::transport::Stop{});
}

void PreviewPanel::resolve_selected(PathUse use) {
    if (!selected_key_ || !session_.is_ready()) return;
    const SampleKey key = *selected_key_;
    const std::uint64_t request = ++open_request_;
    if (use == PathUse::Play) status_label_->setText(tr("Opening…"));
    session_.run(
        this,
        [key](app::Application& application) { return application.resolve_playable_path(key.library, key.sample_id); },
        [this, key, request, use](Result<std::filesystem::path> path) {
            if (request != open_request_ || key != selected_key_) return;  // selection moved on
            if (!path) {
                status_label_->setText(tr("Cannot preview: %1").arg(DisplayFormat::error(path.error())));
                emit statusMessage(status_label_->text());
                return;
            }
            if (use == PathUse::WaveformOnly) {
                selected_path_ = *path;
                start_waveform(key, *path);
                return;
            }
            on_path_resolved(key, *path, true);
        });
}

void PreviewPanel::on_path_resolved(const SampleKey& key, const std::filesystem::path& path, bool autoplay) {
    selected_path_ = path;
    // Previewing authorizes bounded decoding of this one file for its waveform.
    if (waveform_view_->state() != WaveformView::State::Ready && waveform_key_ != key) start_waveform(key, path);
    if (!engine_) {
        // The device is still being opened in the background (or failed before: try again now),
        // and the file plays as soon as it is ready.
        pending_open_ = PendingOpen{key, path, autoplay};
        request_engine();
        return;
    }
    if (auto opened = engine_->open(path); !opened) {
        status_label_->setText(tr("Cannot preview: %1").arg(DisplayFormat::error(opened.error())));
        return;
    }
    loaded_key_ = key;
    loaded_generation_ = engine_->snapshot().generation;
    status_label_->clear();
    post(audio::transport::SetSpeed{speed_spin_->value()});
    if (autoplay) play_loaded();
    update_controls_enabled();
}

void PreviewPanel::request_engine() {
    if (engine_ || engine_requested_) return;
    engine_requested_ = true;
    using Creation = Result<std::unique_ptr<audio::PreviewEngine>>;
    audio::PreviewOptions options;
    options.backend = backend_from_environment();
    if (const auto device = session_.preference(UiPreferenceKeys::kPreviewOutputDevice); device.is_string())
        options.device = audio::OutputDeviceId{device.get<std::string>()};
    if (!engine_failure_.isEmpty()) status_label_->setText(tr("Opening the audio output again…"));
    // Opening the output device can take hundreds of milliseconds: never on the UI thread.
    run_in_background(
        engine_pool_, this, [options, factory = engine_factory_]() -> Creation { return factory(options); },
        [this](Creation created) {
            engine_requested_ = false;
            if (!created) {
                // The next Play tries again.
                engine_failure_ = DisplayFormat::error(created.error());
                pending_open_.reset();
                status_label_->setText(tr("Audio output unavailable: %1").arg(engine_failure_));
                update_controls_enabled();
                return;
            }
            engine_failure_.clear();
            status_label_->clear();
            engine_ = std::move(*created);
            device_timer_.start();
            update_device_notice(engine_->snapshot());
            const int decibels = volume_slider_->value();
            if (decibels <= kMinimumVolumeDecibels)
                engine_->post(audio::transport::SetVolumeLinear{0.0});
            else
                engine_->post(audio::transport::SetVolumeDecibels{static_cast<double>(decibels)});
            engine_->post(audio::transport::SetSeamFades{seam_fades_check_->isChecked()});
            if (auto pending = std::exchange(pending_open_, std::nullopt); pending && pending->key == selected_key_)
                on_path_resolved(pending->key, pending->path, pending->autoplay);
            update_controls_enabled();
        });
}

void PreviewPanel::post(audio::TransportCommand command) {
    if (!engine_) return;
    engine_->post(std::move(command));
    if (!poll_timer_.isActive()) poll_timer_.start();
}

void PreviewPanel::poll_engine() {
    if (!engine_) {
        poll_timer_.stop();
        return;
    }
    const audio::PreviewSnapshot snapshot = engine_->snapshot();
    const bool playing = snapshot.state == audio::PreviewState::Playing;
    if (playing != showing_pause_) {
        // Only on a change: screen readers announce every accessible-name update.
        showing_pause_ = playing;
        play_button_->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
        play_button_->setAccessibleName(playing ? tr("Pause") : tr("Play"));
    }

    total_frames_ = snapshot.total_frames;
    const bool known_length = snapshot.total_frames && *snapshot.total_frames > 0;
    std::optional<double> fraction;
    if (known_length)
        fraction = std::clamp(
            static_cast<double>(snapshot.position_frames) / static_cast<double>(*snapshot.total_frames), 0.0, 1.0);
    if (!seek_slider_->isSliderDown() && fraction) {
        const QSignalBlocker blocker(seek_slider_);
        seek_slider_->setValue(static_cast<int>(std::lround(*fraction * kSeekResolution)));
    }
    const QString position = format_clock(snapshot.position_frames, snapshot.source_sample_rate);
    const QString total = known_length ? format_clock(*snapshot.total_frames, snapshot.source_sample_rate) : tr("–:––");
    time_label_->setText(snapshot.total_frames_exact || !known_length ? tr("%1 / %2").arg(position, total)
                                                                      : tr("%1 / %2 (estimated)").arg(position, total));
    const bool showing_loaded =
        loaded_key_ && loaded_key_ == selected_key_ && snapshot.generation == loaded_generation_;
    waveform_view_->set_playhead(showing_loaded ? fraction : std::nullopt);
    seek_slider_->setEnabled(showing_loaded && known_length);
    waveform_view_->set_seekable(showing_loaded && known_length);

    update_device_notice(snapshot);
    if (snapshot.last_error && *snapshot.last_error != ErrorCode::DeviceLost)
        status_label_->setText(
            tr("Playback error: %1").arg(DisplayFormat::from_utf8(mso::to_string(*snapshot.last_error))));
    // Show what the engine actually loops (clamped to the source, at least kMinimumLoopMilliseconds)
    // and whether seam fades are on, as reported rather than as requested.
    if (snapshot.loop && snapshot.source_sample_rate > 0) {
        loop_label_->setText(tr("Looping %1–%2")
                                 .arg(format_clock(snapshot.loop->start_frame, snapshot.source_sample_rate),
                                      format_clock(snapshot.loop->end_frame, snapshot.source_sample_rate)));
    } else {
        loop_label_->clear();
    }
    if (seam_fades_check_->isChecked() != snapshot.seam_fades) {
        const QSignalBlocker blocker(seam_fades_check_);
        seam_fades_check_->setChecked(snapshot.seam_fades);
    }
    if (loop_check_->isChecked() && known_length && showing_loaded && loop_generation_ != snapshot.generation)
        apply_loop();

    const bool active = playing || snapshot.state == audio::PreviewState::Loading;
    if (!active) poll_timer_.stop();  // device_timer_ keeps watching the device meanwhile
}

void PreviewPanel::update_device_notice(const audio::PreviewSnapshot& snapshot) {
    const bool lost = snapshot.output_device_lost;
    if (lost)
        device_notice_label_->setText(
            tr("The audio device is unavailable (unplugged or in use); previews are "
               "silent. It is retried automatically."));
    if (device_notice_->isHidden() == lost) device_notice_->setVisible(lost);
    const QString name = DisplayFormat::from_utf8(snapshot.output_device_name.view());
    device_button_->setToolTip(lost || name.isEmpty() ? tr("Audio output device for previews")
                                                      : tr("Previews play on %1").arg(name));
}

void PreviewPanel::retry_output_device() {
    if (!engine_) {
        request_engine();  // no engine yet (creation failed): create it again
        return;
    }
    restart_output_device(false, true);
}

template <class Value, class Deliver>
audio::DeviceRequestCompletion<Value> PreviewPanel::on_ui_thread(Deliver deliver) {
    ++device_requests_running_;
    // Called once, on the engine's output thread (or at once on this thread for a refused request):
    // only a queued call to the UI thread happens there. The guard is never null while it runs (the
    // destructor destroys the engine, which answers every waiting request, before anything else);
    // it keeps a late answer from reaching a destroyed panel all the same.
    const QPointer<PreviewPanel> guard(this);
    return [guard, deliver = std::move(deliver)](Result<Value> result) {
        PreviewPanel* panel = guard.data();
        if (!panel) return;
        QMetaObject::invokeMethod(
            panel,
            [panel, deliver, result = std::move(result)] {
                --panel->device_requests_running_;
                deliver(result);
            },
            Qt::QueuedConnection);
    };
}

void PreviewPanel::restart_output_device(bool then_play, bool announce_failure) {
    if (!engine_) return;
    play_after_restart_ = play_after_restart_ || then_play;
    announce_restart_failure_ = announce_restart_failure_ || announce_failure;
    if (restart_in_flight_) return;  // the restart already on its way answers this request too
    restart_in_flight_ = true;
    status_label_->setText(tr("Reconnecting the audio device…"));
    engine_->restart_output_device_async(on_ui_thread<void>([this](const Result<void>& restarted) {
        restart_in_flight_ = false;
        const bool play = std::exchange(play_after_restart_, false);
        const bool announce = std::exchange(announce_restart_failure_, false);
        if (!restarted) {
            // The engine keeps retrying by itself; the notice stays up.
            status_label_->setText(
                tr("The audio device is still unavailable: %1").arg(DisplayFormat::error(restarted.error())));
            if (announce) emit statusMessage(status_label_->text());
        } else {
            status_label_->clear();
            // The restart resumes what was playing when the device went; Play asked
            // for playback in any case (Play is idempotent while playing).
            if (play && loaded_key_ && loaded_key_ == selected_key_) post(audio::transport::Play{});
        }
        update_device_notice(engine_->snapshot());
        if (!poll_timer_.isActive()) poll_timer_.start();  // a resumed preview shows its progress
    }));
}

bool PreviewPanel::device_notice_visible() const {
    return !device_notice_->isHidden();
}

QString PreviewPanel::status_text() const {
    return status_label_->text();
}

void PreviewPanel::fill_device_menu() {
    populate_device_menu();  // the last list at once: opening the menu never waits for the device
    if (!engine_ || listing_devices_) return;
    listing_devices_ = true;
    engine_->list_output_devices_async(on_ui_thread<std::vector<audio::OutputDeviceInfo>>(
        [this](const Result<std::vector<audio::OutputDeviceInfo>>& devices) {
            listing_devices_ = false;
            device_list_ = devices;
            // Only an open menu is rebuilt (a closed one is filled when it opens).
            if (device_menu_->isVisible()) populate_device_menu();
        }));
}

void PreviewPanel::populate_device_menu() {
    device_menu_->clear();
    if (!engine_) {
        device_menu_->addAction(tr("(audio output not open yet)"))->setEnabled(false);
        return;
    }
    const auto stored = session_.preference(UiPreferenceKeys::kPreviewOutputDevice);
    const audio::OutputDeviceId chosen{stored.is_string() ? stored.get<std::string>() : std::string()};
    QAction* follow_default =
        device_menu_->addAction(tr("System default"), this, [this] { choose_output_device(audio::OutputDeviceId{}); });
    follow_default->setCheckable(true);
    follow_default->setChecked(chosen.is_system_default());
    device_menu_->addSeparator();
    if (!device_list_) {
        device_menu_->addAction(tr("Looking for devices…"))->setEnabled(false);
        return;
    }
    if (!*device_list_) {
        device_menu_->addAction(tr("Devices unavailable: %1").arg(DisplayFormat::error(device_list_->error())))
            ->setEnabled(false);
    } else {
        for (const auto& device : **device_list_) {
            QAction* action = device_menu_->addAction(DisplayFormat::from_utf8(device.name), this,
                                                      [this, id = device.id] { choose_output_device(id); });
            action->setCheckable(true);
            action->setChecked(!chosen.is_system_default() && device.id == chosen);
        }
    }
    if (listing_devices_) device_menu_->addAction(tr("Updating the list…"))->setEnabled(false);
}

void PreviewPanel::choose_output_device(const audio::OutputDeviceId& device) {
    if (!engine_) return;
    status_label_->setText(tr("Switching the audio device…"));
    engine_->select_output_device_async(
        device, on_ui_thread<void>([this, device](const Result<void>& selected) {
            if (!selected) {
                status_label_->setText(
                    tr("Could not switch the audio device: %1").arg(DisplayFormat::error(selected.error())));
                emit statusMessage(status_label_->text());
                return;
            }
            session_.set_preference(UiPreferenceKeys::kPreviewOutputDevice, device.value);
            status_label_->clear();
            update_device_notice(engine_->snapshot());
        }));
}

void PreviewPanel::apply_loop() {
    if (!engine_) return;
    if (loop_check_->isChecked() && total_frames_ && *total_frames_ > 0) {
        post(audio::transport::SetLoop{audio::LoopRegion{0, *total_frames_}});
        loop_generation_ = engine_->snapshot().generation;
    } else if (!loop_check_->isChecked()) {
        post(audio::transport::SetLoop{std::nullopt});
        loop_generation_.reset();
    }
}

void PreviewPanel::start_waveform(const SampleKey& key, const std::filesystem::path& path) {
    cancel_waveform();
    waveform_stop_ = std::stop_source();
    const std::stop_token stop = waveform_stop_.get_token();
    const std::uint64_t request = ++waveform_request_;
    waveform_key_ = key;
    waveform_view_->show_generating(0.0);
    run_in_background(
        waveform_pool_, this,
        [this, path, stop, request]() {
            // Only the decoder and the waveform builder: preview decoding is not an analysis run.
            return analysis::compute_waveform_overview(path, kWaveformBins, stop, [this, request](double fraction) {
                // Worker thread: hop to the UI thread; the destructor waits for this pool.
                QMetaObject::invokeMethod(
                    this,
                    [this, request, fraction] {
                        if (request == waveform_request_ && waveform_key_ == selected_key_)
                            waveform_view_->show_generating(fraction);
                    },
                    Qt::QueuedConnection);
            });
        },
        [this, key, request](Result<analysis::WaveformOverview> overview) {
            if (request != waveform_request_) return;
            waveform_key_.reset();
            if (!overview) {
                if (overview.error().code != ErrorCode::Cancelled && key == selected_key_)
                    waveform_view_->show_failed(DisplayFormat::error(overview.error()));
                return;
            }
            if (overview->maximum.empty()) {
                if (key == selected_key_) waveform_view_->show_failed(tr("no audio was decoded"));
                return;
            }
            WaveformBins bins{std::move(overview->minimum), std::move(overview->maximum), std::move(overview->rms)};
            waveform_cache_.emplace_front(key, bins);
            if (waveform_cache_.size() > kWaveformCacheEntries) waveform_cache_.pop_back();
            if (key == selected_key_) waveform_view_->show_waveform(std::move(bins));
            update_controls_enabled();
        });
}

void PreviewPanel::cancel_waveform() {
    waveform_stop_.request_stop();
    ++waveform_request_;
    waveform_key_.reset();
}

void PreviewPanel::show_cached_waveform_or_placeholder() {
    if (!selected_key_) {
        waveform_view_->show_no_sample();
        return;
    }
    for (auto entry = waveform_cache_.begin(); entry != waveform_cache_.end(); ++entry) {
        if (entry->first == *selected_key_) {
            waveform_cache_.splice(waveform_cache_.begin(), waveform_cache_, entry);  // mark most recent
            waveform_view_->show_waveform(waveform_cache_.front().second);
            return;
        }
    }
    if (waveform_key_ == selected_key_) return;  // still generating for this sample
    waveform_view_->show_not_generated();
}

void PreviewPanel::update_controls_enabled() {
    // Play stays enabled after a failed engine creation: pressing it tries again.
    const bool has_selection = selected_key_.has_value();
    play_button_->setEnabled(has_selection);
    stop_button_->setEnabled(engine_ != nullptr);
    generate_button_->setEnabled(selected_key_.has_value() && waveform_view_->state() != WaveformView::State::Ready);
    const bool showing_loaded = loaded_key_ && loaded_key_ == selected_key_;
    seek_slider_->setEnabled(showing_loaded && total_frames_.has_value());
}

}  // namespace mso::ui
