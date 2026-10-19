#pragma once
/// @file
/// Per-sample gain smoothing used by the real-time renderer (volume, play/pause fades).
#include <algorithm>
#include <cstdint>

namespace mso::audio::detail {

/// Gain that moves toward its target by a fixed step per sample and lands on it exactly, so a
/// smoothed change always finishes in a bounded, predictable number of samples.
class LinearRamp {
public:
    LinearRamp() = default;
    /// Starts settled at `initial_value`, moving `step_per_sample` per next() once retargeted.
    LinearRamp(float initial_value, float step_per_sample) noexcept
        : value_(initial_value), target_(initial_value), step_(step_per_sample) {}

    /// Step that crosses `span` (e.g. full scale 1.0) in `duration_ms` at `sample_rate`.
    static float step_for(std::uint32_t sample_rate, double duration_ms, float span = 1.0f) noexcept {
        const double samples = std::max(1.0, static_cast<double>(sample_rate) * duration_ms / 1000.0);
        return static_cast<float>(static_cast<double>(span) / samples);
    }

    /// Changes the per-sample step (for a new sample rate).
    void set_step(float step_per_sample) noexcept { step_ = step_per_sample; }
    /// Where next() moves toward.
    void set_target(float target) noexcept { target_ = target; }
    /// Moves to `value` at once and makes it the target (no smoothing).
    void jump_to(float value) noexcept { value_ = target_ = value; }
    /// Moves the current value to `value` at once, keeping the target (a ramp restarts from there).
    void restart_from(float value) noexcept { value_ = value; }

    /// Advances one sample and returns the new value.
    float next() noexcept {
        if (value_ < target_) {
            value_ = std::min(value_ + step_, target_);
        } else if (value_ > target_) {
            value_ = std::max(value_ - step_, target_);
        }
        return value_;
    }

    /// Current value.
    float value() const noexcept { return value_; }
    /// Value the ramp is heading to.
    float target() const noexcept { return target_; }
    /// True once the value has reached the target.
    bool is_settled() const noexcept { return value_ == target_; }

private:
    float value_ = 0.0f;   ///< current gain
    float target_ = 0.0f;  ///< gain being approached
    float step_ = 1.0f;    ///< largest change per sample
};

}  // namespace mso::audio::detail
