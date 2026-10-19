#pragma once
/// @file
/// Independent tempo and pitch for stereo audio, delegated to signalsmith-stretch (phase vocoder).
/// Internal to mso_audio; used on the decoder thread only, never on the audio callback.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace mso::audio::detail {

/// Stretches interleaved stereo float frames at one sample rate: the tempo is the ratio of input to
/// output frames handed to process(), the pitch is a transposition in semitones. Output lines up with
/// input after prime(), so source positions keep their exact rational mapping. Allocates only when
/// constructed; prime() and process() reuse the preallocated buffers.
class TempoPitchStretcher {
public:
    /// Configures the default signalsmith preset (120 ms blocks, 30 ms hop) for `sample_rate`.
    /// Throws std::bad_alloc only.
    explicit TempoPitchStretcher(std::uint32_t sample_rate);
    ~TempoPitchStretcher();
    TempoPitchStretcher(const TempoPitchStretcher&) = delete;
    TempoPitchStretcher& operator=(const TempoPitchStretcher&) = delete;

    /// Rate the stretcher was configured for.
    std::uint32_t sample_rate() const noexcept { return sample_rate_; }

    /// Input frames prime() needs so that output starts aligned at `tempo` (input per output frame).
    std::size_t priming_frames(double tempo) const;

    /// Starts afresh at `pitch_semitones`, consuming priming_frames() frames of `stereo_input`.
    /// The next process() output begins exactly at the first primed frame.
    void prime(std::span<const float> stereo_input, double pitch_semitones);

    /// Turns all of `stereo_input` into all of `stereo_output`; their length ratio is the tempo.
    void process(std::span<const float> stereo_input, std::span<float> stereo_output);

private:
    struct Engine;
    std::unique_ptr<Engine> engine_;  ///< the signalsmith stretcher (kept out of this header)
    std::uint32_t sample_rate_ = 0;   ///< configured rate
};

}  // namespace mso::audio::detail
