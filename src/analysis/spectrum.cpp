/// @file
/// pocketfft-backed implementation of the analysis FFT helpers.
#include "mso/analysis/spectrum.hpp"

#include <pocketfft_hdronly.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>

namespace mso::analysis {

/// A reusable real-input FFT plan. pocketfft's convenience functions (r2c, c2r) build a fresh plan
/// for every call unless POCKETFFT_CACHE_SIZE is set; holding the plan object directly computes the
/// factorization and twiddle factors once per MagnitudeSpectrum. `pocketfft_r` lives in pocketfft's
/// `detail` namespace but is the documented plan class those functions use themselves.
class MagnitudeSpectrum::Plan {
public:
    explicit Plan(std::size_t length) : transform_(length) {}

    /// In-place forward transform of `buffer` into FFTPACK half-complex order:
    /// r0, r1, i1, r2, i2, ..., and for an even length the Nyquist real part last.
    void forward(std::span<float> buffer) const { transform_.exec(buffer.data(), 1.0F, /*fwd=*/true); }

private:
    pocketfft::detail::pocketfft_r<float> transform_;
};

MagnitudeSpectrum::MagnitudeSpectrum(std::size_t frame_size)
    : plan_(std::make_unique<Plan>(std::max<std::size_t>(frame_size, 2))),
      window_(std::max<std::size_t>(frame_size, 2)),
      transform_buffer_(window_.size()),
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

MagnitudeSpectrum::~MagnitudeSpectrum() = default;
MagnitudeSpectrum::MagnitudeSpectrum(MagnitudeSpectrum&&) noexcept = default;
MagnitudeSpectrum& MagnitudeSpectrum::operator=(MagnitudeSpectrum&&) noexcept = default;

std::span<const float> MagnitudeSpectrum::compute(std::span<const float> frame) {
    return compute(frame, {});
}

std::span<const float> MagnitudeSpectrum::compute(std::span<const float> head, std::span<const float> tail) {
    const std::size_t length = window_.size();
    const std::size_t head_count = std::min(head.size(), length);
    const std::size_t tail_count = std::min(tail.size(), length - head_count);
    for (std::size_t index = 0; index < head_count; ++index)
        transform_buffer_[index] = head[index] * window_[index];
    for (std::size_t index = 0; index < tail_count; ++index)
        transform_buffer_[head_count + index] = tail[index] * window_[head_count + index];
    std::fill(transform_buffer_.begin() + static_cast<std::ptrdiff_t>(head_count + tail_count), transform_buffer_.end(),
              0.0F);

    plan_->forward(transform_buffer_);

    // Unpack the half-complex layout: bin k > 0 is (r_k, i_k) at positions 2k-1 and 2k.
    magnitudes_[0] = std::abs(transform_buffer_[0]) * normalization_;
    // sqrt of the squared sum in double instead of std::hypot: squares of finite floats cannot
    // overflow a double (even for a crafted file with huge float samples), and hypot's
    // overflow-safe scaling costs several times as much on this per-bin hot path. The magnitude can
    // still exceed FLT_MAX by up to sqrt(2), and narrowing such a double to float is undefined, so it
    // is capped first (one min per bin).
    constexpr double kLargestFloat = std::numeric_limits<float>::max();
    for (std::size_t bin = 1; 2 * bin < length; ++bin) {
        const double real = transform_buffer_[2 * bin - 1];
        const double imaginary = transform_buffer_[2 * bin];
        const double magnitude = std::min(std::sqrt(real * real + imaginary * imaginary), kLargestFloat);
        magnitudes_[bin] = static_cast<float>(magnitude) * normalization_;
    }
    if (length % 2 == 0) magnitudes_[length / 2] = std::abs(transform_buffer_[length - 1]) * normalization_;
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
