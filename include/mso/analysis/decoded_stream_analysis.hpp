#pragma once
/// @file
/// The decode-driven half of analyze_file(): one streaming pass over an open decoder that feeds
/// every requested decode stage. Split out so it can run against any media::Decoder, including
/// in-memory decoders in tests.
#include <cstddef>
#include <span>
#include <stop_token>

#include "mso/analysis/analysis.hpp"
#include "mso/analysis/progress_reporter.hpp"
#include "mso/media/media.hpp"

namespace mso::analysis {

/// Frames requested from the decoder per read; bounds memory and cancellation latency.
inline constexpr std::size_t kDecodeBlockFrames = 4096;
/// Largest channel count analyzed; more is reported as UNSUPPORTED_CODEC.
inline constexpr int kMaximumChannels = 64;
/// Upper bound of AnalysisRequest::max_musical_seconds (keeps the onset envelope bounded).
inline constexpr double kMaximumMusicalSeconds = 3600.0;

/// Requested stages as flags, with duplicates collapsed.
struct StageSelection {
    bool waveform = false;
    bool loudness = false;
    bool tempo = false;
    bool key = false;
    bool byte_hash = false;
    bool pcm_hash = false;

    static StageSelection from(std::span<const Stage> stages) noexcept;
    /// Stages that need every decoded frame, not only the musical window.
    bool needs_whole_stream() const noexcept { return waveform || loudness || pcm_hash; }
    bool needs_musical_features() const noexcept { return tempo || key; }
    bool needs_decode() const noexcept { return needs_whole_stream() || needs_musical_features(); }
};

/// Checks the request's numeric limits: waveform_bins in [1, kMaximumWaveformBins] when the
/// waveform stage is requested, and max_musical_seconds finite in (0, kMaximumMusicalSeconds].
Result<void> validate_request(const AnalysisRequest& request);

/// Reads the decoder from its current position to the end (or, when only tempo/key are
/// requested, until the musical window is full) in kDecodeBlockFrames blocks and fills the
/// waveform, loudness, tempo, key and pcm_sha256 parts of the report plus frames_decoded and
/// warnings. Ignores Stage::ByteHash, which works on file bytes. Non-finite samples are treated
/// as silence (0) by every stage and reported in a warning.
///
/// Errors: CANCELLED (checked before every block), decoder errors as returned by the decoder,
/// UNSUPPORTED_CODEC / CORRUPT_AUDIO for unusable channel counts or sample rates.
Result<AnalysisReport> analyze_decoded_stream(media::Decoder& decoder, const AnalysisRequest& request,
                                              std::stop_token stop, const ProgressPhase& progress);

}  // namespace mso::analysis
