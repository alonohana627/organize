/// @file
/// Autocorrelation / beat-contrast comb tempo estimator.
#include "mso/analysis/tempo_estimator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "mso/analysis/spectrum.hpp"

namespace mso::analysis {
namespace {

constexpr double kLocalMeanSeconds = 0.4;          ///< moving-average window removed from the envelope
constexpr double kMaximumLagFraction = 2.0 / 3.0;  ///< longer lags have too few overlapping frames
constexpr double kCoarseStepBpm = 0.25;
constexpr int kCoarseHarmonics = 4;           ///< beat lags averaged by the coarse beat comb
constexpr int kMinimumHarmonics = 2;          ///< a tempo needs at least two beat lags, i.e. three beats
constexpr double kRefineSpanFraction = 0.03;  ///< fine search within +-3% of the coarse winner
constexpr double kRefineStepBpm = 0.005;
constexpr int kRefineHarmonics = 32;
constexpr double kPriorCenterBpm = 120.0;
constexpr double kPriorWidthOctaves = 1.5;  ///< weak: only breaks near-ties between octaves
constexpr double kOctaveFactors[] = {2.0, 3.0};
constexpr double kOctaveAcceptance = 0.85;      ///< the faster tempo wins at >= 85% of the slower comb
constexpr double kOctaveSearchFraction = 0.02;  ///< faster tempo searched within +-2% of factor x bpm
constexpr double kMinimumPeriodicity = 0.25;    ///< beat-comb score below which tempo is unknown
constexpr double kTransientPeakFraction = 0.2;  ///< transient = local maximum above 20% of the largest
constexpr double kTransientMinimumSpacingSeconds = 0.1;
constexpr std::size_t kMinimumTransients = 3;

/// Envelope minus its moving average, half-wave rectified: keeps the transients and drops slow
/// loudness changes so sustained material does not masquerade as periodicity.
std::vector<double> rectified_novelty(std::span<const float> envelope, double frame_rate) {
    const std::size_t count = envelope.size();
    std::vector<double> prefix_sums(count + 1, 0.0);
    for (std::size_t index = 0; index < count; ++index)
        prefix_sums[index + 1] = prefix_sums[index] + envelope[index];

    const auto half_window = static_cast<std::size_t>(std::max(1.0, std::round(kLocalMeanSeconds * frame_rate / 2.0)));
    std::vector<double> novelty(count, 0.0);
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t first = index > half_window ? index - half_window : 0;
        const std::size_t last = std::min(count, index + half_window + 1);
        const double local_mean = (prefix_sums[last] - prefix_sums[first]) / static_cast<double>(last - first);
        novelty[index] = std::max(0.0, static_cast<double>(envelope[index]) - local_mean);
    }
    return novelty;
}

/// Number of distinct transients: local maxima above a fraction of the strongest one and at least
/// kTransientMinimumSpacingSeconds apart.
std::size_t count_transients(const std::vector<double>& novelty, double frame_rate) {
    const double strongest = novelty.empty() ? 0.0 : *std::max_element(novelty.begin(), novelty.end());
    if (!(strongest > 0.0)) return 0;
    const double threshold = kTransientPeakFraction * strongest;
    const auto minimum_spacing = static_cast<std::size_t>(kTransientMinimumSpacingSeconds * frame_rate);

    std::size_t transients = 0;
    std::optional<std::size_t> previous_transient;
    for (std::size_t index = 1; index + 1 < novelty.size(); ++index) {
        const bool is_peak =
            novelty[index] >= threshold && novelty[index] > novelty[index - 1] && novelty[index] >= novelty[index + 1];
        if (!is_peak) continue;
        if (previous_transient && index - *previous_transient < minimum_spacing) continue;
        ++transients;
        previous_transient = index;
    }
    return transients;
}

/// Normalized unbiased autocorrelation r(lag) of a mean-removed signal with r(0) = 1, readable at
/// fractional lags by linear interpolation up to max_lag().
class LagCorrelation {
public:
    /// Builds the correlation; valid() is false for a signal without energy.
    explicit LagCorrelation(std::vector<double> signal) {
        const std::size_t count = signal.size();
        if (count < 4) return;
        double mean = 0.0;
        for (const double value : signal)
            mean += value;
        mean /= static_cast<double>(count);
        for (double& value : signal)
            value -= mean;

        const std::vector<double> sums = autocorrelation(signal);
        if (!(sums[0] > 1e-12)) return;
        max_lag_ = std::floor(kMaximumLagFraction * static_cast<double>(count));
        correlation_.resize(std::min(static_cast<std::size_t>(max_lag_) + 2, count));
        const double energy = sums[0] / static_cast<double>(count);
        for (std::size_t lag = 0; lag < correlation_.size(); ++lag)
            correlation_[lag] = sums[lag] / static_cast<double>(count - lag) / energy;
    }

    bool valid() const noexcept { return !correlation_.empty(); }
    double max_lag() const noexcept { return max_lag_; }

    /// r at a fractional lag; 0 outside [0, max_lag()].
    double at(double lag) const noexcept {
        if (!(lag >= 0.0) || lag > max_lag_) return 0.0;
        const auto lower = static_cast<std::size_t>(lag);
        const double fraction = lag - static_cast<double>(lower);
        const double upper_value = lower + 1 < correlation_.size() ? correlation_[lower + 1] : 0.0;
        return correlation_[lower] * (1.0 - fraction) + upper_value * fraction;
    }

private:
    std::vector<double> correlation_;
    double max_lag_ = 0.0;
};

double beat_lag_frames(double bpm, double frame_rate) {
    return 60.0 * frame_rate / bpm;
}

/// Mean of r over the first kCoarseHarmonics beat multiples: high when transients recur every
/// beat. nullopt when fewer than kMinimumHarmonics beat lags fit in the reliable lag range.
std::optional<double> beat_comb(const LagCorrelation& correlation, double bpm, double frame_rate) {
    const double beat_lag = beat_lag_frames(bpm, frame_rate);
    const int harmonics = std::min(kCoarseHarmonics, static_cast<int>(correlation.max_lag() / beat_lag));
    if (harmonics < kMinimumHarmonics) return std::nullopt;
    double sum = 0.0;
    for (int multiple = 1; multiple <= harmonics; ++multiple)
        sum += correlation.at(multiple * beat_lag);
    return sum / harmonics;
}

/// Weak log-normal preference for moderate tempi; 1 at kPriorCenterBpm.
double tempo_prior(double bpm) {
    const double octaves = std::log2(bpm / kPriorCenterBpm) / kPriorWidthOctaves;
    return std::exp(-0.5 * octaves * octaves);
}

/// A tempo candidate and its beat-comb score.
struct TempoCandidate {
    double bpm = 0.0;
    double comb = 0.0;
};

/// Best beat comb, optionally weighted by the prior, on the coarse grid within [lowest, highest].
std::optional<TempoCandidate> best_candidate(const LagCorrelation& correlation, double frame_rate, double lowest,
                                             double highest, bool weight_by_prior) {
    std::optional<TempoCandidate> best;
    double best_score = 0.0;
    const auto steps = static_cast<int>(std::floor((highest - lowest) / kCoarseStepBpm));
    for (int step = 0; step <= steps; ++step) {
        const double bpm = lowest + step * kCoarseStepBpm;
        const std::optional<double> comb = beat_comb(correlation, bpm, frame_rate);
        if (!comb || *comb <= 0.0) continue;
        const double score = weight_by_prior ? *comb * tempo_prior(bpm) : *comb;
        if (score > best_score) {
            best_score = score;
            best = TempoCandidate{bpm, *comb};
        }
    }
    return best;
}

/// Octave handling. A slow tempo's comb also hits every multiple of the true beat, so while two or
/// three times the current tempo (still in range) explains the correlation nearly as well, the
/// faster tempo is taken as the beat and the slower one as a grouping of beats.
TempoCandidate climb_octaves(const LagCorrelation& correlation, double frame_rate, TempoCandidate candidate) {
    bool climbed = true;
    while (climbed) {
        climbed = false;
        for (const double factor : kOctaveFactors) {
            const double target = candidate.bpm * factor;
            const double lowest = target * (1.0 - kOctaveSearchFraction);
            if (lowest > kMaximumReportedBpm) continue;
            const double highest = std::min(kMaximumReportedBpm, target * (1.0 + kOctaveSearchFraction));
            const std::optional<TempoCandidate> faster =
                best_candidate(correlation, frame_rate, lowest, highest, false);
            if (faster && faster->comb >= kOctaveAcceptance * candidate.comb) {
                candidate = *faster;
                climbed = true;
                break;
            }
        }
    }
    return candidate;
}

/// Sum of r over the first `harmonics` beat multiples. Sharpens the period estimate because the
/// k-th multiple moves k times faster than the beat lag itself.
double comb_sum(const LagCorrelation& correlation, double beat_lag, int harmonics) {
    double sum = 0.0;
    for (int multiple = 1; multiple <= harmonics; ++multiple)
        sum += correlation.at(multiple * beat_lag);
    return sum;
}

/// Fine tempo search around the coarse winner, staying in the same octave.
double refine_bpm(const LagCorrelation& correlation, double coarse_bpm, double frame_rate) {
    const double lowest = std::max(kMinimumReportedBpm, coarse_bpm * (1.0 - kRefineSpanFraction));
    const double highest = std::min(kMaximumReportedBpm, coarse_bpm * (1.0 + kRefineSpanFraction));
    // The same multiple count for every candidate, so no candidate wins merely by a longer comb.
    const int harmonics =
        std::clamp(static_cast<int>(correlation.max_lag() / beat_lag_frames(lowest, frame_rate)), 1, kRefineHarmonics);

    double best_bpm = coarse_bpm;
    double best_sum = comb_sum(correlation, beat_lag_frames(coarse_bpm, frame_rate), harmonics);
    const auto steps = static_cast<int>((highest - lowest) / kRefineStepBpm);
    for (int step = 0; step <= steps; ++step) {
        const double bpm = lowest + step * kRefineStepBpm;
        const double sum = comb_sum(correlation, beat_lag_frames(bpm, frame_rate), harmonics);
        if (sum > best_sum) {
            best_sum = sum;
            best_bpm = bpm;
        }
    }
    return best_bpm;
}

}  // namespace

TempoResult estimate_tempo(std::span<const float> onset_envelope, double frame_rate) {
    const TempoResult unknown;
    if (!(frame_rate > 0.0) || onset_envelope.empty()) return unknown;

    std::vector<double> novelty = rectified_novelty(onset_envelope, frame_rate);
    if (count_transients(novelty, frame_rate) < kMinimumTransients) return unknown;

    const LagCorrelation correlation(std::move(novelty));
    if (!correlation.valid()) return unknown;

    const std::optional<TempoCandidate> coarse =
        best_candidate(correlation, frame_rate, kMinimumReportedBpm, kMaximumReportedBpm, true);
    if (!coarse) return unknown;

    const TempoCandidate beat = climb_octaves(correlation, frame_rate, *coarse);
    const double bpm = refine_bpm(correlation, beat.bpm, frame_rate);
    const double periodicity = beat_comb(correlation, bpm, frame_rate).value_or(0.0);
    if (periodicity < kMinimumPeriodicity) return unknown;

    TempoResult result;
    result.milli_bpm = std::llround(bpm * 1000.0);
    result.confidence = std::clamp(periodicity, 0.0, 1.0);
    return result;
}

}  // namespace mso::analysis
