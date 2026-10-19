/// @file
/// Single streaming decode pass shared by the waveform, loudness, tempo, key and PCM-hash stages.
#include "mso/analysis/decoded_stream_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <optional>

#include "mso/analysis/detail/decode_loop.hpp"
#include "mso/analysis/loudness_meter.hpp"
#include "mso/analysis/musical_features.hpp"
#include "mso/analysis/pcm_hasher.hpp"
#include "mso/analysis/waveform_builder.hpp"

namespace mso::analysis {
namespace {

/// The consumers of one decode pass; each is present only when its stage was requested.
struct StageConsumers {
    std::optional<WaveformBuilder> waveform;
    std::optional<LoudnessMeter> loudness;
    std::optional<PcmHasher> pcm_hash;
    std::optional<MusicalFeatureExtractor> musical;

    /// Feeds one block of interleaved samples to every present consumer.
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
    for (auto stage = request.stages.begin(); stage != request.stages.end(); ++stage) {
        if (std::find(std::next(stage), request.stages.end(), *stage) != request.stages.end())
            return fail(ErrorCode::InvalidArgument, std::format("stage {} is requested twice", to_string(*stage)));
    }
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
    if (auto format = detail::validate_stream_format(decoder); !format) return std::unexpected(format.error());

    const int channels = decoder.channels();
    const int sample_rate = decoder.sample_rate();
    const std::optional<std::int64_t> reported_frames = decoder.total_frames();
    const std::optional<std::int64_t> known_total =
        reported_frames && *reported_frames > 0 ? reported_frames : std::optional<std::int64_t>{};
    // Only an exact count may size the waveform bins; an estimate (VBR MP3) could leave them too coarse.
    const std::optional<std::int64_t> exact_total = decoder.frames_exact() ? known_total : std::nullopt;
    const auto max_musical_frames =
        static_cast<std::int64_t>(std::floor(request.max_musical_seconds * static_cast<double>(sample_rate)));

    StageConsumers consumers;
    if (stages.waveform) consumers.waveform.emplace(request.waveform_bins, sample_rate, exact_total);
    if (stages.loudness) consumers.loudness.emplace();
    if (stages.pcm_hash) consumers.pcm_hash.emplace(channels, sample_rate);
    if (stages.needs_musical_features())
        consumers.musical.emplace(sample_rate, channels, max_musical_frames, stages.tempo, stages.key);

    // Frames this pass will read, for progress; unknown length means no intermediate progress
    // (the work counter still advances once per block).
    std::optional<std::int64_t> progress_target = known_total;
    if (!stages.needs_whole_stream() && progress_target)
        progress_target = std::min(*progress_target, max_musical_frames);

    const auto summary =
        detail::decode_blocks(decoder, stop, [&](std::span<const float> samples, std::int64_t frames_decoded) {
            consumers.add(samples, channels);
            progress.record_work();
            if (progress_target && *progress_target > 0)
                progress.report(static_cast<double>(frames_decoded) / static_cast<double>(*progress_target));
            // Only tempo/key requested: stop once audio beyond the musical window is known to exist.
            return stages.needs_whole_stream() || frames_decoded <= max_musical_frames;
        });
    if (!summary) return std::unexpected(summary.error());
    const std::int64_t frames_decoded = summary->frames_decoded;

    report.frames_decoded = frames_decoded;
    if (consumers.waveform) report.waveform = consumers.waveform->overview(summary->reached_end);
    if (consumers.loudness) report.loudness = consumers.loudness->result();
    if (consumers.pcm_hash) report.hashes.pcm_sha256 = consumers.pcm_hash->finish();
    if (consumers.musical) {
        report.tempo = consumers.musical->tempo();
        report.key = consumers.musical->key();
        progress.record_work();
        if (frames_decoded > max_musical_frames)
            report.warnings.push_back(
                std::format("tempo/key analyzed only the first {:.1f} s", request.max_musical_seconds));
    }
    if (summary->non_finite_samples > 0)
        report.warnings.push_back(std::format("{} non-finite samples treated as silence", summary->non_finite_samples));
    if (summary->reached_end && decoder.frames_exact() && known_total && *known_total != frames_decoded)
        report.warnings.push_back(
            std::format("decoder announced {} frames but produced {}", *known_total, frames_decoded));
    return report;
}

}  // namespace mso::analysis
