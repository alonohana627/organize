#pragma once
/// @file
/// Spectral-flux onset envelope, the input of tempo estimation (docs/05_AUDIO_INTELLIGENCE.md §4).
#include <span>
#include <vector>

#include "mso/analysis/spectrogram_stream.hpp"

namespace mso::analysis {

/// Streams mono audio into a positive spectral-flux onset strength curve.
///
/// Instead of resampling to 22,050 Hz the STFT is scaled with the sample rate: the window is the
/// power of two closest to 46 ms (1,024 samples at 22,050 Hz) and the hop is a quarter window
/// (about 11.6 ms), so the envelope has the same time resolution at every source rate. Each frame
/// contributes sum(max(0, log(1 + g*|X_t|) - log(1 + g*|X_t-1|))) over all bins; log compression
/// keeps quiet hi-hats visible next to loud kicks. Memory grows by one float per hop.
class OnsetEnvelopeBuilder {
public:
    explicit OnsetEnvelopeBuilder(int sample_rate);

    /// Appends mono samples; may be called with blocks of any size.
    void add(std::span<const float> mono_samples);

    /// Onset strength per hop, oldest first.
    std::span<const float> envelope() const noexcept { return envelope_; }
    /// Envelope values per second (sample_rate / hop_size).
    double frame_rate() const noexcept { return frame_rate_; }

private:
    SpectrogramStream spectrogram_;
    double frame_rate_;
    std::vector<float> previous_log_magnitudes_;
    std::vector<float> envelope_;
};

}  // namespace mso::analysis
