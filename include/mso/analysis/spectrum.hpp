#pragma once
/// @file
/// FFT building blocks shared by the onset (tempo) and chroma (key) analyzers. pocketfft stays an
/// implementation detail of spectrum.cpp so no other translation unit depends on it.
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace mso::analysis {

/// Hann-windowed magnitude spectrum of fixed-size real frames.
///
/// Magnitudes are scaled by 2 / sum(window), so a full-scale sine centred on a bin reads about 1.0
/// regardless of the frame size. The FFT plan (factorization and twiddle factors) and every
/// buffer are built once in the constructor and reused frame after frame; the only per-frame
/// allocation left is the one scratch array pocketfft's real transform allocates internally.
/// Removing it would need a recorded patch to vendored pocketfft; one small aligned allocation per
/// frame costs far less than the transform it serves, so it is kept.
class MagnitudeSpectrum {
public:
    /// @param frame_size samples per frame; values below 2 are raised to 2.
    explicit MagnitudeSpectrum(std::size_t frame_size);
    ~MagnitudeSpectrum();
    MagnitudeSpectrum(MagnitudeSpectrum&&) noexcept;
    MagnitudeSpectrum& operator=(MagnitudeSpectrum&&) noexcept;
    MagnitudeSpectrum(const MagnitudeSpectrum&) = delete;
    MagnitudeSpectrum& operator=(const MagnitudeSpectrum&) = delete;

    /// Windows `frame` and returns frame_size()/2 + 1 magnitudes, valid until the next call. A
    /// shorter frame is zero-padded; samples beyond frame_size() are ignored.
    std::span<const float> compute(std::span<const float> frame);

    /// Same as compute(frame) for a frame stored in two pieces, `head` followed by `tail` (the
    /// two halves of a wrapped ring buffer), without first copying them together.
    std::span<const float> compute(std::span<const float> head, std::span<const float> tail);

    /// Samples per frame.
    std::size_t frame_size() const noexcept { return window_.size(); }
    /// Magnitudes per frame: frame_size() / 2 + 1.
    std::size_t bin_count() const noexcept { return magnitudes_.size(); }

private:
    class Plan;  ///< the pocketfft real-transform plan, kept out of this header

    std::unique_ptr<Plan> plan_;           ///< built once per frame size
    std::vector<float> window_;            ///< periodic Hann window, frame_size() samples
    std::vector<float> transform_buffer_;  ///< windowed input, then pocketfft's packed half-complex output
    std::vector<float> magnitudes_;        ///< what compute() returns: bin_count() values
    float normalization_ = 1.0F;           ///< 2 / sum(window), so a full-scale sine reads about 1.0
};

/// Biased autocorrelation sum(x[n] * x[n + lag]) for lags 0 .. signal.size()-1, computed with a
/// zero-padded FFT so it costs O(n log n) instead of O(n^2). Called once per tempo estimate, so
/// it builds its plan per call.
[[nodiscard]] std::vector<double> autocorrelation(std::span<const double> signal);

/// Power of two closest (in log terms) to `target`, clamped to [minimum, maximum]. Returns
/// `minimum` for a target that is not above 1 (including NaN).
[[nodiscard]] std::size_t nearest_power_of_two(double target, std::size_t minimum, std::size_t maximum) noexcept;

}  // namespace mso::analysis
