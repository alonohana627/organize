/// @file
/// Krumhansl-Schmuckler key finding with ambiguity rejection.
#include "mso/analysis/key_estimator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <numeric>

namespace mso::analysis {
namespace {

/// Krumhansl & Kessler (1982) probe-tone ratings, tonic first.
constexpr std::array<double, 12> kMajorProfile{6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr std::array<double, 12> kMinorProfile{6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

constexpr double kMinimumTonalFrameWeight = 3.0;  ///< about 0.3 s of clearly tonal frames
constexpr double kMinimumChromaVariation = 0.3;   ///< std/mean of the 12 bins; flat noise is near 0
/// A pitch class is active above this fraction of the strongest one. A single harmonic note
/// activates only its root and fifth (its major-third overtone stays far below), so requiring
/// three active classes keeps one note from posing as a major or minor key.
constexpr double kActivePitchClassFraction = 0.2;
constexpr int kMinimumActivePitchClasses = 3;
constexpr double kMinimumCorrelation = 0.5;
/// Best correlation minus the best one of an unrelated key (neither the relative nor the parallel
/// key of the winner). Relative keys share every pitch class and parallel keys their tonic, so on
/// real music they routinely score within a few hundredths of the winner; rejecting those near
/// ties would make most results "unknown". They lower the confidence instead.
constexpr double kMinimumMargin = 0.03;
/// Margin over the runner-up (any key) at which confidence is no longer reduced.
constexpr double kMarginForFullConfidence = 0.15;

/// Pearson correlation between the chroma and a key profile rotated so its tonic is `tonic`.
double key_correlation(const std::array<double, 12>& chroma, const std::array<double, 12>& profile, int tonic) {
    const double chroma_mean = std::accumulate(chroma.begin(), chroma.end(), 0.0) / 12.0;
    const double profile_mean = std::accumulate(profile.begin(), profile.end(), 0.0) / 12.0;
    double covariance = 0.0;
    double chroma_variance = 0.0;
    double profile_variance = 0.0;
    for (int pitch_class = 0; pitch_class < 12; ++pitch_class) {
        const double chroma_deviation = chroma[static_cast<std::size_t>(pitch_class)] - chroma_mean;
        const double profile_deviation =
            profile[static_cast<std::size_t>((pitch_class - tonic + 12) % 12)] - profile_mean;
        covariance += chroma_deviation * profile_deviation;
        chroma_variance += chroma_deviation * chroma_deviation;
        profile_variance += profile_deviation * profile_deviation;
    }
    const double denominator = std::sqrt(chroma_variance * profile_variance);
    return denominator > 0.0 ? covariance / denominator : 0.0;
}

/// Coefficient of variation of the chroma bins: 0 for a perfectly flat chroma.
double chroma_variation(const std::array<double, 12>& chroma) {
    const double mean = std::accumulate(chroma.begin(), chroma.end(), 0.0) / 12.0;
    if (!(mean > 0.0)) return 0.0;
    double variance = 0.0;
    for (const double value : chroma)
        variance += (value - mean) * (value - mean);
    return std::sqrt(variance / 12.0) / mean;
}

/// Number of pitch classes holding at least kActivePitchClassFraction of the strongest one.
int active_pitch_classes(const std::array<double, 12>& chroma) {
    const double strongest = *std::max_element(chroma.begin(), chroma.end());
    return static_cast<int>(std::count_if(chroma.begin(), chroma.end(), [strongest](double energy) {
        return energy >= kActivePitchClassFraction * strongest;
    }));
}

/// Whether `candidate` is the relative key (same pitch set, other mode) or the parallel key (same
/// tonic, other mode) of `key`.
bool is_relative_or_parallel(const MusicalKey& key, const MusicalKey& candidate) {
    if (candidate.mode == key.mode) return false;
    if (candidate.pitch_class == key.pitch_class) return true;
    // C major's relative minor is A minor (+9 semitones); A minor's relative major is C major (+3).
    const int relative_offset = key.mode == KeyMode::Major ? 9 : 3;
    return candidate.pitch_class == (key.pitch_class + relative_offset) % 12;
}

/// A key and how well the chroma correlates with its profile.
struct KeyScore {
    MusicalKey key;
    double correlation = 0.0;
};

}  // namespace

KeyResult estimate_key(const ChromaProfile& profile) {
    const KeyResult unknown;
    if (profile.tonal_frame_weight < kMinimumTonalFrameWeight) return unknown;
    const auto& chroma = profile.pitch_class_energy;
    if (chroma_variation(chroma) < kMinimumChromaVariation) return unknown;
    if (active_pitch_classes(chroma) < kMinimumActivePitchClasses) return unknown;

    std::array<KeyScore, 24> scores{};
    std::size_t index = 0;
    for (const KeyMode mode : {KeyMode::Major, KeyMode::Minor}) {
        const auto& key_profile = mode == KeyMode::Major ? kMajorProfile : kMinorProfile;
        for (int tonic = 0; tonic < 12; ++tonic)
            scores[index++] = KeyScore{MusicalKey{tonic, mode}, key_correlation(chroma, key_profile, tonic)};
    }
    // Stable sort, so equal correlations keep the fixed major-then-minor, C-to-B order.
    std::ranges::stable_sort(scores, std::ranges::greater{}, &KeyScore::correlation);
    const KeyScore& best = scores[0];
    const double runner_up = scores[1].correlation;
    const auto unrelated = std::find_if(scores.begin() + 1, scores.end(), [&best](const KeyScore& score) {
        return !is_relative_or_parallel(best.key, score.key);
    });
    const double best_unrelated = unrelated != scores.end() ? unrelated->correlation : -1.0;

    if (best.correlation < kMinimumCorrelation || best.correlation - best_unrelated < kMinimumMargin) return unknown;

    KeyResult result;
    result.key = best.key;
    result.confidence = std::clamp(best.correlation, 0.0, 1.0) *
                        std::clamp((best.correlation - runner_up) / kMarginForFullConfidence, 0.0, 1.0);
    return result;
}

}  // namespace mso::analysis
