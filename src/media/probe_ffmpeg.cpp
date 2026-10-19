// FFmpeg probe fallback for containers without a native parser (only built with MSO_HAVE_FFMPEG).
#ifdef MSO_HAVE_FFMPEG

#include <algorithm>
#include <limits>

#include "mso/media/detail/ffmpeg_input.hpp"
#include "mso/media/detail/probe_formats.hpp"
#include "mso/media/detail/tag_mapping.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

namespace mso::media::detail {

namespace {

/// First name of a demuxer's comma-separated name list ("matroska,webm" -> "matroska").
std::string demuxer_short_name(const char* names) {
    const std::string_view list = names != nullptr ? names : "";
    return std::string(list.substr(0, list.find(',')));
}

/// True for codecs FFmpeg describes as lossless (bit depth is then meaningful).
bool is_lossless(AVCodecID codec_id) {
    const AVCodecDescriptor* descriptor = avcodec_descriptor_get(codec_id);
    return descriptor != nullptr && (descriptor->props & AV_CODEC_PROP_LOSSLESS);
}

/// Stores the tags of an FFmpeg metadata dictionary that map to EmbeddedTags fields. FFmpeg has
/// already decoded them to UTF-8 and leaves binary attachments out of the dictionary.
void store_metadata(const AVDictionary* metadata, EmbeddedTags& tags) {
    const AVDictionaryEntry* entry = nullptr;
    while ((entry = av_dict_get(metadata, "", entry, AV_DICT_IGNORE_SUFFIX)) != nullptr) {
        if (entry->key == nullptr || entry->value == nullptr) continue;
        if (const auto field = field_for_tag_name(entry->key)) store_tag(tags, *field, entry->value);
    }
}

/// Records layout, codec and duration from the selected stream.
void describe_stream(const FfmpegInput& input, const AVStream& stream, ProbeResult& result) {
    const AVCodecParameters* parameters = stream.codecpar;
    result.codec = avcodec_get_name(parameters->codec_id);
    if (parameters->sample_rate > 0) result.sample_rate = parameters->sample_rate;
    if (parameters->ch_layout.nb_channels > 0) result.channels = parameters->ch_layout.nb_channels;
    if (is_lossless(parameters->codec_id)) {
        const int bits =
            parameters->bits_per_raw_sample > 0 ? parameters->bits_per_raw_sample : parameters->bits_per_coded_sample;
        if (bits > 0) result.bit_depth = bits;
    }
    // Container durations are often bitrate estimates; never treat them as sample-exact.
    const AVFormatContext* context = input.context();
    if (stream.duration != AV_NOPTS_VALUE && stream.duration > 0) {
        result.duration_us = av_rescale_q(stream.duration, stream.time_base, AVRational{1, AV_TIME_BASE});
    } else if (context->duration != AV_NOPTS_VALUE && context->duration > 0) {
        result.duration_us = context->duration;
    }
    result.duration_exact = false;
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
    result.format = demuxer_short_name((*input)->context()->iformat->name);

    // Stream analysis fills durations and codec parameters the header alone does not state. It
    // reads within the same byte budget; running out leaves a partial result with what is known.
    const Result<void> analysed = (*input)->find_stream_info();
    if (!analysed && analysed.error().code != ErrorCode::ProbeLimit) return analysed;

    const AVStream* stream = (*input)->audio_stream();
    if (stream == nullptr) {
        if (!analysed) return analysed;
        return fail(ErrorCode::UnsupportedCodec, "file has no audio stream");
    }
    describe_stream(**input, *stream, result);
    store_metadata((*input)->context()->metadata, result.tags);
    store_metadata(stream->metadata, result.tags);
    return analysed;
}

}  // namespace mso::media::detail

#endif  // MSO_HAVE_FFMPEG
