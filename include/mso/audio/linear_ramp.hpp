#pragma once
/// @file
/// Per-sample gain smoothing used by the real-time renderer (volume, play/pause fades, declick).
#include <algorithm>
#include <cstdint>

namespace mso::audio {

/// Gain that moves toward its target by a fixed step per sample and lands on it exactly, so a
/// smoothed change always finishes in a bounded, predictable number of samples.
class LinearRamp {
public:
    LinearRamp() = default;
    LinearRamp(float initial_value, float step_per_sample) noexcept
        : value_(initial_value), target_(initial_value), step_(step_per_sample) {}

    /// Step that crosses `span` (e.g. full scale 1.0) in `duration_ms` at `sample_rate`.
    static float step_for(std::uint32_t sample_rate, double duration_ms, float span = 1.0f) noexcept {
        const double samples = std::max(1.0, static_cast<double>(sample_rate) * duration_ms / 1000.0);
        return static_cast<float>(static_cast<double>(span) / samples);
    }

    void set_step(float step_per_sample) noexcept { step_ = step_per_sample; }
    void set_target(float target) noexcept { target_ = target; }
    /// Moves to `value` at once and makes it the target (no smoothing).
    void jump_to(float value) noexcept { value_ = target_ = value; }

    /// Advances one sample and returns the new value.
    float next() noexcept {
        if (value_ < target_) {
            value_ = std::min(value_ + step_, target_);
        } else if (value_ > target_) {
            value_ = std::max(value_ - step_, target_);
        }
        return value_;
    }

    float value() const noexcept { return value_; }
    float target() const noexcept { return target_; }
    bool is_settled() const noexcept { return value_ == target_; }

private:
    float value_ = 0.0f;
    float target_ = 0.0f;
    float step_ = 1.0f;
};

}  // namespace mso::audio
