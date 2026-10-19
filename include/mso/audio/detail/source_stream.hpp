#pragma once
/// @file
/// Decoder-thread pipeline from a media::Decoder to stereo ring blocks at the device rate.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "mso/audio/detail/pcm_ring.hpp"
#include "mso/audio/detail/source_position.hpp"
#include "mso/audio/detail/stereo_converter.hpp"
#include "mso/audio/transport.hpp"
#include "mso/domain/error.hpp"
#include "mso/media/media.hpp"

namespace mso::audio::detail {

class TempoPitchStretcher;

/// Loop, speed, pitch, mode and seam fades applied to a source. Changing any audible one re-anchors
/// playback at the audible position.
struct PlaybackShape {
    std::optional<LoopRegion> loop;                     ///< requested loop region, if any
    std::int64_t speed_permille = kUnitySpeedPermille;  ///< speed in thousandths
    PlaybackMode mode = PlaybackMode::Varispeed;        ///< how speed and pitch interact
    double pitch_semitones = 0.0;                       ///< used by PlaybackMode::IndependentTempoPitch only
    bool seam_fades = false;                            ///< fade out/in around every loop seam

    bool operator==(const PlaybackShape&) const = default;
};

/// Streams one source in bounded chunks: reads from the decoder, scrubs NaN/Inf, wraps loops,
/// folds channels to stereo and resamples into ring blocks. In Varispeed mode the resampler ratio
/// carries the speed; in IndependentTempoPitch mode audio is resampled at 1× and then time-stretched
/// and transposed by a TempoPitchStretcher. Streaming only: never more than one decoded chunk plus
/// the stretcher's look-ahead is held. Used only by the decoder thread.
///
/// Audition fidelity (docs/05_AUDIO_INTELLIGENCE.md §8): the source is never faded where it is
/// intact. A segment that starts at frame 0 keeps its attack untouched; only a start in the middle
/// of the signal (seek, re-anchor) gets a 5 ms fade-in to avoid a step. Loop seams are a hard cut
/// unless seam fades are selected (PlaybackShape::seam_fades), which the snapshot reports.
class SourceStream {
public:
    /// Takes ownership of an opened decoder. Fails with UNSUPPORTED_CODEC for unusable formats.
    [[nodiscard]] static Result<std::unique_ptr<SourceStream>> create(std::unique_ptr<media::Decoder> decoder);

    ~SourceStream();
    SourceStream(const SourceStream&) = delete;
    SourceStream& operator=(const SourceStream&) = delete;

    /// Source frames per second.
    int source_rate() const noexcept { return source_rate_; }
    /// Source channel count.
    int source_channels() const noexcept { return static_cast<int>(channels_); }
    /// Source length as the decoder reports it.
    std::optional<std::int64_t> total_frames() const { return decoder_->total_frames(); }
    /// Whether total_frames() is exact.
    bool total_frames_exact() const { return decoder_->frames_exact(); }

    /// Starts a new segment at `source_frame` (clamped to the source) with `shape`, producing audio
    /// for a device at `output_rate`. Resets the resampler; fades in only when starting mid-signal.
    [[nodiscard]] Result<void> restart(std::int64_t source_frame, const PlaybackShape& shape,
                                       std::uint32_t output_rate);

    /// Frame where the current segment started (the decoder's actual landing position after a seek).
    std::int64_t anchor_frame() const noexcept { return clock_.anchor_frame(); }

    /// Loop region in effect after clamping to the source and lengthening to the minimum, or nullopt.
    /// Its end moves to the real end of the source when a loop reaches past an inexact length (and
    /// its start moves back when that would leave less than the minimum).
    [[nodiscard]] std::optional<LoopRegion> effective_loop() const noexcept;

    /// Fills one ring block: up to kPcmBlockFrames stereo frames into `stereo_samples` and every
    /// header field except the epoch. Marks end_of_stream once the source is exhausted.
    /// A decoder failure (file vanished, corrupt data) is returned unchanged.
    [[nodiscard]] Result<void> render_block(PcmBlockHeader& header, std::span<float> stereo_samples);

private:
    explicit SourceStream(std::unique_ptr<media::Decoder> decoder);

    /// Fills `stereo_output` with converted (folded + resampled) audio; fewer frames at the end.
    [[nodiscard]] Result<std::size_t> convert_into(std::span<float> stereo_output);
    /// Fills `stereo_output` with time-stretched audio; returns the frames that carry sound.
    [[nodiscard]] Result<std::size_t> stretch_into(std::span<float> stereo_output);
    /// Creates (or reuses) the stretcher and primes it so output starts aligned with the anchor.
    [[nodiscard]] Result<void> prime_stretcher(const PlaybackShape& shape, std::uint32_t output_rate);
    /// Records the stretcher input converted so far; once the source has run out, fixes the output
    /// frame at which the stretched sound ends.
    void account_stretch_input(std::size_t converted, std::size_t requested) noexcept;
    /// Normalizes a requested loop: clamped to the source, at least kMinimumLoopMilliseconds long.
    [[nodiscard]] std::optional<LoopRegion> normalized_loop(const std::optional<LoopRegion>& requested) const;
    /// Decodes the next chunk into staged_, wrapping the loop and applying the selected fades.
    [[nodiscard]] Result<void> refill_staging();
    /// Seeks back to the loop start and continues from where the decoder actually lands.
    [[nodiscard]] Result<void> wrap_to_loop_start();
    /// Applies the mid-signal fade-in and, with seam fades, the seam fade-out to staged_.
    void apply_fades() noexcept;
    /// Frames of the staged chunk not yet fed to the converter.
    std::size_t staged_frames_remaining() const noexcept { return staged_frames_ - staged_offset_; }
    /// total_frames() when exact, else nullopt.
    [[nodiscard]] std::optional<std::int64_t> exact_total_frames() const;

    std::unique_ptr<media::Decoder> decoder_;     ///< the source
    std::unique_ptr<StereoConverter> converter_;  ///< fold + resample for the current segment
    std::size_t channels_ = 0;                    ///< source channel count
    int source_rate_ = 0;                         ///< source frames per second
    std::int64_t ramp_frames_ = 1;                ///< fade length in source frames (~5 ms)
    std::int64_t minimum_loop_frames_ = 1;        ///< kMinimumLoopMilliseconds in source frames

    std::vector<float> staged_;      ///< one decoded chunk, interleaved source channels
    std::size_t staged_frames_ = 0;  ///< frames in staged_
    std::size_t staged_offset_ = 0;  ///< frames of the chunk already fed to the converter

    std::int64_t read_position_ = 0;               ///< next source frame the decoder will return
    std::int64_t frames_until_fade_in_ends_ = 0;   ///< source frames left in the current fade-in
    std::int64_t output_frames_since_anchor_ = 0;  ///< output frames produced in this segment
    SourceClock clock_;                            ///< output → source frames of this segment
    LoopFold loop_;                                ///< loop applied to this segment (inactive: none)
    std::optional<LoopRegion> configured_loop_;    ///< normalized loop of the shape (even if not applied)
    bool seam_fades_ = false;                      ///< fade around loop seams
    bool input_exhausted_ = false;                 ///< the decoder returned its last frame
    PlaybackMode mode_ = PlaybackMode::Varispeed;  ///< mode of this segment

    // Independent tempo/pitch mode.
    std::unique_ptr<TempoPitchStretcher> stretcher_;  ///< reused while the output rate stays
    SourceClock tempo_;                               ///< output frames → stretcher input frames (the tempo ratio)
    std::vector<float> stretch_input_;                ///< converted stereo awaiting the stretcher
    std::size_t priming_frames_ = 0;                  ///< input consumed by priming (the stretcher's look-ahead)
    std::int64_t stretch_input_frames_ = 0;           ///< real (not padded) input fed since the anchor
    std::optional<std::int64_t> stretch_output_end_;  ///< output frame where the sound ends, once known
};

}  // namespace mso::audio::detail
