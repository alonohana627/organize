#pragma once
/// @file
/// Internal: the block-by-block decode loop shared by analyze_decoded_stream() and
/// compute_waveform_overview(). Header-only, so the preview entry point reuses it without linking
/// the tempo, key or hashing stages.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <stop_token>
#include <vector>

#include "mso/analysis/limits.hpp"
#include "mso/domain/error.hpp"
#include "mso/media/media.hpp"

namespace mso::analysis::detail {

/// Checks the channel count and sample rate a decoder reports before any stage relies on them:
/// CORRUPT_AUDIO for no channels or an impossible rate, UNSUPPORTED_CODEC above kMaximumChannels.
inline Result<void> validate_stream_format(const media::Decoder& decoder) {
    const int channels = decoder.channels();
    const int sample_rate = decoder.sample_rate();
    if (channels < 1) return fail(ErrorCode::CorruptAudio, "decoder reports no channels");
    if (channels > kMaximumChannels)
        return fail(ErrorCode::UnsupportedCodec, std::format("{} channels exceed the analysis limit", channels));
    if (sample_rate < 1 || sample_rate > kMaximumSampleRate)
        return fail(ErrorCode::CorruptAudio, std::format("invalid sample rate {}", sample_rate));
    return {};
}

/// Replaces NaN and infinities with 0 so no stage sees them; returns how many were replaced.
inline std::int64_t replace_non_finite_samples(std::span<float> samples) noexcept {
    std::int64_t replaced = 0;
    for (float& sample : samples) {
        if (!std::isfinite(sample)) {
            sample = 0.0F;
            ++replaced;
        }
    }
    return replaced;
}

/// How a decode loop ended.
struct DecodeLoopSummary {
    std::int64_t frames_decoded = 0;
    std::int64_t non_finite_samples = 0;  ///< replaced by silence
    bool reached_end = false;             ///< false when the handler asked to stop early
};

/// Reads `decoder` from its current position in kDecodeBlockFrames blocks, sanitizes each block
/// with replace_non_finite_samples() and hands it to `on_block(std::span<const float> samples,
/// std::int64_t frames_decoded_so_far) -> bool`, which returns false to stop before the end.
/// The decoder's format must have passed validate_stream_format().
///
/// Errors: CANCELLED (checked before every read), the decoder's own errors, and INTERNAL when a
/// decoder returns more frames than requested.
template <class BlockHandler>
Result<DecodeLoopSummary> decode_blocks(media::Decoder& decoder, const std::stop_token& stop, BlockHandler&& on_block) {
    const auto channels = static_cast<std::size_t>(decoder.channels());
    std::vector<float> block(kDecodeBlockFrames * channels);
    DecodeLoopSummary summary;
    while (true) {
        if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "analysis cancelled");
        const Result<std::size_t> frames_read = decoder.read(block.data(), kDecodeBlockFrames);
        if (!frames_read) return std::unexpected(frames_read.error());
        if (*frames_read == 0) {
            summary.reached_end = true;
            return summary;
        }
        if (*frames_read > kDecodeBlockFrames)
            return fail(ErrorCode::Internal, "decoder returned more frames than requested");

        const std::span<float> samples(block.data(), *frames_read * channels);
        summary.non_finite_samples += replace_non_finite_samples(samples);
        summary.frames_decoded += static_cast<std::int64_t>(*frames_read);
        if (!on_block(std::span<const float>(samples), summary.frames_decoded)) return summary;
    }
}

}  // namespace mso::analysis::detail
