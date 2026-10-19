#pragma once
/// @file
/// Drift-free bookkeeping of source positions: output frames map to source frames through an exact
/// integer ratio, and loop playback folds the resulting position back into the loop region.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>

namespace mso::audio {

/// Speed is carried as an integer number of thousandths so that the rate ratio stays rational.
inline constexpr std::int64_t kUnitySpeedPermille = 1000;
inline constexpr std::int64_t kMinimumSpeedPermille = 250;
inline constexpr std::int64_t kMaximumSpeedPermille = 4000;

/// Rounds a speed ratio to thousandths within 0.25..4; returns unity for NaN.
inline std::int64_t speed_permille_from_ratio(double ratio) noexcept {
    if (!std::isfinite(ratio)) {
        return kUnitySpeedPermille;
    }
    const auto permille = static_cast<std::int64_t>(std::llround(ratio * 1000.0));
    return std::clamp(permille, kMinimumSpeedPermille, kMaximumSpeedPermille);
}

/// Maps output frames to source frames within one playback segment (between two restarts):
///     source = anchor + output_frames * (source_rate * speed) / output_rate
/// The ratio is a reduced integer fraction evaluated in integer arithmetic only, so a position
/// reported after hours of playback is as exact as one reported after a second.
class SourceClock {
public:
    SourceClock() = default;

    /// Clock for a segment that starts at `anchor_frame` and plays `source_rate` at `speed_permille`
    /// thousandths of normal speed into a device running at `output_rate`.
    SourceClock(std::int64_t anchor_frame, std::int64_t source_rate, std::int64_t speed_permille,
                std::int64_t output_rate) noexcept
        : anchor_frame_(anchor_frame) {
        const std::int64_t numerator = source_rate * speed_permille;
        const std::int64_t denominator = output_rate * kUnitySpeedPermille;
        const std::int64_t divisor = std::gcd(numerator, denominator);
        numerator_ = numerator / divisor;
        denominator_ = denominator / divisor;
    }

    /// Source frame reached after `output_frames` frames of output since the anchor.
    /// Split into whole periods and a remainder so the products cannot overflow 64 bits.
    std::int64_t source_frame_after(std::int64_t output_frames) const noexcept {
        const std::int64_t whole_periods = output_frames / denominator_;
        const std::int64_t remainder = output_frames % denominator_;
        return anchor_frame_ + whole_periods * numerator_ + remainder * numerator_ / denominator_;
    }

    std::int64_t anchor_frame() const noexcept { return anchor_frame_; }
    /// Reduced source side of the ratio (the resampler's input rate).
    std::int64_t source_units() const noexcept { return numerator_; }
    /// Reduced output side of the ratio (the resampler's output rate).
    std::int64_t output_units() const noexcept { return denominator_; }

private:
    std::int64_t anchor_frame_ = 0;
    std::int64_t numerator_ = 1;
    std::int64_t denominator_ = 1;
};

/// Folds a monotonically increasing ("unfolded") source position into a loop region: once playback
/// passes end_frame it continues at start_frame, so later unfolded positions wrap around.
struct LoopFold {
    std::int64_t start_frame = 0;
    std::int64_t end_frame = 0;  ///< exclusive; the fold is inactive when end_frame <= start_frame

    bool is_active() const noexcept { return end_frame > start_frame; }

    std::int64_t fold(std::int64_t unfolded_frame) const noexcept {
        if (!is_active() || unfolded_frame < end_frame) {
            return unfolded_frame;
        }
        return start_frame + (unfolded_frame - end_frame) % (end_frame - start_frame);
    }
};

}  // namespace mso::audio
