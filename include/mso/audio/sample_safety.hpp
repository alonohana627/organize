#pragma once
/// @file
/// Sample sanitizing shared by the decoder thread (scrub decoder output) and the callback (output clamp).
#include <algorithm>
#include <cmath>
#include <span>

namespace mso::audio {

/// Replaces NaN and infinity with silence; finite samples pass unchanged.
inline float finite_or_silence(float sample) noexcept {
    return std::isfinite(sample) ? sample : 0.0f;
}

/// Hard output safety: non-finite samples become silence and everything else is clamped to ±1.
inline float clamp_for_output(float sample) noexcept {
    return std::clamp(finite_or_silence(sample), -1.0f, 1.0f);
}

/// Scrubs a decoded block in place so NaN/Inf can never reach the resampler state or the device.
inline void replace_non_finite_with_silence(std::span<float> samples) noexcept {
    for (float& sample : samples) {
        sample = finite_or_silence(sample);
    }
}

}  // namespace mso::audio
