/// @file
/// Single streaming decode pass shared by the waveform, loudness, tempo, key and PCM-hash stages.
#include "mso/analysis/decoded_stream_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <vector>

#include "mso/analysis/loudness_meter.hpp"
#include "mso/analysis/musical_features.hpp"
#include "mso/analysis/pcm_hasher.hpp"
#include "mso/analysis/waveform_builder.hpp"

namespace mso::analysis {
namespace {

constexpr int kMaximumSampleRate = 10'000'000;

/// Replaces NaN and infinities with 0 so no stage sees them; returns how many were replaced.
std::int64_t replace_non_finite_samples(std::span<float> samples) noexcept {
    std::int64_t replaced = 0;
    for (float& sample : samples) {
        if (!std::isfinite(sample)) {
            sample = 0.0F;
            ++replaced;
        }
    }
    return replaced;
}

/// The consumers of one decode pass; each is present only when its stage was requested.
struct StageConsumers {
    std::optional<WaveformBuilder> waveform;
    std::optional<LoudnessMeter> loudness;
    std::optional<PcmHasher> pcm_hash;
    std::optional<MusicalFeatureExtractor> musical;

    void add(std::span<const float> samples, int channels) {
        if (waveform) waveform->add(samples, channels);
        if (loudness) loudness->add(samples);
        if (pcm_hash) pcm_hash->add(samples);
        if (musical) musical->add(samples);
    }
};

}  // namespace

StageSelection StageSelection::from(std::span<const Stage> stages) noexcept {
    StageSelection selection;
    for (const Stage stage : stages) {
        switch (stage) {
            case Stage::Waveform:
                selection.waveform = true;
                break;
            case Stage::Loudness:
                selection.loudness = true;
                break;
            case Stage::Tempo:
                selection.tempo = true;
                break;
            case Stage::Key:
                selection.key = true;
                break;
            case Stage::ByteHash:
                selection.byte_hash = true;
                break;
            case Stage::PcmHash:
                selection.pcm_hash = true;
                break;
        }
    }
    return selection;
}

Result<void> validate_request(const AnalysisRequest& request) {
    const StageSelection stages = StageSelection::from(request.stages);
    if (stages.waveform && (request.waveform_bins < 1 || request.waveform_bins > kMaximumWaveformBins))
        return fail(ErrorCode::InvalidArgument, std::format("waveform_bins must be in [1, {}]", kMaximumWaveformBins));
    if (!std::isfinite(request.max_musical_seconds) || request.max_musical_seconds <= 0.0 ||
        request.max_musical_seconds > kMaximumMusicalSeconds)
        return fail(ErrorCode::InvalidArgument,
                    std::format("max_musical_seconds must be in (0, {}]", kMaximumMusicalSeconds));
    return {};
}

Result<AnalysisReport> analyze_decoded_stream(media::Decoder& decoder, const AnalysisRequest& request,
                                              std::stop_token stop, const ProgressPhase& progress) {
    if (auto valid = validate_request(request); !valid) return std::unexpected(valid.error());
    const StageSelection stages = StageSelection::from(request.stages);
    AnalysisReport report;
    if (!stages.needs_decode()) return report;

    const int channels = decoder.channels();
    const int sample_rate = decoder.sample_rate();
    if (channels < 1) return fail(ErrorCode::CorruptAudio, "decoder reports no channels");
    if (channels > kMaximumChannels)
        return fail(ErrorCode::UnsupportedCodec, std::format("{} channels exceed the analysis limit", channels));
    if (sample_rate < 1 || sample_rate > kMaximumSampleRate)
        return fail(ErrorCode::CorruptAudio, std::format("invalid sample rate {}", sample_rate));

    const std::optional<std::int64_t> reported_frames = decoder.total_frames();
    const std::optional<std::int64_t> known_total =
        reported_frames && *reported_frames > 0 ? reported_frames : std::optional<std::int64_t>{};
    const auto max_musical_frames =
        static_cast<std::int64_t>(std::floor(request.max_musical_seconds * static_cast<double>(sample_rate)));

    StageConsumers consumers;
    if (stages.waveform) consumers.waveform.emplace(request.waveform_bins, sample_rate, known_total);
    if (stages.loudness) consumers.loudness.emplace();
    if (stages.pcm_hash) consumers.pcm_hash.emplace(channels, sample_rate);
    if (stages.needs_musical_features())
        consumers.musical.emplace(sample_rate, channels, max_musical_frames, stages.tempo, stages.key);

    // Frames this pass will read, for progress; unknown length means no intermediate progress.
    std::optional<std::int64_t> progress_target = known_total;
    if (!stages.needs_whole_stream() && progress_target)
        progress_target = std::min(*progress_target, max_musical_frames);

    std::vector<float> block(kDecodeBlockFrames * static_cast<std::size_t>(channels));
    std::int64_t frames_decoded = 0;
    std::int64_t non_finite_samples = 0;
    bool reached_end = false;
    while (true) {
        if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "analysis cancelled");
        const Result<std::size_t> frames_read = decoder.read(block.data(), kDecodeBlockFrames);
        if (!frames_read) return std::unexpected(frames_read.error());
        if (*frames_read == 0) {
            reached_end = true;
            break;
        }
        if (*frames_read > kDecodeBlockFrames)
            return fail(ErrorCode::Internal, "decoder returned more frames than requested");

        const std::span<float> samples(block.data(), *frames_read * static_cast<std::size_t>(channels));
        non_finite_samples += replace_non_finite_samples(samples);
        consumers.add(samples, channels);
        frames_decoded += static_cast<std::int64_t>(*frames_read);
        if (progress_target && *progress_target > 0)
            progress.report(static_cast<double>(frames_decoded) / static_cast<double>(*progress_target));

        // Only tempo/key requested: stop once audio beyond the musical window is known to exist.
        if (!stages.needs_whole_stream() && frames_decoded > max_musical_frames) break;
    }

    report.frames_decoded = frames_decoded;
    if (consumers.waveform) report.waveform = consumers.waveform->overview(reached_end);
    if (consumers.loudness) report.loudness = consumers.loudness->result();
    if (consumers.pcm_hash) report.hashes.pcm_sha256 = consumers.pcm_hash->finish();
    if (consumers.musical) {
        report.tempo = consumers.musical->tempo();
        report.key = consumers.musical->key();
        if (frames_decoded > max_musical_frames)
            report.warnings.push_back(
                std::format("tempo/key analyzed only the first {:.1f} s", request.max_musical_seconds));
    }
    if (non_finite_samples > 0)
        report.warnings.push_back(std::format("{} non-finite samples treated as silence", non_finite_samples));
    if (reached_end && decoder.frames_exact() && known_total && *known_total != frames_decoded)
        report.warnings.push_back(
            std::format("decoder announced {} frames but produced {}", *known_total, frames_decoded));
    return report;
}

}  // namespace mso::analysis
