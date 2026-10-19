#pragma once
/// @file
/// The real-time half of preview: turns PCM ring blocks into device output.
#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "mso/audio/detail/linear_ramp.hpp"
#include "mso/audio/detail/pcm_ring.hpp"
#include "mso/audio/detail/playhead_history.hpp"
#include "mso/audio/detail/shared_playback_state.hpp"

namespace mso::audio::detail {

/// Runs inside the device's audio callback (or render_offline). Real-time safe: no allocation,
/// locks, I/O, logging or decoding; only ring reads, atomics and per-sample arithmetic.
///
/// Per callback it drops ring blocks of superseded epochs (and of another device rate), plays
/// blocks of the current epoch while output is enabled, smooths volume per sample, decays the last
/// output value to zero whenever audio is cut (every epoch change, flush, underrun), clamps the
/// result to ±1, and publishes consumption, the latency-compensated audible position, end-of-stream
/// and underruns.
///
/// Fades never touch intact audio: a fresh epoch starts at full gain (its first frame is either
/// frame 0 of the source or already faded in by the decoder thread), while the previous epoch's last
/// output value decays over 5 ms on top of it, so a seek or re-anchor while playing never steps.
/// Only resuming after a pause, an underrun or a device change, which re-enters the signal mid-way,
/// fades in over 5 ms; pausing fades out.
class PlaybackRenderer {
public:
    /// Renderer over `ring`, publishing into `shared`. configure() must run before the first render().
    PlaybackRenderer(PcmRing& ring, SharedPlaybackState& shared) noexcept;

    PlaybackRenderer(const PlaybackRenderer&) = delete;
    PlaybackRenderer& operator=(const PlaybackRenderer&) = delete;

    /// Sets the device rate (ramp lengths, accepted blocks) and the output latency the playhead is
    /// compensated for, for a device that is about to start. The playhead history restarts from the
    /// position last heard on the previous device, a held block of another rate is dropped on the
    /// next render, and output that is enabled fades back in. Call only while no callback can run.
    void configure(std::uint32_t output_rate, std::uint32_t latency_frames) noexcept;

    /// Fills all of `stereo_output` (interleaved left/right).
    void render(std::span<float> stereo_output) noexcept;

private:
    /// Adopts the decoder thread's epoch and output switch: drops superseded blocks, cuts the old
    /// audio, and sets the transport gain for the new state.
    void adopt_gate(TransportGate gate) noexcept;
    /// True while frames should be pulled from the ring (playing, or a pause fade still audible).
    [[nodiscard]] bool consuming() const noexcept {
        return transport_gain_.target() > 0.0f || transport_gain_.value() > 0.0f;
    }
    /// Reads the next frame of the current epoch; false when there is none (or output just stopped).
    [[nodiscard]] bool read_frame(float& left, float& right) noexcept;
    /// True when a block must not be played: an older epoch, or rendered for another device rate.
    [[nodiscard]] bool is_superseded(const PcmBlockHeader& header) const noexcept;
    /// Releases the current block (reporting end of stream when it carries the flag).
    void finish_block() noexcept;
    /// Releases the held block unplayed when it belongs to an older epoch or another device rate.
    void release_superseded_held_block() noexcept;
    /// Starts the decay of the last output value when audio was audible: avoids a step to silence or
    /// to another place in the source.
    void cut_audio() noexcept;
    /// Publishes consumption and the audible position for this callback.
    void publish_progress() noexcept;

    PcmRing& ring_;                ///< blocks from the decoder thread
    SharedPlaybackState& shared_;  ///< published status

    std::uint32_t output_rate_ = 0;     ///< device rate; blocks of other rates are dropped
    std::uint64_t latency_frames_ = 0;  ///< device buffer the playhead lags by
    std::uint64_t epoch_ = 0;           ///< newest epoch adopted
    bool output_enabled_ = false;       ///< output switch adopted with epoch_

    std::optional<ReadablePcmBlock> current_block_;  ///< block being played (still owned by the ring)
    std::uint32_t frames_into_block_ = 0;            ///< frames of current_block_ already played

    std::uint64_t position_epoch_ = 0;      ///< epoch of the last frame consumed
    std::int64_t position_frame_ = 0;       ///< folded source position after that frame
    std::int64_t epoch_frames_played_ = 0;  ///< frames consumed from position_epoch_
    std::uint64_t output_frames_ = 0;       ///< frames written to the device since creation
    PlayheadHistory playhead_history_;      ///< consumption history for latency compensation

    LinearRamp transport_gain_;  ///< 0 → 1 while output is enabled, 1 → 0 when disabled
    LinearRamp volume_gain_;     ///< smoothed preview volume

    std::array<float, 2> last_output_{};     ///< last frame written to the device
    std::array<float, 2> decay_start_{};     ///< level the declick decay started from
    std::uint32_t decay_length_ = 1;         ///< decay duration in frames (~5 ms)
    std::uint32_t decay_remaining_ = 0;      ///< frames of decay still to add
    bool previous_frame_had_audio_ = false;  ///< the last frame came from the ring
    bool fade_in_after_underrun_ = false;    ///< the next frame re-enters the signal after starving
    bool dropped_foreign_rate_ = false;      ///< this callback dropped blocks of another device rate
};

}  // namespace mso::audio::detail
