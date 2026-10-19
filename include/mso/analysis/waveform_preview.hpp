#pragma once
/// @file
/// The narrow waveform entry point for the desktop preview. Invariant 10 allows the interactive
/// preview to decode the selected file; this function does exactly that and nothing more: it
/// links only the decoder and the waveform builder (no tempo, key or hashing code), writes nothing
/// and never stamps evidence, so it is not an analysis run and its result is not an artifact.
#include <filesystem>
#include <stop_token>

#include "mso/analysis/analysis.hpp"
#include "mso/domain/error.hpp"

namespace mso::analysis {

/// Decodes `file` once and returns its min/max/RMS overview in at most `bins` bins (more than
/// bins / 2 when the length is not known exactly up front).
///
/// `progress` receives fractions in [0, 1] at most 10 times per second when the decoder knows the
/// length, then a final 1.0; it runs on the calling thread. Cancellation is checked before every
/// decoded block. No exception escapes, whatever the decoder or the callback throws.
///
/// Errors: INVALID_ARGUMENT for `bins` outside [1, kMaximumWaveformBins], CANCELLED, the decoder's
/// errors (FILE_MISSING, ACCESS_DENIED, UNSUPPORTED_CODEC, CORRUPT_AUDIO, ...), RESOURCE_LIMIT
/// when memory runs out and INTERNAL for any other failure.
[[nodiscard]] Result<WaveformOverview> compute_waveform_overview(const std::filesystem::path& file, int bins,
                                                                 std::stop_token stop,
                                                                 const ProgressCallback& progress = {});

}  // namespace mso::analysis
