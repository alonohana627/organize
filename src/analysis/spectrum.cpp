/// @file
/// pocketfft-backed implementation of the analysis FFT helpers.
#include "mso/analysis/spectrum.hpp"

#include <pocketfft_hdronly.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mso::analysis {

MagnitudeSpectrum::MagnitudeSpectrum(std::size_t frame_size)
    : window_(std::max<std::size_t>(frame_size, 2)),
      windowed_frame_(window_.size()),
      spectrum_(window_.size() / 2 + 1),
      magnitudes_(window_.size() / 2 + 1) {
    // Periodic Hann window: the standard choice for overlapped STFT analysis.
    const double length = static_cast<double>(window_.size());
    double window_sum = 0.0;
    for (std::size_t index = 0; index < window_.size(); ++index) {
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(index) / length;
        window_[index] = static_cast<float>(0.5 - 0.5 * std::cos(phase));
        window_sum += window_[index];
    }
    normalization_ = static_cast<float>(2.0 / window_sum);
}

std::span<const float> MagnitudeSpectrum::compute(std::span<const float> frame) {
    const std::size_t count = std::min(frame.size(), window_.size());
    for (std::size_t index = 0; index < count; ++index)
        windowed_frame_[index] = frame[index] * window_[index];
    std::fill(windowed_frame_.begin() + static_cast<std::ptrdiff_t>(count), windowed_frame_.end(), 0.0F);

    const pocketfft::shape_t shape{windowed_frame_.size()};
    const pocketfft::stride_t input_stride{sizeof(float)};
    const pocketfft::stride_t output_stride{sizeof(std::complex<float>)};
    pocketfft::r2c(shape, input_stride, output_stride, 0, pocketfft::FORWARD, windowed_frame_.data(), spectrum_.data(),
                   1.0F);

    for (std::size_t bin = 0; bin < spectrum_.size(); ++bin)
        magnitudes_[bin] = std::abs(spectrum_[bin]) * normalization_;
    return magnitudes_;
}

std::vector<double> autocorrelation(std::span<const double> signal) {
    if (signal.empty()) return {};
    // Padding to at least twice the length turns the FFT's circular correlation into a linear one.
    const std::size_t padded_size = 2 * signal.size();
    std::vector<double> padded(padded_size, 0.0);
    std::copy(signal.begin(), signal.end(), padded.begin());

    std::vector<std::complex<double>> spectrum(padded_size / 2 + 1);
    const pocketfft::shape_t shape{padded_size};
    const pocketfft::stride_t real_stride{sizeof(double)};
    const pocketfft::stride_t complex_stride{sizeof(std::complex<double>)};
    pocketfft::r2c(shape, real_stride, complex_stride, 0, pocketfft::FORWARD, padded.data(), spectrum.data(), 1.0);
    for (auto& value : spectrum)
        value = std::complex<double>(std::norm(value), 0.0);
    pocketfft::c2r(shape, complex_stride, real_stride, 0, pocketfft::BACKWARD, spectrum.data(), padded.data(),
                   1.0 / static_cast<double>(padded_size));

    padded.resize(signal.size());
    return padded;
}

std::size_t nearest_power_of_two(double target, std::size_t minimum, std::size_t maximum) noexcept {
    if (!(target > 1.0)) return minimum;
    const double exponent = std::round(std::log2(target));
    const double candidate = std::exp2(std::min(exponent, 62.0));
    return std::clamp(static_cast<std::size_t>(candidate), minimum, maximum);
}

}  // namespace mso::analysis
