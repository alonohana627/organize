/// @file
/// Autocorrelation beat-comb tempo estimator with accent-based metrical-level selection.
#include "mso/analysis/tempo_estimator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "mso/analysis/spectrum.hpp"

namespace mso::analysis {
namespace {

constexpr double kLocalMeanSeconds = 0.4;          ///< moving-average window removed from the envelope
constexpr double kMaximumLagFraction = 2.0 / 3.0;  ///< longer lags have too few overlapping frames
constexpr double kCoarseStepBpm = 0.25;            ///< tempo grid of the coarse beat-comb search
constexpr int kCoarseHarmonics = 4;                ///< beat lags averaged by the coarse beat comb
constexpr int kMinimumHarmonics = 2;               ///< a tempo needs at least two beat lags, i.e. three beats
constexpr double kRefineSpanFraction = 0.03;       ///< fine search within +-3% of the coarse winner
constexpr double kRefineStepBpm = 0.005;           ///< tempo grid of the fine search
constexpr int kRefineHarmonics = 32;               ///< beat lags averaged by the fine beat comb
constexpr double kPriorCenterBpm = 120.0;          ///< centre of the log-normal tempo prior
constexpr double kPriorWidthOctaves = 1.5;         ///< weak: only breaks near-ties between metrical levels
/// Comb peaks scoring at least this fraction of the best prior-weighted comb are candidate beat
/// levels. A dense groove (16th-note hats) makes every multiple of its tatum score almost equally,
/// including musically implausible ones such as 4/3 or 5/4 of the beat.
constexpr double kCandidateFraction = 0.85;
constexpr double kOctaveFactors[] = {2.0, 3.0};  ///< faster metrical levels tried against the winner
/// The faster tempo wins at >= 85% of the slower one's prior-weighted comb...
constexpr double kOctaveAcceptance = 0.85;
/// ...and only when its prior-weighted beat salience reaches 79% of the slower one's: its grid
/// must land on onsets nearly as strong as the slower grid does. A subdivision that falls between
/// the accents (the 8th or 16th hats of a groove) fails, unless the prior pulls towards it, as for
/// a half-time backbeat at 70 BPM whose 140 BPM pulse is the musically conventional reading.
constexpr double kSalienceAcceptance = 0.79;
constexpr double kOctaveSearchFraction = 0.02;           ///< faster tempo searched within +-2% of factor x bpm
constexpr double kMinimumPeriodicity = 0.25;             ///< beat-comb score below which tempo is unknown
constexpr double kTransientPeakFraction = 0.2;           ///< transient = local maximum above 20% of the largest
constexpr double kTransientMinimumSpacingSeconds = 0.1;  ///< closer peaks count as one transient
constexpr std::size_t kMinimumTransients = 3;            ///< fewer transients means no tempo (one-shots)
/// Beat-grid points snap to the strongest novelty within this many frames, absorbing the drift of a
/// coarse-grid tempo over the analysis window.
constexpr std::size_t kGridSnapFrames = 1;

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
    std::vector<double> correlation_;  ///< autocorrelation by integer lag
    double max_lag_ = 0.0;             ///< longest lag considered (kMaximumLagFraction of the envelope)
};

/// Length of one beat at `bpm`, in envelope frames.
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

/// Best beat comb, unweighted, on the coarse grid within [lowest, highest].
std::optional<TempoCandidate> best_candidate(const LagCorrelation& correlation, double frame_rate, double lowest,
                                             double highest) {
    std::optional<TempoCandidate> best;
    const auto steps = static_cast<int>(std::floor((highest - lowest) / kCoarseStepBpm));
    for (int step = 0; step <= steps; ++step) {
        const double bpm = lowest + step * kCoarseStepBpm;
        const std::optional<double> comb = beat_comb(correlation, bpm, frame_rate);
        if (!comb || *comb <= 0.0) continue;
        if (!best || *comb > best->comb) best = TempoCandidate{bpm, *comb};
    }
    return best;
}

/// Local maxima of the prior-weighted comb over the whole reported range that score at least
/// kCandidateFraction of the best one: the metrical levels the periodicity alone cannot tell apart.
std::vector<TempoCandidate> candidate_levels(const LagCorrelation& correlation, double frame_rate) {
    const auto steps = static_cast<int>(std::floor((kMaximumReportedBpm - kMinimumReportedBpm) / kCoarseStepBpm));
    std::vector<TempoCandidate> grid;
    std::vector<double> weighted;
    grid.reserve(static_cast<std::size_t>(steps) + 1);
    weighted.reserve(static_cast<std::size_t>(steps) + 1);
    for (int step = 0; step <= steps; ++step) {
        const double bpm = kMinimumReportedBpm + step * kCoarseStepBpm;
        const double comb = beat_comb(correlation, bpm, frame_rate).value_or(0.0);
        grid.push_back(TempoCandidate{bpm, comb});
        weighted.push_back(comb > 0.0 ? comb * tempo_prior(bpm) : 0.0);
    }
    const double best = weighted.empty() ? 0.0 : *std::max_element(weighted.begin(), weighted.end());
    std::vector<TempoCandidate> levels;
    if (!(best > 0.0)) return levels;
    for (std::size_t index = 0; index < weighted.size(); ++index) {
        const bool rises = index == 0 || weighted[index] > weighted[index - 1];
        const bool falls = index + 1 == weighted.size() || weighted[index] >= weighted[index + 1];
        if (rises && falls && weighted[index] >= kCandidateFraction * best) levels.push_back(grid[index]);
    }
    return levels;
}

/// How strongly a beat grid at `bpm` lands on onsets: the mean novelty at the grid points (each
/// snapped to the strongest frame within kGridSnapFrames), for the best phase, relative to the
/// mean novelty of the whole signal. A grid on the accents (kick and snare) scores higher than one
/// that also visits the weaker onsets in between (hats), even when both are equally periodic.
double beat_salience(std::span<const double> novelty, double novelty_mean, double bpm, double frame_rate) {
    const double beat_lag = beat_lag_frames(bpm, frame_rate);
    if (!(novelty_mean > 0.0) || !(beat_lag >= 1.0) || novelty.empty()) return 0.0;
    const std::size_t count = novelty.size();
    const auto snapped = [&](double position) {
        const auto center = static_cast<std::size_t>(std::llround(position));
        const std::size_t first = center > kGridSnapFrames ? center - kGridSnapFrames : 0;
        const std::size_t last = std::min(count - 1, center + kGridSnapFrames);
        return *std::max_element(novelty.begin() + static_cast<std::ptrdiff_t>(first),
                                 novelty.begin() + static_cast<std::ptrdiff_t>(last) + 1);
    };
    double best_mean = 0.0;
    const auto phases = static_cast<std::size_t>(std::ceil(beat_lag));
    for (std::size_t phase = 0; phase < phases; ++phase) {
        double sum = 0.0;
        std::size_t points = 0;
        for (double position = static_cast<double>(phase); position < static_cast<double>(count - 1);
             position += beat_lag) {
            sum += snapped(position);
            ++points;
        }
        if (points > 0) best_mean = std::max(best_mean, sum / static_cast<double>(points));
    }
    return best_mean / novelty_mean;
}

/// Metrical-level selection. Among the candidate `levels` (non-empty), the beat is the one whose
/// grid best lands on the accents, weighted by the prior; then, while two or three times the tempo
/// (still in range) explains the correlation nearly as well and its grid still lands on onsets
/// nearly as strong (both weighted by the prior), the faster tempo is taken as the beat and the
/// slower one as a grouping of beats. The salience test keeps a dense subdivision (16th-note hats
/// over a kick on every beat, 8th-note hats over an 85 BPM half-time backbeat) from doubling the
/// tempo, while an even pulse (a kick on every beat) still climbs from its half-tempo comb peak.
TempoCandidate choose_beat_level(const LagCorrelation& correlation, std::span<const TempoCandidate> levels,
                                 std::span<const double> novelty, double frame_rate) {
    double novelty_sum = 0.0;
    for (const double value : novelty)
        novelty_sum += value;
    const double novelty_mean = novelty.empty() ? 0.0 : novelty_sum / static_cast<double>(novelty.size());
    // Salience weighted by the prior: how well a grid at `bpm` lands on accents, and how plausible
    // the tempo is.
    const auto weighted_salience = [&](double bpm) {
        return beat_salience(novelty, novelty_mean, bpm, frame_rate) * tempo_prior(bpm);
    };

    TempoCandidate beat = levels.front();
    double beat_score = weighted_salience(beat.bpm);
    for (const TempoCandidate& level : levels) {
        const double score = weighted_salience(level.bpm);
        if (score > beat_score) {
            beat = level;
            beat_score = score;
        }
    }

    bool climbed = true;
    while (climbed) {
        climbed = false;
        for (const double factor : kOctaveFactors) {
            const double target = beat.bpm * factor;
            const double lowest = target * (1.0 - kOctaveSearchFraction);
            if (lowest > kMaximumReportedBpm) continue;
            const double highest = std::min(kMaximumReportedBpm, target * (1.0 + kOctaveSearchFraction));
            const std::optional<TempoCandidate> faster = best_candidate(correlation, frame_rate, lowest, highest);
            if (!faster) continue;
            const bool periodic_enough =
                faster->comb * tempo_prior(faster->bpm) >= kOctaveAcceptance * beat.comb * tempo_prior(beat.bpm);
            if (periodic_enough &&
                weighted_salience(faster->bpm) >= kSalienceAcceptance * weighted_salience(beat.bpm)) {
                beat = *faster;
                climbed = true;
                break;
            }
        }
    }
    return beat;
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

    const std::vector<double> novelty = rectified_novelty(onset_envelope, frame_rate);
    if (count_transients(novelty, frame_rate) < kMinimumTransients) return unknown;

    const LagCorrelation correlation(novelty);
    if (!correlation.valid()) return unknown;
    const std::vector<TempoCandidate> levels = candidate_levels(correlation, frame_rate);
    if (levels.empty()) return unknown;

    const TempoCandidate beat = choose_beat_level(correlation, levels, novelty, frame_rate);
    const double bpm = refine_bpm(correlation, beat.bpm, frame_rate);
    const double periodicity = beat_comb(correlation, bpm, frame_rate).value_or(0.0);
    if (periodicity < kMinimumPeriodicity) return unknown;

    TempoResult result;
    result.milli_bpm = std::llround(bpm * 1000.0);
    result.confidence = std::clamp(periodicity, 0.0, 1.0);
    return result;
}

}  // namespace mso::analysis
