#pragma once
/// @file
/// Mono front end shared by the tempo and key stages: downmix, duration limit and the two
/// streaming feature builders.
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "mso/analysis/analysis.hpp"
#include "mso/analysis/chroma_accumulator.hpp"
#include "mso/analysis/onset_envelope.hpp"

namespace mso::analysis {

/// Feeds the first `max_frames` frames of a decoded stream to the onset-envelope (tempo) and
/// chroma (key) builders.
///
/// Channels are downmixed with equal 1/N coefficients: the decoder does not expose a channel
/// layout, and 1/N can never clip. Memory is bounded by max_frames through the envelope (one
/// float per ~11.6 ms hop); no audio is retained.
class MusicalFeatureExtractor {
public:
    /// @param analyze_tempo / analyze_key select which builders run.
    MusicalFeatureExtractor(int sample_rate, int channels, std::int64_t max_frames, bool analyze_tempo,
                            bool analyze_key);

    /// Adds interleaved frames; frames beyond max_frames are ignored.
    void add(std::span<const float> interleaved_samples);

    /// True once max_frames frames have been analyzed; later audio is ignored.
    bool saturated() const noexcept { return frames_analyzed_ >= max_frames_; }
    std::int64_t frames_analyzed() const noexcept { return frames_analyzed_; }

    /// Tempo of the analyzed audio; nullopt when tempo analysis was not requested.
    std::optional<TempoResult> tempo() const;
    /// Key of the analyzed audio; nullopt when key analysis was not requested.
    std::optional<KeyResult> key() const;

private:
    int channels_;
    std::int64_t max_frames_;
    std::int64_t frames_analyzed_ = 0;
    std::optional<OnsetEnvelopeBuilder> onset_envelope_;
    std::optional<ChromaAccumulator> chroma_;
    std::vector<float> mono_samples_;  ///< reused downmix scratch for one block
};

}  // namespace mso::analysis
