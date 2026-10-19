/// @file
/// Downmix and dispatch to the tempo and key feature builders.
#include "mso/analysis/musical_features.hpp"

#include <algorithm>

#include "mso/analysis/key_estimator.hpp"
#include "mso/analysis/tempo_estimator.hpp"

namespace mso::analysis {

MusicalFeatureExtractor::MusicalFeatureExtractor(int sample_rate, int channels, std::int64_t max_frames,
                                                 bool analyze_tempo, bool analyze_key)
    : channels_(std::max(channels, 1)), max_frames_(std::max<std::int64_t>(max_frames, 0)) {
    if (analyze_tempo) onset_envelope_.emplace(sample_rate);
    if (analyze_key) chroma_.emplace(sample_rate);
}

void MusicalFeatureExtractor::add(std::span<const float> interleaved_samples) {
    const auto channel_count = static_cast<std::size_t>(channels_);
    const auto remaining = static_cast<std::size_t>(max_frames_ - std::min(frames_analyzed_, max_frames_));
    const std::size_t frame_count = std::min(interleaved_samples.size() / channel_count, remaining);
    if (frame_count == 0) return;

    mono_samples_.resize(frame_count);
    const float channel_weight = 1.0F / static_cast<float>(channel_count);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        float sum = 0.0F;
        for (std::size_t channel = 0; channel < channel_count; ++channel)
            sum += interleaved_samples[frame * channel_count + channel];
        mono_samples_[frame] = sum * channel_weight;
    }

    if (onset_envelope_) onset_envelope_->add(mono_samples_);
    if (chroma_) chroma_->add(mono_samples_);
    frames_analyzed_ += static_cast<std::int64_t>(frame_count);
}

std::optional<TempoResult> MusicalFeatureExtractor::tempo() const {
    if (!onset_envelope_) return std::nullopt;
    return estimate_tempo(onset_envelope_->envelope(), onset_envelope_->frame_rate());
}

std::optional<KeyResult> MusicalFeatureExtractor::key() const {
    if (!chroma_) return std::nullopt;
    return estimate_key(chroma_->profile());
}

}  // namespace mso::analysis
