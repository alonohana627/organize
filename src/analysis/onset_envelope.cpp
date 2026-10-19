/// @file
/// Spectral-flux onset envelope implementation.
#include "mso/analysis/onset_envelope.hpp"

#include <algorithm>
#include <cmath>

namespace mso::analysis {
namespace {

constexpr double kWindowSeconds = 1024.0 / 22050.0;  ///< about 46 ms, the documented 1,024 @ 22.05 kHz
constexpr std::size_t kHopDivisor = 4;               ///< hop = window / 4 (256 @ 22.05 kHz)
constexpr float kLogCompressionGain = 100.0F;        ///< g in log(1 + g*|X|)

/// Onset window length at `sample_rate`: the power of two closest to kWindowSeconds.
std::size_t onset_window_size(int sample_rate) {
    return nearest_power_of_two(kWindowSeconds * static_cast<double>(sample_rate), 64, 16384);
}

}  // namespace

OnsetEnvelopeBuilder::OnsetEnvelopeBuilder(int sample_rate)
    : spectrogram_(onset_window_size(sample_rate), onset_window_size(sample_rate) / kHopDivisor),
      frame_rate_(static_cast<double>(std::max(sample_rate, 1)) / static_cast<double>(spectrogram_.hop_size())),
      previous_log_magnitudes_(spectrogram_.bin_count(), 0.0F) {}

void OnsetEnvelopeBuilder::add(std::span<const float> mono_samples) {
    spectrogram_.add(mono_samples, [this](std::span<const float> magnitudes) {
        double positive_flux = 0.0;
        for (std::size_t bin = 0; bin < magnitudes.size(); ++bin) {
            const float log_magnitude = std::log1p(kLogCompressionGain * magnitudes[bin]);
            positive_flux += std::max(0.0F, log_magnitude - previous_log_magnitudes_[bin]);
            previous_log_magnitudes_[bin] = log_magnitude;
        }
        envelope_.push_back(static_cast<float>(positive_flux));
    });
}

}  // namespace mso::analysis
