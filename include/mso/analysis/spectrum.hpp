#pragma once
/// @file
/// FFT building blocks shared by the onset (tempo) and chroma (key) analyzers. pocketfft stays an
/// implementation detail of spectrum.cpp so no other translation unit depends on it.
#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace mso::analysis {

/// Hann-windowed magnitude spectrum of fixed-size real frames.
///
/// Magnitudes are scaled by 2 / sum(window), so a full-scale sine centred on a bin reads about 1.0
/// regardless of the frame size. The object owns its scratch buffers and is reused frame after
/// frame without allocating.
class MagnitudeSpectrum {
public:
    /// @param frame_size samples per frame; must be at least 2.
    explicit MagnitudeSpectrum(std::size_t frame_size);

    /// Windows `frame` (exactly frame_size() samples) and returns frame_size()/2 + 1 magnitudes,
    /// valid until the next call.
    std::span<const float> compute(std::span<const float> frame);

    std::size_t frame_size() const noexcept { return window_.size(); }
    std::size_t bin_count() const noexcept { return magnitudes_.size(); }

private:
    std::vector<float> window_;
    std::vector<float> windowed_frame_;
    std::vector<std::complex<float>> spectrum_;
    std::vector<float> magnitudes_;
    float normalization_ = 1.0F;
};

/// Biased autocorrelation sum(x[n] * x[n + lag]) for lags 0 .. signal.size()-1, computed with a
/// zero-padded FFT so it costs O(n log n) instead of O(n^2).
std::vector<double> autocorrelation(std::span<const double> signal);

/// Power of two closest (in log terms) to `target`, clamped to [minimum, maximum].
std::size_t nearest_power_of_two(double target, std::size_t minimum, std::size_t maximum) noexcept;

}  // namespace mso::analysis
