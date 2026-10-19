#include "mso/ui/preview_panel.hpp"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

#include "mso/analysis/analysis.hpp"
#include "mso/ui/application_session.hpp"
#include "mso/ui/background_tasks.hpp"
#include "mso/ui/display_format.hpp"

namespace mso::ui {

namespace {

constexpr int kPollIntervalMilliseconds = 30;
constexpr int kSeekResolution = 1000;        ///< slider steps across the whole sample
constexpr int kMinimumVolumeDecibels = -60;  ///< slider bottom = silence
constexpr int kWaveformBins = 1024;
constexpr std::size_t kWaveformCacheEntries = 8;
constexpr const char* kVolumePreference = "preview.volume_db";

/// "1:02.3" from a frame position and rate.
QString format_clock(std::int64_t frames, int sample_rate) {
    if (sample_rate <= 0) return QStringLiteral("–:––");
    const double seconds = static_cast<double>(frames) / sample_rate;
    const int minutes = static_cast<int>(seconds / 60.0);
    const double remainder = seconds - minutes * 60.0;
    return QStringLiteral("%1:%2").arg(minutes).arg(remainder, 4, 'f', 1, QLatin1Char('0'));
}

/// Backend chosen by MSO_AUDIO_BACKEND ("null" or "system"); the system device by default.
audio::OutputBackend backend_from_environment() {
    const char* value = std::getenv("MSO_AUDIO_BACKEND");
    if (value && std::string_view(value) == "null") return audio::OutputBackend::Null;
    return audio::OutputBackend::System;
}

}  // namespace

PreviewPanel::PreviewPanel(ApplicationSession& session, QWidget* parent) : QWidget(parent), session_(session) {
    waveform_pool_.setMaxThreadCount(1);
    poll_timer_.setInterval(kPollIntervalMilliseconds);
    connect(&poll_timer_, &QTimer::timeout, this, &PreviewPanel::poll_engine);

    title_label_ = new QLabel(tr("No sample selected"), this);
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

    time_label_ = new QLabel(QStringLiteral("0:00.0 / –:––"), this);
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
            open_selected(false);  // resolves the path; the waveform starts with the preview
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
        session_.set_preference(kVolumePreference, decibels);
    });

    loop_check_ = new QCheckBox(tr("Loop"), this);
    loop_check_->setAccessibleName(tr("Loop preview"));
    connect(loop_check_, &QCheckBox::toggled, this, &PreviewPanel::apply_loop);

    speed_spin_ = new QDoubleSpinBox(this);
    speed_spin_->setRange(audio::kMinimumSpeedRatio, audio::kMaximumSpeedRatio);
    speed_spin_->setSingleStep(0.05);
    speed_spin_->setValue(1.0);
    speed_spin_->setSuffix(tr("×"));
    speed_spin_->setAccessibleName(tr("Playback speed"));
    speed_spin_->setToolTip(tr("Speed (pitch follows speed)"));
    connect(speed_spin_, &QDoubleSpinBox::valueChanged, this,
            [this](double ratio) { post(audio::transport::SetSpeed{ratio}); });

    status_label_ = new QLabel(this);
    status_label_->setWordWrap(true);
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
    options_row->addWidget(new QLabel(tr("Speed"), this));
    options_row->addWidget(speed_spin_);
    options_row->addWidget(generate_button_);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(title_label_);
    layout->addLayout(transport_row);
    layout->addWidget(waveform_view_, 1);
    layout->addLayout(options_row);
    layout->addWidget(status_label_);

    update_controls_enabled();
}

PreviewPanel::~PreviewPanel() {
    cancel_waveform();
    waveform_pool_.waitForDone();
    engine_.reset();  // stops the device and joins the decoder thread on the control thread
}

void PreviewPanel::load_preferences() {
    const auto stored = session_.preference(kVolumePreference);
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
        post(audio::transport::TogglePlayPause{});
        return;
    }
    open_selected(true);
}

void PreviewPanel::play_selected() {
    if (engine_ && loaded_key_ && loaded_key_ == selected_key_) {
        post(audio::transport::Play{});
        return;
    }
    open_selected(true);
}

void PreviewPanel::stop() {
    post(audio::transport::Stop{});
}

void PreviewPanel::open_selected(bool autoplay) {
    if (!selected_key_ || !session_.is_ready()) return;
    const SampleKey key = *selected_key_;
    const std::uint64_t request = ++open_request_;
    status_label_->setText(tr("Opening…"));
    session_.run(
        this,
        [key](app::Application& application) { return application.resolve_playable_path(key.library, key.sample_id); },
        [this, key, request, autoplay](Result<std::filesystem::path> path) {
            if (request != open_request_ || key != selected_key_) return;  // selection moved on
            if (!path) {
                status_label_->setText(tr("Cannot preview: %1").arg(DisplayFormat::error(path.error())));
                emit statusMessage(status_label_->text());
                return;
            }
            on_path_resolved(key, *path, autoplay);
        });
}

void PreviewPanel::on_path_resolved(const SampleKey& key, const std::filesystem::path& path, bool autoplay) {
    selected_path_ = path;
    // Previewing authorizes bounded decoding of this one file for its waveform.
    if (waveform_view_->state() != WaveformView::State::Ready && waveform_key_ != key) start_waveform(key, path);
    if (!ensure_engine()) return;
    if (auto opened = engine_->open(path); !opened) {
        status_label_->setText(tr("Cannot preview: %1").arg(DisplayFormat::error(opened.error())));
        return;
    }
    loaded_key_ = key;
    loaded_generation_ = engine_->snapshot().generation;
    status_label_->clear();
    post(audio::transport::SetSpeed{speed_spin_->value()});
    if (autoplay) post(audio::transport::Play{});
    update_controls_enabled();
}

bool PreviewPanel::ensure_engine() {
    if (engine_) return true;
    if (engine_unavailable_) return false;
    audio::PreviewOptions options;
    options.backend = backend_from_environment();
    auto created = audio::PreviewEngine::create(options);
    if (!created) {
        engine_unavailable_ = true;
        status_label_->setText(tr("Audio output unavailable: %1").arg(DisplayFormat::error(created.error())));
        update_controls_enabled();
        return false;
    }
    engine_ = std::move(*created);
    const int decibels = volume_slider_->value();
    if (decibels <= kMinimumVolumeDecibels)
        engine_->post(audio::transport::SetVolumeLinear{0.0});
    else
        engine_->post(audio::transport::SetVolumeDecibels{static_cast<double>(decibels)});
    return true;
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
    play_button_->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    play_button_->setAccessibleName(playing ? tr("Pause") : tr("Play"));

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
    const QString total =
        known_length ? format_clock(*snapshot.total_frames, snapshot.source_sample_rate) : QStringLiteral("–:––");
    time_label_->setText(QStringLiteral("%1 / %2%3")
                             .arg(format_clock(snapshot.position_frames, snapshot.source_sample_rate), total,
                                  snapshot.total_frames_exact || !known_length ? QString() : tr(" (estimated)")));
    const bool showing_loaded =
        loaded_key_ && loaded_key_ == selected_key_ && snapshot.generation == loaded_generation_;
    waveform_view_->set_playhead(showing_loaded ? fraction : std::nullopt);
    seek_slider_->setEnabled(showing_loaded && known_length);
    waveform_view_->set_seekable(showing_loaded && known_length);

    if (snapshot.last_error) {
        const QString error = DisplayFormat::from_utf8(mso::to_string(*snapshot.last_error));
        status_label_->setText(*snapshot.last_error == ErrorCode::DeviceLost
                                   ? tr("Audio device lost (%1); playback paused.").arg(error)
                                   : tr("Playback error: %1").arg(error));
    }
    if (loop_check_->isChecked() && known_length && showing_loaded && loop_generation_ != snapshot.generation)
        apply_loop();

    const bool active = playing || snapshot.state == audio::PreviewState::Loading;
    if (!active) poll_timer_.stop();
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
            analysis::AnalysisRequest analysis_request;
            analysis_request.file = path;
            analysis_request.stages = {analysis::Stage::Waveform};
            analysis_request.waveform_bins = kWaveformBins;
            return analysis::analyze_file(analysis_request, stop, [this, request](double fraction) {
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
        [this, key, request](Result<analysis::AnalysisReport> report) {
            if (request != waveform_request_) return;
            waveform_key_.reset();
            if (!report) {
                if (report.error().code != ErrorCode::Cancelled && key == selected_key_)
                    waveform_view_->show_failed(DisplayFormat::error(report.error()));
                return;
            }
            if (!report->waveform) {
                if (key == selected_key_) waveform_view_->show_failed(tr("no audio was decoded"));
                return;
            }
            WaveformBins bins{std::move(report->waveform->minimum), std::move(report->waveform->maximum),
                              std::move(report->waveform->rms)};
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
    const bool has_selection = selected_key_.has_value() && !engine_unavailable_;
    play_button_->setEnabled(has_selection);
    stop_button_->setEnabled(engine_ != nullptr);
    generate_button_->setEnabled(selected_key_.has_value() && waveform_view_->state() != WaveformView::State::Ready);
    const bool showing_loaded = loaded_key_ && loaded_key_ == selected_key_;
    seek_slider_->setEnabled(showing_loaded && total_frames_.has_value());
}

}  // namespace mso::ui
