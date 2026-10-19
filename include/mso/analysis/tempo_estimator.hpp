#pragma once
/// @file
/// Tempo estimation from an onset envelope (algorithm AlgorithmVersions::kTempo).
#include <span>

#include "mso/analysis/analysis.hpp"

namespace mso::analysis {

/// Tempo range reported by the estimator. A pulse outside it is reported at the octave
/// (half or double tempo) that falls inside, never clamped.
inline constexpr double kMinimumReportedBpm = 60.0;
inline constexpr double kMaximumReportedBpm = 200.0;

/// Estimates the tempo of an onset envelope sampled at `frame_rate` values per second.
///
/// Steps: local-mean removal and half-wave rectification of the envelope; normalized unbiased
/// autocorrelation r; a beat comb (mean of r at the first four beat lags) over 60-200 BPM whose
/// prior-weighted peaks (weak log-normal prior around 120 BPM) are the candidate metrical levels;
/// among them the level whose beat grid lands on the strongest onsets (beat salience), weighted
/// by the prior; octave handling that moves to 2x or 3x the tempo while the faster pulse explains
/// r nearly as well and its grid still lands on comparably strong onsets, both weighted by the
/// prior (a slow tempo's comb also hits every multiple of the true beat, but a dense subdivision
/// such as 16th-note hats is not the beat); and a fine search within +-3% that sums up to 32
/// beat lags for sub-frame precision. Confidence is the beat comb at the final tempo.
///
/// Returns an unknown tempo (nullopt) when the envelope covers fewer than three beats, holds
/// fewer than three distinct transients (one-shots), or when the periodicity score is below the
/// reliability threshold (noise, ambience, sustained tones). Deterministic for a given input.
TempoResult estimate_tempo(std::span<const float> onset_envelope, double frame_rate);

}  // namespace mso::analysis
