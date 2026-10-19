#pragma once
/// @file
/// Decoder-thread pipeline from a media::Decoder to stereo ring blocks at the device rate.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "mso/audio/pcm_ring.hpp"
#include "mso/audio/source_position.hpp"
#include "mso/audio/stereo_converter.hpp"
#include "mso/audio/transport.hpp"
#include "mso/domain/error.hpp"
#include "mso/media/media.hpp"

namespace mso::audio {

class TempoPitchStretcher;

/// Loop, speed, pitch and mode applied to a source. Changing any audible one re-anchors playback
/// at the audible position.
struct PlaybackShape {
    std::optional<LoopRegion> loop;
    std::int64_t speed_permille = kUnitySpeedPermille;
    PlaybackMode mode = PlaybackMode::Varispeed;
    double pitch_semitones = 0.0;  ///< used by PlaybackMode::IndependentTempoPitch only
};

/// Streams one source in bounded chunks: reads from the decoder, scrubs NaN/Inf, applies the short
/// declick ramps (fade-in at every segment start, fade-out before a loop wrap), wraps loops, folds
/// channels to stereo and resamples into ring blocks. In Varispeed mode the resampler ratio carries
/// the speed; in IndependentTempoPitch mode audio is resampled at 1× and then time-stretched and
/// transposed by a TempoPitchStretcher. Streaming only: never more than one decoded chunk plus the
/// stretcher's look-ahead is held. Used only by the decoder thread.
class SourceStream {
public:
    /// Takes ownership of an opened decoder. Fails with UNSUPPORTED_CODEC for unusable formats.
    static Result<std::unique_ptr<SourceStream>> create(std::unique_ptr<media::Decoder> decoder);

    ~SourceStream();
    SourceStream(const SourceStream&) = delete;
    SourceStream& operator=(const SourceStream&) = delete;

    int source_rate() const noexcept { return source_rate_; }
    int source_channels() const noexcept { return static_cast<int>(channels_); }
    std::optional<std::int64_t> total_frames() const { return decoder_->total_frames(); }
    bool total_frames_exact() const { return decoder_->frames_exact(); }

    /// Starts a new segment at `source_frame` (clamped to the source) with `shape`, producing audio
    /// for a device at `output_rate`. Resets the resampler and begins with a fade-in.
    Result<void> restart(std::int64_t source_frame, const PlaybackShape& shape, std::uint32_t output_rate);

    /// Frame where the current segment started (the decoder's actual landing position after a seek).
    std::int64_t anchor_frame() const noexcept { return clock_.anchor_frame(); }

    /// Fills one ring block: up to kPcmBlockFrames stereo frames into `stereo_samples` and every
    /// header field except the epoch. Marks end_of_stream once the source is exhausted.
    /// A decoder failure (file vanished, corrupt data) is returned unchanged.
    Result<void> render_block(PcmBlockHeader& header, std::span<float> stereo_samples);

private:
    explicit SourceStream(std::unique_ptr<media::Decoder> decoder);

    /// Fills `stereo_output` with converted (folded + resampled) audio; fewer frames at the end.
    Result<std::size_t> convert_into(std::span<float> stereo_output);
    /// Fills `stereo_output` with time-stretched audio; returns the frames that carry sound.
    Result<std::size_t> stretch_into(std::span<float> stereo_output);
    Result<void> prime_stretcher(const PlaybackShape& shape, std::uint32_t output_rate);
    /// The source ran out: owe the output that the stretcher's look-ahead still holds.
    void start_stretch_tail() noexcept;
    Result<void> refill_staging();
    Result<void> wrap_to_loop_start();
    void apply_declick_ramps() noexcept;
    std::size_t staged_frames_remaining() const noexcept { return staged_frames_ - staged_offset_; }
    std::optional<std::int64_t> exact_total_frames() const;

    std::unique_ptr<media::Decoder> decoder_;
    std::unique_ptr<StereoConverter> converter_;
    std::size_t channels_ = 0;
    int source_rate_ = 0;
    std::int64_t ramp_frames_ = 1;  ///< declick ramp length in source frames (~5 ms)

    std::vector<float> staged_;  ///< one decoded chunk, interleaved source channels
    std::size_t staged_frames_ = 0;
    std::size_t staged_offset_ = 0;  ///< frames of the chunk already fed to the converter

    std::int64_t read_position_ = 0;        ///< next source frame the decoder will return
    std::int64_t segment_frames_read_ = 0;  ///< source frames since the last segment start or wrap
    std::int64_t output_frames_since_anchor_ = 0;
    SourceClock clock_;
    LoopFold loop_;
    bool input_exhausted_ = false;
    PlaybackMode mode_ = PlaybackMode::Varispeed;

    // Independent tempo/pitch mode.
    std::unique_ptr<TempoPitchStretcher> stretcher_;
    SourceClock tempo_;                 ///< output frames → stretcher input frames (the tempo ratio)
    std::vector<float> stretch_input_;  ///< converted stereo awaiting the stretcher
    std::size_t priming_frames_ = 0;
    std::size_t stretch_tail_frames_ = 0;
};

}  // namespace mso::audio
