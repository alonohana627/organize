/// @file
/// Waveform overview accumulation with adaptive bin coarsening.
#include "mso/analysis/waveform_builder.hpp"

#include <algorithm>
#include <cmath>

namespace mso::analysis {

void WaveformBuilder::BinAccumulator::add(float sample) noexcept {
    if (sample_count == 0) {
        minimum = sample;
        maximum = sample;
    } else {
        minimum = std::min(minimum, sample);
        maximum = std::max(maximum, sample);
    }
    sum_of_squares += static_cast<double>(sample) * sample;
    ++sample_count;
}

void WaveformBuilder::BinAccumulator::merge(const BinAccumulator& other) noexcept {
    if (other.sample_count == 0) return;
    if (sample_count == 0) {
        *this = other;
        return;
    }
    minimum = std::min(minimum, other.minimum);
    maximum = std::max(maximum, other.maximum);
    sum_of_squares += other.sum_of_squares;
    sample_count += other.sample_count;
}

WaveformBuilder::WaveformBuilder(int max_bins, int sample_rate, std::optional<std::int64_t> exact_total_frames)
    : max_bins_(static_cast<std::size_t>(std::clamp(max_bins, 1, kMaximumWaveformBins))),
      sample_rate_(sample_rate),
      exact_total_frames_(exact_total_frames) {
    if (exact_total_frames && *exact_total_frames > 0) {
        const auto bin_limit = static_cast<std::int64_t>(max_bins_);
        frames_per_bin_ = std::max<std::int64_t>(1, (*exact_total_frames + bin_limit - 1) / bin_limit);
        // Reserve what the known length needs, not max_bins: a short file stays small.
        bins_.reserve(static_cast<std::size_t>((*exact_total_frames + frames_per_bin_ - 1) / frames_per_bin_));
    }
}

void WaveformBuilder::add(std::span<const float> interleaved_samples, int channels) {
    if (channels <= 0) return;
    const auto channel_count = static_cast<std::size_t>(channels);
    const std::size_t frame_count = interleaved_samples.size() / channel_count;

    std::size_t frame = 0;
    while (frame < frame_count) {
        auto bin_index = static_cast<std::size_t>(frames_covered_ / frames_per_bin_);
        while (bin_index >= max_bins_) {
            coarsen();
            bin_index = static_cast<std::size_t>(frames_covered_ / frames_per_bin_);
        }
        if (bin_index == bins_.size()) bins_.emplace_back();

        // Consume the run of frames that belongs to the current bin in one pass.
        const std::int64_t frames_left_in_bin = frames_per_bin_ - frames_covered_ % frames_per_bin_;
        const std::size_t run = std::min(frame_count - frame, static_cast<std::size_t>(frames_left_in_bin));
        BinAccumulator& bin = bins_[bin_index];
        const auto run_samples = interleaved_samples.subspan(frame * channel_count, run * channel_count);
        for (const float sample : run_samples)
            bin.add(sample);
        frame += run;
        frames_covered_ += static_cast<std::int64_t>(run);
    }
}

void WaveformBuilder::coarsen() {
    std::size_t merged_count = 0;
    for (std::size_t index = 0; index < bins_.size(); index += 2) {
        BinAccumulator merged = bins_[index];
        if (index + 1 < bins_.size()) merged.merge(bins_[index + 1]);
        bins_[merged_count++] = merged;
    }
    bins_.resize(merged_count);
    frames_per_bin_ *= 2;
}

WaveformOverview WaveformBuilder::overview(bool complete) const {
    WaveformOverview overview;
    overview.frames_per_bin = frames_per_bin_;
    overview.frames_covered = frames_covered_;
    overview.sample_rate = sample_rate_;
    overview.complete = complete;
    if (complete) {
        overview.total_frames = frames_covered_;
    } else if (exact_total_frames_ && *exact_total_frames_ >= frames_covered_) {
        overview.total_frames = *exact_total_frames_;
    }

    overview.minimum.reserve(bins_.size());
    overview.maximum.reserve(bins_.size());
    overview.rms.reserve(bins_.size());
    for (const BinAccumulator& bin : bins_) {
        const double mean_square =
            bin.sample_count > 0 ? bin.sum_of_squares / static_cast<double>(bin.sample_count) : 0.0;
        overview.minimum.push_back(std::clamp(bin.minimum, -1.0F, 1.0F));
        overview.maximum.push_back(std::clamp(bin.maximum, -1.0F, 1.0F));
        overview.rms.push_back(static_cast<float>(std::min(std::sqrt(mean_square), 1.0)));
    }
    return overview;
}

}  // namespace mso::analysis
