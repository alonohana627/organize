/// @file
/// Sample peak and RMS metering.
#include "mso/analysis/loudness_meter.hpp"

#include <algorithm>
#include <cmath>

namespace mso::analysis {

double amplitude_to_dbfs(double amplitude) noexcept {
    if (!(amplitude > 0.0)) return kSilenceDbfs;
    return std::max(kSilenceDbfs, 20.0 * std::log10(amplitude));
}

void LoudnessMeter::add(std::span<const float> samples) noexcept {
    for (const float sample : samples) {
        peak_magnitude_ = std::max(peak_magnitude_, std::abs(sample));
        sum_of_squares_ += static_cast<double>(sample) * sample;
    }
    sample_count_ += static_cast<std::int64_t>(samples.size());
}

LoudnessResult LoudnessMeter::result() const noexcept {
    LoudnessResult result;
    result.peak_dbfs = amplitude_to_dbfs(peak_magnitude_);
    const double mean_square = sample_count_ > 0 ? sum_of_squares_ / static_cast<double>(sample_count_) : 0.0;
    result.rms_dbfs = amplitude_to_dbfs(std::sqrt(mean_square));
    return result;
}

}  // namespace mso::analysis
