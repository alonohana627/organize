#pragma once
/// @file
/// The real-time half of preview: turns PCM ring blocks into device output.
#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "mso/audio/linear_ramp.hpp"
#include "mso/audio/pcm_ring.hpp"
#include "mso/audio/shared_playback_state.hpp"

namespace mso::audio {

/// Runs inside the device's audio callback (or render_offline). Real-time safe: no allocation,
/// locks, I/O, logging or decoding; only ring reads, atomics and per-sample arithmetic.
///
/// Per callback it drops ring blocks of superseded epochs, plays blocks of the current epoch while
/// output is enabled, fades 5 ms in/out on play/pause, smooths volume per sample, decays the last
/// output value to zero whenever audio stops abruptly (flush, underrun), clamps the result to ±1,
/// and publishes the audible position, end-of-stream and underrun count.
class PlaybackRenderer {
public:
    PlaybackRenderer(PcmRing& ring, SharedPlaybackState& shared) noexcept;

    PlaybackRenderer(const PlaybackRenderer&) = delete;
    PlaybackRenderer& operator=(const PlaybackRenderer&) = delete;

    /// Sets the ramp lengths for a device rate. Call only while no callback can run.
    void configure(std::uint32_t output_rate) noexcept;

    /// Fills all of `stereo_output` (interleaved left/right).
    void render(std::span<float> stereo_output) noexcept;

private:
    /// Adopts a newer epoch: drops superseded blocks and, when output is off, cuts the fade-out.
    void follow_current_epoch(bool output_enabled) noexcept;
    bool read_frame(float& left, float& right) noexcept;
    void finish_block() noexcept;
    void start_decay_from_last_output() noexcept;
    void publish_position() noexcept;

    PcmRing& ring_;
    SharedPlaybackState& shared_;

    std::uint64_t epoch_ = 0;                        ///< newest epoch this renderer knows about
    std::optional<ReadablePcmBlock> current_block_;  ///< block being played (still owned by the ring)
    std::uint32_t frames_into_block_ = 0;

    std::uint64_t position_epoch_ = 0;  ///< epoch of the last frame played
    std::int64_t position_frame_ = 0;   ///< folded source position after that frame
    std::int64_t epoch_frames_played_ = 0;

    LinearRamp transport_gain_;  ///< 0 → 1 while output is enabled (play), 1 → 0 when disabled
    LinearRamp volume_gain_;     ///< smoothed preview volume

    std::array<float, 2> last_output_{};  ///< last frame written to the device
    std::array<float, 2> decay_start_{};  ///< level the declick decay started from
    std::uint32_t decay_length_ = 1;
    std::uint32_t decay_remaining_ = 0;
    bool previous_frame_had_audio_ = false;
};

}  // namespace mso::audio
