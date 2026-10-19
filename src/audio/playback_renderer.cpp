#include "mso/audio/detail/playback_renderer.hpp"

#include <algorithm>

#include "mso/audio/detail/sample_safety.hpp"

namespace mso::audio::detail {
namespace {

/// Fade when output is disabled (pause) or re-enters the signal (resume, underrun recovery).
constexpr double kTransportFadeMilliseconds = 5.0;
/// Time a full-scale volume change takes; smaller changes take proportionally less.
constexpr double kVolumeGlideMilliseconds = 20.0;
/// Decay of the last output value when audio is cut (flush, underrun).
constexpr double kDeclickDecayMilliseconds = 5.0;

}  // namespace

PlaybackRenderer::PlaybackRenderer(PcmRing& ring, SharedPlaybackState& shared) noexcept
    : ring_(ring), shared_(shared), volume_gain_(1.0f, 1.0f) {}

void PlaybackRenderer::configure(std::uint32_t output_rate, std::uint32_t latency_frames) noexcept {
    // The previous device's output frames and latency mean nothing to this one. Until this device
    // has played through its own latency, the position last heard on the previous one stands.
    playhead_history_.restart(playhead_history_.audible(output_frames_, latency_frames_));

    output_rate_ = output_rate;
    latency_frames_ = latency_frames;
    transport_gain_.set_step(LinearRamp::step_for(output_rate, kTransportFadeMilliseconds));
    volume_gain_.set_step(LinearRamp::step_for(output_rate, kVolumeGlideMilliseconds));
    decay_length_ = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(static_cast<double>(output_rate) * kDeclickDecayMilliseconds / 1000.0));
    // The device starts from silence: no decay carries over from the previous device, and audio that
    // is still enabled re-enters the signal mid-way, so it fades in.
    decay_remaining_ = 0;
    previous_frame_had_audio_ = false;
    last_output_ = {};
    if (output_enabled_) {
        transport_gain_.restart_from(0.0f);
    }
}

void PlaybackRenderer::render(std::span<float> stereo_output) noexcept {
    adopt_gate(TransportGate::unpacked(shared_.transport_gate.load(std::memory_order_acquire)));
    release_superseded_held_block();  // e.g. a block rendered for the previous device's rate
    volume_gain_.set_target(shared_.target_volume.load(std::memory_order_relaxed));
    dropped_foreign_rate_ = false;

    bool starved = false;
    const std::size_t frame_count = stereo_output.size() / kOutputChannels;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        float left = 0.0f;
        float right = 0.0f;
        const bool wanted = consuming();
        const bool has_audio = wanted && read_frame(left, right);
        if (wanted && !has_audio && output_enabled_) {
            starved = true;
            // Re-entering the signal after a gap is a mid-signal start: fade the next audio in.
            fade_in_after_underrun_ = position_epoch_ == epoch_;
        }
        if (has_audio && fade_in_after_underrun_) {
            transport_gain_.restart_from(0.0f);
            fade_in_after_underrun_ = false;
        }
        if (!has_audio && previous_frame_had_audio_) {
            cut_audio();
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
    output_frames_ += frame_count;

    publish_progress();
    const bool primed = position_epoch_ == epoch_ && epoch_ != 0;
    const bool ended = shared_.ended_epoch.load(std::memory_order_relaxed) == epoch_;
    if (starved && primed && !ended && !dropped_foreign_rate_) {
        shared_.underrun_count.fetch_add(1, std::memory_order_relaxed);
    }
}

void PlaybackRenderer::adopt_gate(TransportGate gate) noexcept {
    if (gate.epoch != epoch_) {
        epoch_ = gate.epoch;
        release_superseded_held_block();
        // Every epoch change moves to another place in the source (or to silence), whether or not a
        // block is held and wherever in the callback it happens: the old signal decays, never steps.
        cut_audio();
        // Blocks are in production order, so every superseded block sits in front of the current ones.
        for (auto block = ring_.front(); block && is_superseded(*block->header); block = ring_.front()) {
            ring_.pop_front();
        }
        fade_in_after_underrun_ = false;
        if (!gate.output_enabled) {
            // Stop, end of source, new source, device loss: nothing of the new epoch may leak out
            // under the old fade-out; the old audio is decayed by cut_audio().
            transport_gain_.jump_to(0.0f);
        }
    }
    if (gate.output_enabled != output_enabled_) {
        output_enabled_ = gate.output_enabled;
        if (!output_enabled_) {
            transport_gain_.set_target(0.0f);
        } else if (position_epoch_ != epoch_) {
            // A fresh epoch begins clean (frame 0, or faded in by the decoder thread): full gain at once.
            transport_gain_.jump_to(1.0f);
        } else {
            transport_gain_.set_target(1.0f);  // resuming mid-signal after a pause
        }
    }
}

bool PlaybackRenderer::is_superseded(const PcmBlockHeader& header) const noexcept {
    return header.epoch < epoch_ || header.output_rate != output_rate_;
}

bool PlaybackRenderer::read_frame(float& left, float& right) noexcept {
    for (;;) {
        if (!current_block_) {
            const std::optional<ReadablePcmBlock> block = ring_.front();
            if (!block) {
                return false;
            }
            // Copied first: adopting the gate may release this very block back to the producer.
            const std::uint64_t block_epoch = block->header->epoch;
            if (block_epoch > epoch_) {
                // Written after this callback began: the decoder thread published the gate before the
                // block, so adopting it now brings the matching output switch along.
                adopt_gate(TransportGate::unpacked(shared_.transport_gate.load(std::memory_order_acquire)));
                if (block_epoch > epoch_ || !consuming()) {
                    // Not yet announced (never happens with a well-behaved producer), or the new state
                    // stopped output: consume nothing of it in this frame.
                    return false;
                }
                continue;
            }
            if (is_superseded(*block->header)) {
                dropped_foreign_rate_ = dropped_foreign_rate_ || block->header->output_rate != output_rate_;
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

void PlaybackRenderer::release_superseded_held_block() noexcept {
    if (current_block_ && is_superseded(*current_block_->header)) {
        ring_.pop_front();
        current_block_.reset();
    }
}

void PlaybackRenderer::cut_audio() noexcept {
    if (!previous_frame_had_audio_) {
        return;
    }
    decay_start_ = last_output_;
    decay_remaining_ = decay_length_;
    previous_frame_had_audio_ = false;
}

void PlaybackRenderer::publish_progress() noexcept {
    playhead_history_.record({output_frames_, position_epoch_, position_frame_});
    PlaybackProgress progress;
    progress.consumed_epoch = position_epoch_;
    progress.consumed_frames = epoch_frames_played_;
    progress.consumed_position = position_frame_;
    if (const auto heard = playhead_history_.audible(output_frames_, latency_frames_)) {
        progress.audible_epoch = heard->epoch;
        progress.audible_frame = heard->position;
    }
    shared_.progress.store(progress);
}

}  // namespace mso::audio::detail
