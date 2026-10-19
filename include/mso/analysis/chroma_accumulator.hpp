#pragma once
/// @file
/// Time-averaged, tuning-compensated pitch-class profile (chroma), the input of key estimation.
#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "mso/analysis/spectrogram_stream.hpp"

namespace mso::analysis {

/// Pitch-class energy summed over all tonal, stationary frames of a signal.
struct ChromaProfile {
    std::array<double, 12> pitch_class_energy{};  ///< index 0 = C ... 11 = B, after tuning compensation
    double tonal_frame_weight = 0.0;              ///< sum of per-frame weights (0 = nothing tonal)
    double tuning_offset_semitones = 0.0;         ///< estimated deviation from A440 in [-0.5, 0.5)
};

/// Streams mono audio into a chroma profile.
///
/// Each frame (power of two closest to 186 ms, half-frame hop) contributes its spectral peaks
/// between 80 Hz and 2.5 kHz, located with parabolic interpolation on log magnitude and mapped on
/// a log-frequency axis to a 120-bin (10 per semitone) fine chroma.
///
/// A frame's weight is tonality x stationarity:
/// - tonality = 1 - spectral flatness / 0.35, clamped to [0, 1], drops noise and clicks;
/// - stationarity = the smaller spectral similarity (sum of min / sum of max) with its previous
///   and next frame, drops transients such as a kick's pitch glide or a drum hit, while the
///   steady body of held notes and chords keeps a weight near 1.
/// Because of the one-frame lookahead the newest frame (and the first one) never contributes.
///
/// profile() estimates the tuning offset as the circular mean of the fine-bin positions within a
/// semitone and folds the fine chroma to 12 pitch classes around it, so a recording tuned to
/// A435 or A445 still lands on the right pitch classes.
class ChromaAccumulator {
public:
    explicit ChromaAccumulator(int sample_rate);

    /// Appends mono samples; may be called with blocks of any size.
    void add(std::span<const float> mono_samples);

    /// The profile of every frame accumulated so far.
    ChromaProfile profile() const;

    static constexpr std::size_t kFineBinsPerSemitone = 10;
    static constexpr std::size_t kFineBinCount = 12 * kFineBinsPerSemitone;

private:
    /// Receives each new frame; accumulates the pending (previous) one now that its successor is known.
    void on_frame(std::span<const float> magnitudes);
    void accumulate_pending_frame(double stationarity);

    SpectrogramStream spectrogram_;
    double bin_frequency_hz_;
    std::size_t first_bin_;
    std::size_t last_bin_;                   ///< exclusive
    std::vector<float> pending_magnitudes_;  ///< the frame waiting for its successor
    double pending_similarity_to_previous_ = 0.0;
    bool has_pending_frame_ = false;
    std::array<double, kFineBinCount> fine_chroma_{};
    double tonal_frame_weight_ = 0.0;
};

}  // namespace mso::analysis
