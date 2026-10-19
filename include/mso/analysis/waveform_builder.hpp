#pragma once
/// @file
/// Bounded-memory waveform overview accumulation (algorithm AlgorithmVersions::kWaveform).
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "mso/analysis/analysis.hpp"

namespace mso::analysis {

/// Largest bin count accepted from a request or a stored overview.
inline constexpr int kMaximumWaveformBins = 1 << 20;

/// Accumulates signed min/max and RMS per bin across all channels while audio streams through.
///
/// With a known frame count the bin width is ceil(total / max_bins), so the overview spans the
/// whole file in at most max_bins bins. When the count is unknown or turns out too small, the
/// builder starts narrow and, whenever another bin would exceed max_bins, doubles
/// frames_per_bin and merges neighbouring pairs, so memory never exceeds max_bins bins.
/// Bin boundaries are always multiples of frames_per_bin from frame 0.
class WaveformBuilder {
public:
    /// @param max_bins upper bound on bins, clamped to [1, kMaximumWaveformBins].
    /// @param expected_total_frames frame-count hint from the decoder, if any.
    WaveformBuilder(int max_bins, int sample_rate, std::optional<std::int64_t> expected_total_frames);

    /// Adds interleaved samples (whole frames of `channels` samples; a trailing partial frame is ignored).
    void add(std::span<const float> interleaved_samples, int channels);

    std::int64_t frames_covered() const noexcept { return frames_covered_; }

    /// Snapshot of the overview. `complete` marks a fully decoded stream, in which case
    /// total_frames equals frames_covered; otherwise total_frames is the hint when it is not
    /// below the covered count, or 0 (unknown).
    WaveformOverview overview(bool complete) const;

private:
    /// Running statistics of one bin.
    struct BinAccumulator {
        float minimum = 0.0F;
        float maximum = 0.0F;
        double sum_of_squares = 0.0;
        std::int64_t sample_count = 0;

        void add(float sample) noexcept;
        void merge(const BinAccumulator& other) noexcept;
    };

    /// Doubles frames_per_bin and merges neighbouring bins pairwise.
    void coarsen();

    std::size_t max_bins_;
    int sample_rate_;
    std::optional<std::int64_t> expected_total_frames_;
    std::int64_t frames_per_bin_ = 1;
    std::int64_t frames_covered_ = 0;
    std::vector<BinAccumulator> bins_;
};

}  // namespace mso::analysis
