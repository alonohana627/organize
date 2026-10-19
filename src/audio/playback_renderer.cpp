#include "mso/audio/playback_renderer.hpp"

#include <algorithm>

#include "mso/audio/sample_safety.hpp"

namespace mso::audio {
namespace {

/// Fade applied by the callback when output is enabled or disabled (play/pause).
constexpr double kTransportFadeMilliseconds = 5.0;
/// Time a full-scale volume change takes; smaller changes take proportionally less.
constexpr double kVolumeGlideMilliseconds = 20.0;
/// Decay of the last output value when audio stops abruptly (flush, underrun).
constexpr double kDeclickDecayMilliseconds = 5.0;

}  // namespace

PlaybackRenderer::PlaybackRenderer(PcmRing& ring, SharedPlaybackState& shared) noexcept
    : ring_(ring), shared_(shared), volume_gain_(1.0f, 1.0f) {}

void PlaybackRenderer::configure(std::uint32_t output_rate) noexcept {
    transport_gain_.set_step(LinearRamp::step_for(output_rate, kTransportFadeMilliseconds));
    volume_gain_.set_step(LinearRamp::step_for(output_rate, kVolumeGlideMilliseconds));
    decay_length_ = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(static_cast<double>(output_rate) * kDeclickDecayMilliseconds / 1000.0));
    decay_remaining_ = std::min(decay_remaining_, decay_length_);
}

void PlaybackRenderer::render(std::span<float> stereo_output) noexcept {
    const bool output_enabled = shared_.output_enabled.load(std::memory_order_acquire);
    follow_current_epoch(output_enabled);
    transport_gain_.set_target(output_enabled ? 1.0f : 0.0f);
    volume_gain_.set_target(shared_.target_volume.load(std::memory_order_relaxed));

    bool starved = false;
    const std::size_t frame_count = stereo_output.size() / kOutputChannels;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        float left = 0.0f;
        float right = 0.0f;
        // Keep consuming while a fade-out is still audible; stop once it reaches silence.
        const bool consuming = transport_gain_.target() > 0.0f || transport_gain_.value() > 0.0f;
        const bool has_audio = consuming && read_frame(left, right);
        starved = starved || (consuming && !has_audio);
        if (!has_audio && previous_frame_had_audio_) {
            start_decay_from_last_output();
        }
        previous_frame_had_audio_ = has_audio;

        const float gain = transport_gain_.next() * volume_gain_.next();
        left *= gain;
        right *= gain;
        if (decay_remaining_ > 0) {
            const float weight = static_cast<float>(decay_remaining_) / static_cast<float>(decay_length_);
            left += decay_start_[0] * weight;
            right += decay_start_[1] * weight;
            --decay_remaining_;
        }
        left = clamp_for_output(left);
        right = clamp_for_output(right);
        stereo_output[frame * kOutputChannels] = left;
        stereo_output[frame * kOutputChannels + 1] = right;
        last_output_ = {left, right};
    }

    publish_position();
    const bool primed = position_epoch_ == epoch_ && epoch_ != 0;
    const bool ended = shared_.ended_epoch.load(std::memory_order_relaxed) == epoch_;
    if (starved && output_enabled && primed && !ended) {
        shared_.underrun_count.fetch_add(1, std::memory_order_relaxed);
    }
}

void PlaybackRenderer::follow_current_epoch(bool output_enabled) noexcept {
    const std::uint64_t newest = shared_.pcm_epoch.load(std::memory_order_acquire);
    if (newest == epoch_) {
        return;
    }
    epoch_ = newest;
    if (!output_enabled) {
        // Stop, end of source, new source, device loss: the old audio is cut (and decayed below);
        // the new epoch's first frames must not leak out under a fade-out.
        transport_gain_.jump_to(0.0f);
    }
    if (current_block_ && current_block_->header->epoch < epoch_) {
        ring_.pop_front();
        current_block_.reset();
        if (previous_frame_had_audio_) {
            start_decay_from_last_output();
            previous_frame_had_audio_ = false;
        }
    }
    // Blocks are in production order, so every superseded block sits in front of the current ones.
    for (auto block = ring_.front(); block && block->header->epoch < epoch_; block = ring_.front()) {
        ring_.pop_front();
    }
}

bool PlaybackRenderer::read_frame(float& left, float& right) noexcept {
    for (;;) {
        if (!current_block_) {
            const std::optional<ReadablePcmBlock> block = ring_.front();
            if (!block) {
                return false;
            }
            if (block->header->epoch > epoch_) {
                // Written after this callback began: the published epoch is at least as new.
                epoch_ = shared_.pcm_epoch.load(std::memory_order_acquire);
            }
            if (block->header->epoch < epoch_) {
                ring_.pop_front();
                continue;
            }
            current_block_ = block;
            frames_into_block_ = 0;
        }

        const PcmBlockHeader& header = *current_block_->header;
        if (frames_into_block_ < header.frame_count) {
            left = current_block_->samples[frames_into_block_ * kOutputChannels];
            right = current_block_->samples[frames_into_block_ * kOutputChannels + 1];
            ++frames_into_block_;
            if (position_epoch_ != header.epoch) {
                position_epoch_ = header.epoch;
                epoch_frames_played_ = 0;
            }
            ++epoch_frames_played_;
            position_frame_ = header.source_frame_after(frames_into_block_);
            if (frames_into_block_ == header.frame_count) {
                finish_block();
            }
            return true;
        }
        finish_block();  // an empty block (end-of-stream marker)
    }
}

void PlaybackRenderer::finish_block() noexcept {
    const PcmBlockHeader& header = *current_block_->header;
    if (header.end_of_stream) {
        if (position_epoch_ != header.epoch) {
            position_epoch_ = header.epoch;
            epoch_frames_played_ = 0;
            position_frame_ = header.source_frame_after(header.frame_count);
        }
        shared_.ended_epoch.store(header.epoch, std::memory_order_release);
    }
    ring_.pop_front();
    current_block_.reset();
}

void PlaybackRenderer::start_decay_from_last_output() noexcept {
    decay_start_ = last_output_;
    decay_remaining_ = decay_length_;
}

void PlaybackRenderer::publish_position() noexcept {
    shared_.played_frame.store(position_frame_, std::memory_order_relaxed);
    shared_.played_epoch_frames.store(epoch_frames_played_, std::memory_order_relaxed);
    shared_.played_epoch.store(position_epoch_, std::memory_order_release);
}

}  // namespace mso::audio
