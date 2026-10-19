#include "mso/audio/playback_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <new>

namespace mso::audio {
namespace {

/// Sleep between passes while playing: short against the prebuffer, so the ring never runs dry.
constexpr std::chrono::milliseconds kPlayingPollInterval{5};
/// Sleep while idle; commands and device events wake the thread earlier.
constexpr std::chrono::milliseconds kIdlePollInterval{50};
/// Blocks written per pass before commands are checked again (~43 ms at 48 kHz).
constexpr int kBlocksPerPass = 8;

template <class... Handlers>
struct Overloaded : Handlers... {
    using Handlers::operator()...;
};

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
    {
        const std::scoped_lock lock(request_mutex_);
        pending_request_ = SourceRequest{generation, std::move(origin)};
    }
    wakeup_.notify();
}

void PlaybackController::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        adopt_requested_source();
        follow_output_device();
        while (const std::optional<TransportCommand> command = mailbox_.take()) {
            apply(*command);
        }
        handle_end_of_stream();
        if (!produce()) {
            wakeup_.wait_for(state_ == PreviewState::Playing ? kPlayingPollInterval : kIdlePollInterval);
        }
    }
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

    shared_.output_enabled.store(false, std::memory_order_release);
    stream_.reset();
    shape_.loop.reset();  // loop regions belong to a file; speed carries over to the next one
    begin_epoch(0);       // flush the previous source
    publish_source_facts();

    auto stream = open_stream(std::move(request->origin));
    if (stream) {
        stream_ = std::move(*stream);
        publish_source_facts();
        restart_at(0);
        if (stream_) {
            // A missing endpoint stays reported; the new source itself opened fine.
            shared_.last_error.store(device_loss_handled_ ? std::optional{ErrorCode::DeviceLost} : std::nullopt);
            set_state(PreviewState::Ready);
        }
    } else {
        shared_.last_error.store(stream.error().code);
        set_state(PreviewState::Error);
    }
    shared_.active_generation.store(request->generation, std::memory_order_release);
}

Result<std::unique_ptr<SourceStream>> PlaybackController::open_stream(SourceOrigin origin) {
    try {
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
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceLimit, "out of memory opening the preview source");
    }
}

void PlaybackController::publish_source_facts() {
    shared_.source_sample_rate.store(stream_ ? stream_->source_rate() : 0, std::memory_order_relaxed);
    shared_.source_channels.store(stream_ ? stream_->source_channels() : 0, std::memory_order_relaxed);
    const std::optional<std::int64_t> total = stream_ ? stream_->total_frames() : std::nullopt;
    shared_.total_frames.store(total.value_or(-1), std::memory_order_relaxed);
    shared_.total_frames_exact.store(stream_ && total && stream_->total_frames_exact(), std::memory_order_relaxed);
}

// ---------------------------------------------------------------- device

void PlaybackController::follow_output_device() {
    if (shared_.device_lost.load(std::memory_order_acquire) && !device_loss_handled_) {
        device_loss_handled_ = true;
        shared_.output_enabled.store(false, std::memory_order_release);
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
    output_rate_ = shared_.output_sample_rate.load(std::memory_order_relaxed);
    const bool recovering_from_loss = device_loss_handled_;
    device_loss_handled_ = false;
    if (recovering_from_loss && shared_.last_error.load() == ErrorCode::DeviceLost) {
        shared_.last_error.store(std::nullopt);
    }
    restart_at(audible_frame());  // re-render for the new rate from the audible position
    if (recovering_from_loss && stream_) {
        set_state(PreviewState::Paused);
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
               },
               command);
}

void PlaybackController::play() {
    // Error with a loaded source means the device is gone: playing needs a new endpoint first.
    if (!stream_ || state_ == PreviewState::Error || !output_ready()) {
        return;
    }
    shared_.output_enabled.store(true, std::memory_order_release);
    set_state(PreviewState::Playing);
}

void PlaybackController::pause() {
    if (state_ != PreviewState::Playing) {
        return;
    }
    shared_.output_enabled.store(false, std::memory_order_release);
    set_state(PreviewState::Paused);
}

void PlaybackController::stop() {
    if (!stream_) {
        return;
    }
    shared_.output_enabled.store(false, std::memory_order_release);
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
    if (region && region->start_frame >= 0 && region->end_frame > region->start_frame) {
        shape_.loop = region;
    } else {
        shape_.loop.reset();
    }
    restart_at(audible_frame());
}

void PlaybackController::set_speed(double ratio) {
    const std::int64_t speed_permille = speed_permille_from_ratio(ratio);
    if (speed_permille == shape_.speed_permille) {
        return;
    }
    shape_.speed_permille = speed_permille;
    shared_.speed_ratio.store(static_cast<double>(speed_permille) / static_cast<double>(kUnitySpeedPermille),
                              std::memory_order_relaxed);
    restart_at(audible_frame());
}

void PlaybackController::set_pitch(double semitones) {
    const double pitch = clamped_pitch_semitones(semitones);
    if (pitch == shape_.pitch_semitones) {
        return;
    }
    shape_.pitch_semitones = pitch;
    shared_.pitch_semitones.store(pitch, std::memory_order_relaxed);
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
    shared_.playback_mode.store(mode, std::memory_order_relaxed);
    restart_at(audible_frame());
}

void PlaybackController::handle_end_of_stream() {
    if (!stream_ || shared_.ended_epoch.load(std::memory_order_acquire) != epoch_) {
        return;
    }
    if (state_ == PreviewState::Playing || state_ == PreviewState::Paused) {
        shared_.output_enabled.store(false, std::memory_order_release);
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
            fail_source(rendered.error());
            return false;
        }
        slot->header->epoch = epoch_;
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
        fail_source(restarted.error());
        return;
    }
    begin_epoch(stream_->anchor_frame());
}

void PlaybackController::begin_epoch(std::int64_t start_frame) {
    ++epoch_;
    produced_frames_ = 0;
    end_of_stream_written_ = false;
    shared_.epoch_produced_frames.store(0, std::memory_order_relaxed);
    shared_.epoch_start_frame.store(start_frame, std::memory_order_relaxed);
    shared_.pcm_epoch.store(epoch_, std::memory_order_release);
}

void PlaybackController::fail_source(const Error& error) {
    shared_.output_enabled.store(false, std::memory_order_release);
    stream_.reset();
    begin_epoch(0);  // flush whatever was buffered; replay needs a new open()
    shared_.last_error.store(error.code);
    set_state(PreviewState::Error);
}

void PlaybackController::set_state(PreviewState state) {
    state_ = state;
    shared_.state.store(state, std::memory_order_release);
}

// ---------------------------------------------------------------- positions

bool PlaybackController::output_ready() const noexcept {
    return shared_.output_available.load(std::memory_order_acquire) &&
           !shared_.device_lost.load(std::memory_order_acquire);
}

std::int64_t PlaybackController::audible_frame() const noexcept {
    if (shared_.played_epoch.load(std::memory_order_acquire) == epoch_) {
        return shared_.played_frame.load(std::memory_order_relaxed);
    }
    return shared_.epoch_start_frame.load(std::memory_order_relaxed);
}

std::int64_t PlaybackController::buffered_frames() const noexcept {
    const bool playing_this_epoch = shared_.played_epoch.load(std::memory_order_acquire) == epoch_;
    const std::int64_t played = playing_this_epoch ? shared_.played_epoch_frames.load(std::memory_order_relaxed) : 0;
    return std::max<std::int64_t>(0, produced_frames_ - played);
}

std::int64_t PlaybackController::prebuffer_target_frames() const noexcept {
    const std::int64_t wanted = std::int64_t{prebuffer_milliseconds_} * output_rate_ / 1000;
    // Keep half the ring free so a fresh epoch can fill while superseded blocks await release.
    const auto half_ring = static_cast<std::int64_t>(ring_.frame_capacity() / 2);
    return std::clamp<std::int64_t>(wanted, kPcmBlockFrames, half_ring);
}

}  // namespace mso::audio
