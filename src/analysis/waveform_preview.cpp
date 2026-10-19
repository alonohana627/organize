/// @file
/// Preview-only waveform overview: one decode pass into a WaveformBuilder.
#include "mso/analysis/waveform_preview.hpp"

#include <exception>
#include <format>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "mso/analysis/detail/decode_loop.hpp"
#include "mso/analysis/limits.hpp"
#include "mso/analysis/progress_reporter.hpp"
#include "mso/analysis/waveform_builder.hpp"
#include "mso/media/media.hpp"

namespace mso::analysis {
namespace {

/// The body of compute_waveform_overview(); code below it may throw, the caller catches.
Result<WaveformOverview> compute_waveform_overview_or_throw(const std::filesystem::path& file, int bins,
                                                            const std::stop_token& stop,
                                                            const ProgressCallback& progress) {
    if (bins < 1 || bins > kMaximumWaveformBins)
        return fail(ErrorCode::InvalidArgument, std::format("waveform bins must be in [1, {}]", kMaximumWaveformBins));
    if (stop.stop_requested()) return fail(ErrorCode::Cancelled, "waveform cancelled");

    Result<std::unique_ptr<media::Decoder>> opened = media::open_decoder(file);
    if (!opened) return std::unexpected(opened.error());
    media::Decoder& decoder = **opened;
    if (auto format = detail::validate_stream_format(decoder); !format) return std::unexpected(format.error());

    const int channels = decoder.channels();
    const std::optional<std::int64_t> reported_frames = decoder.total_frames();
    const std::optional<std::int64_t> known_total =
        reported_frames && *reported_frames > 0 ? reported_frames : std::optional<std::int64_t>{};
    WaveformBuilder builder(bins, decoder.sample_rate(), decoder.frames_exact() ? known_total : std::nullopt);
    ProgressReporter reporter(progress);

    const auto summary =
        detail::decode_blocks(decoder, stop, [&](std::span<const float> samples, std::int64_t frames_decoded) {
            builder.add(samples, channels);
            if (known_total) reporter.report(static_cast<double>(frames_decoded) / static_cast<double>(*known_total));
            return true;
        });
    if (!summary) return std::unexpected(summary.error());
    reporter.finish();
    return builder.overview(summary->reached_end);
}

}  // namespace

Result<WaveformOverview> compute_waveform_overview(const std::filesystem::path& file, int bins, std::stop_token stop,
                                                   const ProgressCallback& progress) {
    // The preview runs inside the desktop process, so nothing a malformed file provokes in a
    // decoder library may escape as an exception.
    try {
        return compute_waveform_overview_or_throw(file, bins, stop, progress);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceLimit, "out of memory while computing the waveform");
    } catch (const std::exception& error) {
        return fail(ErrorCode::Internal, std::string("waveform failed: ") + error.what());
    } catch (...) {
        return fail(ErrorCode::Internal, "waveform failed with an unknown exception");
    }
}

}  // namespace mso::analysis
