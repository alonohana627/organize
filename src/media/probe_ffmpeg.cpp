// FFmpeg probe fallback for containers without a native parser (only built with MSO_HAVE_FFMPEG).
#ifdef MSO_HAVE_FFMPEG

#include <algorithm>
#include <limits>

#include "mso/media/ffmpeg_input.hpp"
#include "mso/media/probe_formats.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace mso::media::detail {

namespace {

/// First name of a demuxer's comma-separated name list ("matroska,webm" -> "matroska").
std::string demuxer_short_name(const char* names) {
    const std::string_view list = names != nullptr ? names : "";
    return std::string(list.substr(0, list.find(',')));
}

bool is_lossless(AVCodecID codec_id) {
    const AVCodecDescriptor* descriptor = avcodec_descriptor_get(codec_id);
    return descriptor != nullptr && (descriptor->props & AV_CODEC_PROP_LOSSLESS);
}

}  // namespace

Result<void> probe_with_ffmpeg(BoundedFileReader& reader, ProbeResult& result) {
    const auto probe_bytes = static_cast<std::int64_t>(
        std::min<std::uint64_t>(reader.remaining_budget(), std::numeric_limits<std::int64_t>::max()));
    auto input = FfmpegInput::open(
        [&reader](std::uint64_t offset, std::span<std::uint8_t> destination) {
            return reader.read_into(offset, destination);
        },
        reader.file_size(), probe_bytes);
    if (!input) return std::unexpected(input.error());

    const AVStream* stream = (*input)->audio_stream();
    if (stream == nullptr) return fail(ErrorCode::UnsupportedCodec, "file has no audio stream");
    const AVCodecParameters* parameters = stream->codecpar;
    result.format = demuxer_short_name((*input)->context()->iformat->name);
    result.codec = avcodec_get_name(parameters->codec_id);
    if (parameters->sample_rate > 0) result.sample_rate = parameters->sample_rate;
    if (parameters->ch_layout.nb_channels > 0) result.channels = parameters->ch_layout.nb_channels;
    if (is_lossless(parameters->codec_id)) {
        const int bits =
            parameters->bits_per_raw_sample > 0 ? parameters->bits_per_raw_sample : parameters->bits_per_coded_sample;
        if (bits > 0) result.bit_depth = bits;
    }
    // Container durations are often bitrate estimates; never treat them as sample-exact.
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        result.duration_us = av_rescale_q(stream->duration, stream->time_base, AVRational{1, AV_TIME_BASE});
    } else if ((*input)->context()->duration != AV_NOPTS_VALUE && (*input)->context()->duration > 0) {
        result.duration_us = (*input)->context()->duration;
    }
    result.duration_exact = false;
    return {};
}

}  // namespace mso::media::detail

#endif  // MSO_HAVE_FFMPEG
