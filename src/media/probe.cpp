// probe_file(): container identification and dispatch to the per-format parsers.
#include <algorithm>

#include "mso/domain/core.hpp"
#include "mso/media/bounded_reader.hpp"
#include "mso/media/container_detect.hpp"
#include "mso/media/id3.hpp"
#include "mso/media/media.hpp"
#include "mso/media/probe_formats.hpp"

namespace mso::media {

namespace {

using detail::BoundedFileReader;
using detail::ContainerKind;

constexpr std::size_t kMaxLoopPoints = 64;

#ifdef MSO_HAVE_FFMPEG
/// Containers only FFmpeg understands. Enumeration does not import them by default, but an
/// explicit probe of such a file may use the FFmpeg fallback.
constexpr std::string_view kFfmpegOnlyExtensions[] = {
    "mka", "ape", "tta", "ac3", "eac3", "dts", "mpc", "tak", "au", "snd", "dsf", "amr", "shn", "voc",
};

/// FFmpeg probing is only tried when the extension claims audio: its permissive demuxers could
/// otherwise mistake arbitrary data for a low-confidence MP3 or AC-3 stream.
bool extension_allows_ffmpeg_fallback(std::string_view extension) {
    return is_candidate_extension(extension) ||
           std::ranges::find(kFfmpegOnlyExtensions, extension) != std::ranges::end(kFfmpegOnlyExtensions);
}
#endif

/// Lowercase extension without the dot, used only as a hint.
std::string extension_hint(const std::filesystem::path& file) {
    std::string extension = to_lower_ascii(path_to_utf8(file.extension()));
    if (!extension.empty() && extension.front() == '.') extension.erase(0, 1);
    return extension;
}

/// Turns a parser's CorruptAudio into UnsupportedCodec: used where only the extension suggested
/// the format, so failing to find it means "not audio", not "damaged audio".
Result<void> as_unrecognised(Result<void> outcome) {
    if (!outcome && outcome.error().code == ErrorCode::CorruptAudio) {
        return fail(ErrorCode::UnsupportedCodec, "not a recognised audio file");
    }
    return outcome;
}

/// Last resort for data without a recognised signature.
Result<void> probe_unrecognised(BoundedFileReader& reader, std::string_view extension, ProbeResult& result) {
    if (extension == "mp3" || extension == "mp2" || extension == "mpga") {
        // MP3 files may start with junk before the first frame; the hint justifies a sync scan.
        auto outcome = as_unrecognised(detail::probe_mpeg_audio(reader, 0, result));
        if (outcome || outcome.error().code != ErrorCode::UnsupportedCodec) return outcome;
    }
#ifdef MSO_HAVE_FFMPEG
    if (extension_allows_ffmpeg_fallback(extension)) {
        result = ProbeResult{};
        return detail::probe_with_ffmpeg(reader, result);
    }
#endif
    return fail(ErrorCode::UnsupportedCodec, "not a recognised audio file");
}

/// A leading ID3v2 tag: read its frames, then identify the stream that follows it.
Result<void> probe_after_id3(BoundedFileReader& reader, detail::Bytes head, ProbeResult& result) {
    const std::uint64_t tag_size = *detail::id3v2_tag_size(head);
    const auto tag_bytes = static_cast<std::size_t>(std::min<std::uint64_t>(tag_size, detail::kMaxTagBytes));
    auto tag = reader.read_within_budget(0, tag_bytes);
    if (!tag) return std::unexpected(tag.error());
    detail::parse_id3v2(*tag, result.tags);
    if (tag_size >= reader.file_size()) return fail(ErrorCode::CorruptAudio, "file holds an ID3 tag but no audio");

    auto stream_head = reader.read_available(tag_size, detail::kSignatureBytes);
    if (!stream_head) return std::unexpected(stream_head.error());
    switch (detail::identify_container(*stream_head)) {
        case ContainerKind::Flac:
            return detail::probe_flac(reader, tag_size, result);
        case ContainerKind::Adts:
            return detail::probe_adts(reader, tag_size, result);
        default:
            return detail::probe_mpeg_audio(reader, tag_size, result);
    }
}

/// Identifies the container from its signature and runs the matching parser.
Result<void> probe_container(BoundedFileReader& reader, std::string_view extension, ProbeResult& result) {
    auto head = reader.read_within_budget(0, detail::kSignatureBytes);
    if (!head) return std::unexpected(head.error());
    switch (detail::identify_container(*head)) {
        case ContainerKind::RiffWave:
        case ContainerKind::Rf64Wave:
            return detail::probe_riff_wave(reader, *head, result);
        case ContainerKind::Wave64:
            return detail::probe_wave64(reader, result);
        case ContainerKind::Aiff:
            return detail::probe_aiff(reader, *head, result);
        case ContainerKind::Flac:
            return detail::probe_flac(reader, 0, result);
        case ContainerKind::MpegAudio:
            return detail::probe_mpeg_audio(reader, 0, result);
        case ContainerKind::Adts:
            return detail::probe_adts(reader, 0, result);
        case ContainerKind::Ogg:
            return detail::probe_ogg(reader, result);
        case ContainerKind::IsoMedia:
            return detail::probe_iso_media(reader, result);
        case ContainerKind::Caf:
            return detail::probe_caf(reader, result);
        case ContainerKind::WavPack:
            return detail::probe_wavpack(reader, result);
        case ContainerKind::Asf:
            return detail::probe_asf(reader, result);
        case ContainerKind::Id3Tagged:
            return probe_after_id3(reader, *head, result);
        case ContainerKind::Midi:
            return fail(ErrorCode::UnsupportedCodec, "MIDI files contain no audio");
        case ContainerKind::Unknown:
            break;
    }
    if (head->size() < detail::kSignatureBytes && head->size() < reader.file_size()) {
        return fail(ErrorCode::ProbeLimit, "probe budget too small to identify the container");
    }
    return probe_unrecognised(reader, extension, result);
}

/// Derived fields: exact duration from exact frames, and loops that fit the stream.
void finalize(ProbeResult& result) {
    // `frames` is only ever set from an exact source count, so it decides exactness.
    if (result.frames && result.sample_rate && *result.sample_rate > 0 && *result.frames >= 0) {
        result.duration_us = detail::units_to_microseconds(static_cast<std::uint64_t>(*result.frames),
                                                           static_cast<std::uint64_t>(*result.sample_rate));
    }
    result.duration_exact = result.frames.has_value() && result.duration_us.has_value();
    std::erase_if(result.loops, [&result](const LoopPoint& loop) {
        const bool ordered = loop.start_frame >= 0 && loop.start_frame < loop.end_frame;
        const bool inside = !result.frames || loop.end_frame <= *result.frames;
        return !ordered || !inside;
    });
    if (result.loops.size() > kMaxLoopPoints) result.loops.resize(kMaxLoopPoints);
}

}  // namespace

Result<ProbeResult> probe_file(const std::filesystem::path& file, const ProbeLimits& limits) {
    auto reader = BoundedFileReader::open(file, limits);
    if (!reader) return std::unexpected(reader.error());
    if (reader->file_size() == 0) return fail(ErrorCode::CorruptAudio, "file is empty");

    ProbeResult result;
    const Result<void> outcome = probe_container(*reader, extension_hint(file), result);
    if (!outcome) {
        if (outcome.error().code != ErrorCode::ProbeLimit) return std::unexpected(outcome.error());
        result.partial = true;  // keep what was learned; unknown fields stay empty
    }
    finalize(result);
    result.bytes_read = reader->bytes_read();
    return result;
}

}  // namespace mso::media
