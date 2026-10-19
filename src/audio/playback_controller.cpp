#include "mso/audio/detail/playback_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <new>

#include "mso/audio/detail/denormal_flush.hpp"
#include "mso/audio/detail/overloaded.hpp"

namespace mso::audio::detail {
namespace {

/// Sleep between passes while playing: short against the prebuffer, so the ring never runs dry.
constexpr std::chrono::milliseconds kPlayingPollInterval{5};
/// Sleep while idle; commands and device events wake the thread earlier.
constexpr std::chrono::milliseconds kIdlePollInterval{50};
/// Blocks written per pass before commands are checked again (~43 ms at 48 kHz).
constexpr int kBlocksPerPass = 8;

}  // namespace

PlaybackController::PlaybackController(PcmRing& ring, SharedPlaybackState& shared, CommandMailbox& mailbox,
                                       WakeupSignal& wakeup, std::uint32_t prebuffer_milliseconds)
    : ring_(ring),
      shared_(shared),
      mailbox_(mailbox),
      wakeup_(wakeup),
      prebuffer_milliseconds_(prebuffer_milliseconds),
      thread_([this](std::stop_token stop) { run(stop); }) {}

PlaybackController::~PlaybackController() {
    thread_.request_stop();
    wakeup_.notify();
    thread_.join();
}

void PlaybackController::request_source(std::uint64_t generation, SourceOrigin origin) {
    std::optional<SourceRequest> superseded;
    {
        const std::scoped_lock lock(request_mutex_);
        superseded = std::move(pending_request_);
        pending_request_ = SourceRequest{generation, std::move(origin)};
    }
    superseded.reset();  // a superseded decoder may close a file: never under the lock
    wakeup_.notify();
}

// ---------------------------------------------------------------- thread body

void PlaybackController::run(std::stop_token stop) {
    const ScopedDenormalFlush denormals_flushed;
    while (!stop.stop_requested()) {
        bool more_work = false;
        try {
            more_work = run_pass();
            if (!more_work) {
                wakeup_.wait_for(state_ == PreviewState::Playing ? kPlayingPollInterval : kIdlePollInterval);
            }
        } catch (const std::bad_alloc&) {
            fail_from_exception(ErrorCode::ResourceLimit);
        } catch (const std::exception&) {
            fail_from_exception(ErrorCode::Internal);
        }
    }
}

bool PlaybackController::run_pass() {
    adopt_requested_source();
    follow_output_device();
    while (const std::optional<TransportCommand> command = mailbox_.take()) {
        apply(*command);
    }
    handle_end_of_stream();
    const bool more_work = produce();
    publish_settings();
    return more_work;
}

void PlaybackController::fail_from_exception(ErrorCode code) noexcept {
    // The exception's message is not kept: copying it could throw again. The code says what happened.
    fail_source(code);
    // A request that threw while being opened must not leave the snapshot in Loading forever.
    shared_.active_generation.store(shared_.requested_generation.load(std::memory_order_acquire),
                                    std::memory_order_release);
}

// ---------------------------------------------------------------- sources

void PlaybackController::adopt_requested_source() {
    std::optional<SourceRequest> request;
    {
        const std::scoped_lock lock(request_mutex_);
        request.swap(pending_request_);
    }
    if (!request) {
        return;
    }

    set_output_enabled(false);
    stream_.reset();
    shape_.loop.reset();  // loop regions belong to a file; speed, pitch and mode carry over
    begin_epoch(0);       // flush the previous source
    publish_source_facts();

    auto stream = open_stream(std::move(request->origin));
    if (stream) {
        stream_ = std::move(*stream);
        publish_source_facts();
        restart_at(0);
        if (stream_) {
            if (state_before_device_loss_) {
                state_before_device_loss_ = PreviewState::Ready;  // what to show once a device returns
            }
            // A missing endpoint stays reported; the new source itself opened fine.
            shared_.last_error.store(state_before_device_loss_ ? std::optional{ErrorCode::DeviceLost} : std::nullopt);
            set_state(PreviewState::Ready);
        }
    } else {
        shared_.last_error.store(stream.error().code);
        set_state(PreviewState::Error);
    }
    shared_.active_generation.store(request->generation, std::memory_order_release);
}

Result<std::unique_ptr<SourceStream>> PlaybackController::open_stream(SourceOrigin origin) {
    std::unique_ptr<media::Decoder> decoder;
    if (auto* path = std::get_if<std::filesystem::path>(&origin)) {
        auto opened = media::open_decoder(*path);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        decoder = std::move(*opened);
    } else {
        decoder = std::move(std::get<std::unique_ptr<media::Decoder>>(origin));
    }
    return SourceStream::create(std::move(decoder));
}

void PlaybackController::publish_source_facts() {
    shared_.source_sample_rate.store(stream_ ? stream_->source_rate() : 0, std::memory_order_relaxed);
    shared_.source_channels.store(stream_ ? stream_->source_channels() : 0, std::memory_order_relaxed);
    const std::optional<std::int64_t> total = stream_ ? stream_->total_frames() : std::nullopt;
    shared_.total_frames.store(total.value_or(-1), std::memory_order_relaxed);
    shared_.total_frames_exact.store(stream_ && total && stream_->total_frames_exact(), std::memory_order_relaxed);
}

void PlaybackController::publish_settings() {
    const std::optional<LoopRegion> loop = stream_ ? stream_->effective_loop() : std::nullopt;
    PlaybackSettings settings;
    settings.mode = shape_.mode;
    settings.seam_fades = shape_.seam_fades;
    settings.speed_ratio = static_cast<double>(shape_.speed_permille) / static_cast<double>(kUnitySpeedPermille);
    settings.pitch_semitones = shape_.pitch_semitones;
    settings.loop_start_frame = loop ? loop->start_frame : 0;
    settings.loop_end_frame = loop ? loop->end_frame : 0;
    shared_.settings.store(settings);
}

// ---------------------------------------------------------------- device

void PlaybackController::follow_output_device() {
    if (shared_.device_lost.load(std::memory_order_acquire) && !state_before_device_loss_) {
        state_before_device_loss_ = state_;
        set_output_enabled(false);
        // Flush, but remember where the listener was: a new endpoint resumes there, never at 0.
        restart_at(audible_frame());
        shared_.last_error.store(ErrorCode::DeviceLost);
        if (stream_) {
            set_state(PreviewState::Error);
        }
    }

    const std::uint64_t configuration = shared_.output_configuration.load(std::memory_order_acquire);
    if (configuration == seen_output_configuration_) {
        return;
    }
    seen_output_configuration_ = configuration;
    output_rate_ = shared_.output_facts.load().sample_rate;
    restart_at(audible_frame());  // re-render for the new rate from the audible position
    if (state_before_device_loss_ && !shared_.device_lost.load(std::memory_order_acquire)) {
        const PreviewState previous = *state_before_device_loss_;
        state_before_device_loss_.reset();
        if (shared_.last_error.load() == ErrorCode::DeviceLost) {
            shared_.last_error.store(std::nullopt);
        }
        if (stream_) {
            // Never resume by itself on a new endpoint: playback comes back paused where it was.
            set_state(previous == PreviewState::Playing ? PreviewState::Paused : previous);
        }
    }
}

// ---------------------------------------------------------------- commands

void PlaybackController::apply(const TransportCommand& command) {
    std::visit(Overloaded{
                   [this](const transport::Play&) { play(); },
                   [this](const transport::Pause&) { pause(); },
                   [this](const transport::TogglePlayPause&) {
                       if (state_ == PreviewState::Playing) {
                           pause();
                       } else {
                           play();
                       }
                   },
                   [this](const transport::Stop&) { stop(); },
                   [this](const transport::SeekToFrame& seek) { restart_at(seek.source_frame); },
                   [this](const transport::SeekToFraction& seek) { seek_to_fraction(seek.fraction); },
                   [this](const transport::SetVolumeDecibels& volume) {
                       shared_.target_volume.store(linear_gain_from_decibels(volume.decibels),
                                                   std::memory_order_relaxed);
                   },
                   [this](const transport::SetVolumeLinear& volume) {
                       shared_.target_volume.store(clamped_linear_gain(volume.gain), std::memory_order_relaxed);
                   },
                   [this](const transport::SetLoop& loop) { set_loop(loop.region); },
                   [this](const transport::SetSpeed& speed) { set_speed(speed.ratio); },
                   [this](const transport::SetPitch& pitch) { set_pitch(pitch.semitones); },
                   [this](const transport::SetPlaybackMode& mode) { set_playback_mode(mode.mode); },
                   [this](const transport::SetSeamFades& fades) { set_seam_fades(fades.enabled); },
               },
               command);
}

void PlaybackController::play() {
    // Error with a loaded source means the device is gone: playing needs a new endpoint first.
    if (!stream_ || state_ == PreviewState::Error || !output_ready()) {
        return;
    }
    set_output_enabled(true);
    set_state(PreviewState::Playing);
}

void PlaybackController::pause() {
    if (state_ != PreviewState::Playing) {
        return;
    }
    set_output_enabled(false);
    set_state(PreviewState::Paused);
}

void PlaybackController::stop() {
    if (!stream_) {
        return;
    }
    set_output_enabled(false);
    restart_at(0);
    if (stream_ && state_ != PreviewState::Error) {
        set_state(PreviewState::Stopped);
    }
}

void PlaybackController::seek_to_fraction(double fraction) {
    if (!stream_) {
        return;
    }
    if (const std::optional<std::int64_t> total = stream_->total_frames()) {
        restart_at(static_cast<std::int64_t>(std::llround(fraction * static_cast<double>(*total))));
    } else if (fraction == 0.0) {
        restart_at(0);
    }
    // Other fractions of an unknown length cannot be mapped to a frame and are ignored.
}

void PlaybackController::set_loop(const std::optional<LoopRegion>& region) {
    std::optional<LoopRegion> requested;
    if (region && region->start_frame >= 0 && region->end_frame > region->start_frame) {
        requested = region;
    }
    if (requested == shape_.loop) {
        return;  // nothing changes: keep the buffered audio
    }
    shape_.loop = requested;
    restart_at(audible_frame());
}

void PlaybackController::set_speed(double ratio) {
    const std::int64_t speed_permille = speed_permille_from_ratio(ratio);
    if (speed_permille == shape_.speed_permille) {
        return;
    }
    shape_.speed_permille = speed_permille;
    restart_at(audible_frame());
}

void PlaybackController::set_pitch(double semitones) {
    const double pitch = clamped_pitch_semitones(semitones);
    if (pitch == shape_.pitch_semitones) {
        return;
    }
    shape_.pitch_semitones = pitch;
    if (shape_.mode == PlaybackMode::IndependentTempoPitch) {
        restart_at(audible_frame());  // Varispeed keeps the setting for later without touching audio
    }
}

void PlaybackController::set_playback_mode(PlaybackMode mode) {
    const bool known = mode == PlaybackMode::Varispeed || mode == PlaybackMode::IndependentTempoPitch;
    if (!known || mode == shape_.mode) {
        return;
    }
    shape_.mode = mode;
    restart_at(audible_frame());
}

void PlaybackController::set_seam_fades(bool enabled) {
    if (enabled == shape_.seam_fades) {
        return;
    }
    shape_.seam_fades = enabled;
    if (shape_.loop) {
        restart_at(audible_frame());  // only loop seams are affected
    }
}

void PlaybackController::handle_end_of_stream() {
    if (!stream_ || shared_.ended_epoch.load(std::memory_order_acquire) != gate_.epoch) {
        return;
    }
    if (state_ == PreviewState::Playing || state_ == PreviewState::Paused) {
        set_output_enabled(false);
        restart_at(0);
        if (stream_) {
            set_state(PreviewState::Stopped);
        }
    }
}

// ---------------------------------------------------------------- production

bool PlaybackController::produce() {
    if (!stream_ || end_of_stream_written_) {
        return false;
    }
    for (int pass_block = 0; pass_block < kBlocksPerPass; ++pass_block) {
        if (buffered_frames() >= prebuffer_target_frames()) {
            return false;
        }
        const std::optional<WritablePcmBlock> slot = ring_.begin_write();
        if (!slot) {
            return false;  // the callback still holds superseded blocks; it frees them every period
        }
        if (auto rendered = stream_->render_block(*slot->header, slot->samples); !rendered) {
            fail_source(rendered.error().code);
            return false;
        }
        if (slot->header->frame_count == 0 && !slot->header->end_of_stream) {
            // Defensive: an empty block that is not the end would make this loop spin.
            fail_source(ErrorCode::Internal);
            return false;
        }
        slot->header->epoch = gate_.epoch;
        slot->header->output_rate = output_rate_;
        const bool end_of_stream = slot->header->end_of_stream;
        produced_frames_ += slot->header->frame_count;
        ring_.commit_write();
        shared_.epoch_produced_frames.store(produced_frames_, std::memory_order_release);
        if (end_of_stream) {
            end_of_stream_written_ = true;
            return false;
        }
    }
    return true;
}

void PlaybackController::restart_at(std::int64_t source_frame) {
    if (!stream_) {
        return;
    }
    if (auto restarted = stream_->restart(source_frame, shape_, output_rate_); !restarted) {
        fail_source(restarted.error().code);
        return;
    }
    begin_epoch(stream_->anchor_frame());
}

void PlaybackController::begin_epoch(std::int64_t start_frame) noexcept {
    ++gate_.epoch;
    produced_frames_ = 0;
    end_of_stream_written_ = false;
    shared_.epoch_produced_frames.store(0, std::memory_order_relaxed);
    shared_.epoch_start_frame.store(start_frame, std::memory_order_relaxed);
    shared_.transport_gate.store(gate_.packed(), std::memory_order_release);
}

void PlaybackController::set_output_enabled(bool enabled) noexcept {
    gate_.output_enabled = enabled;
    shared_.transport_gate.store(gate_.packed(), std::memory_order_release);
}

void PlaybackController::fail_source(ErrorCode code) noexcept {
    set_output_enabled(false);
    stream_.reset();
    begin_epoch(0);  // flush whatever was buffered; replay needs a new open()
    shared_.last_error.store(code);
    set_state(PreviewState::Error);
}

void PlaybackController::set_state(PreviewState state) noexcept {
    state_ = state;
    shared_.state.store(state, std::memory_order_release);
}

// ---------------------------------------------------------------- positions

bool PlaybackController::output_ready() const noexcept {
    return shared_.output_available.load(std::memory_order_acquire) &&
           !shared_.device_lost.load(std::memory_order_acquire);
}

std::int64_t PlaybackController::audible_frame() const noexcept {
    const PlaybackProgress progress = shared_.progress.load();
    if (progress.audible_epoch == gate_.epoch) {
        return progress.audible_frame;
    }
    return shared_.epoch_start_frame.load(std::memory_order_relaxed);
}

std::int64_t PlaybackController::buffered_frames() const noexcept {
    const PlaybackProgress progress = shared_.progress.load();
    const std::int64_t played = progress.consumed_epoch == gate_.epoch ? progress.consumed_frames : 0;
    return std::max<std::int64_t>(0, produced_frames_ - played);
}

std::int64_t PlaybackController::prebuffer_target_frames() const noexcept {
    const std::int64_t wanted = std::int64_t{prebuffer_milliseconds_} * output_rate_ / 1000;
    // Keep half the ring free so a fresh epoch can fill while superseded blocks await release.
    const auto half_ring = static_cast<std::int64_t>(ring_.frame_capacity() / 2);
    return std::clamp<std::int64_t>(wanted, kPcmBlockFrames, half_ring);
}

}  // namespace mso::audio::detail
