#include "mso/audio/transport.hpp"

#include <algorithm>
#include <cmath>

namespace mso::audio {

std::string_view to_string(PreviewState state) noexcept {
    switch (state) {
        case PreviewState::Empty:
            return "empty";
        case PreviewState::Loading:
            return "loading";
        case PreviewState::Ready:
            return "ready";
        case PreviewState::Playing:
            return "playing";
        case PreviewState::Paused:
            return "paused";
        case PreviewState::Stopped:
            return "stopped";
        case PreviewState::Error:
            return "error";
    }
    return "unknown";
}

std::string_view to_string(PlaybackMode mode) noexcept {
    switch (mode) {
        case PlaybackMode::Varispeed:
            return "varispeed";
        case PlaybackMode::IndependentTempoPitch:
            return "independent_tempo_pitch";
    }
    return "unknown";
}

float linear_gain_from_decibels(double decibels) noexcept {
    constexpr double kSilenceFloorDecibels = -120.0;
    if (!(decibels > kSilenceFloorDecibels)) {
        return 0.0f;  // also catches NaN and -inf
    }
    return static_cast<float>(std::pow(10.0, std::min(decibels, 0.0) / 20.0));
}

float clamped_linear_gain(double gain) noexcept {
    if (!(gain > 0.0)) {
        return 0.0f;  // also catches NaN
    }
    return static_cast<float>(std::min(gain, 1.0));
}

double clamped_pitch_semitones(double semitones) noexcept {
    if (!std::isfinite(semitones)) {
        return 0.0;
    }
    return std::clamp(semitones, -kMaximumPitchSemitones, kMaximumPitchSemitones);
}

}  // namespace mso::audio
