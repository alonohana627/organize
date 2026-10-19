#pragma once
/// @file
/// Musical key estimation from a chroma profile (algorithm AlgorithmVersions::kKey).
#include "mso/analysis/analysis.hpp"
#include "mso/analysis/chroma_accumulator.hpp"

namespace mso::analysis {

/// Correlates the profile against the 24 rotated Krumhansl-Kessler major and minor key profiles
/// and returns the best key.
///
/// The key is unknown (nullopt) when the audio is not tonal enough to decide: too little tonal
/// material, a nearly flat chroma (noise, clicks, dense inharmonic sound), fewer than three
/// active pitch classes (a single note is a pitch, not evidence of major or minor), a weak best
/// correlation, or a small margin between the best and second-best key. Confidence is the best
/// correlation scaled by how clearly it beats the runner-up; it is a heuristic, not a probability.
KeyResult estimate_key(const ChromaProfile& profile);

}  // namespace mso::analysis
