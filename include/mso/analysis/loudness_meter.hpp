#pragma once
/// @file
/// Sample peak and RMS level (algorithm AlgorithmVersions::kLoudness). Not LUFS.
#include <cstdint>
#include <span>

#include "mso/analysis/analysis.hpp"

namespace mso::analysis {

/// Level reported for digital silence instead of -infinity.
inline constexpr double kSilenceDbfs = -200.0;

/// Converts a linear amplitude to dBFS (20 * log10), floored at kSilenceDbfs.
double amplitude_to_dbfs(double amplitude) noexcept;

/// Streaming sample-peak and RMS meter over every sample of every channel.
///
/// RMS is the plain root mean square of all samples, so a full-scale sine reads -3.01 dBFS (no
/// +3 dB "AES" sine convention) and channels are weighted equally.
class LoudnessMeter {
public:
    /// Adds interleaved samples of any channel count.
    void add(std::span<const float> samples) noexcept;

    /// Peak and RMS of everything added so far (silence for no samples).
    LoudnessResult result() const noexcept;

private:
    float peak_magnitude_ = 0.0F;
    double sum_of_squares_ = 0.0;
    std::int64_t sample_count_ = 0;
};

}  // namespace mso::analysis
