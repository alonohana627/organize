// MPEG audio (MP3/MP2/MP1) and ADTS AAC metadata probe.
#include <algorithm>
#include <optional>
#include <vector>

#include "mso/media/detail/ape_tag.hpp"
#include "mso/media/detail/id3.hpp"
#include "mso/media/detail/mpeg_audio_header.hpp"
#include "mso/media/detail/probe_formats.hpp"

namespace mso::media::detail {

namespace {

constexpr std::size_t kSyncSearchBytes = 64 * 1024;
constexpr std::uint32_t kXingHasFrames = 0x1;
constexpr std::uint32_t kXingHasBytes = 0x2;
constexpr std::uint32_t kXingHasToc = 0x4;
constexpr std::uint32_t kXingHasQuality = 0x8;
constexpr std::size_t kXingTocBytes = 100;
constexpr std::size_t kLameDelayOffset = 21;  // encoder string(9), revision, lowpass, replay gain(8), flags, bitrate
constexpr std::size_t kVbriOffset = 4 + 32;   // fixed position after the header in the first frame
constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;
constexpr std::uint32_t kDecoderDelayFrames = 528 + 1;  // MPEG layer III decoder delay (LAME's definition)

/// The big-endian 32-bit word at `offset` (0 when the span is too short).
std::uint32_t header_word_at(Bytes bytes, std::size_t offset) {
    ByteCursor cursor(bytes.subspan(offset));
    return cursor.u32_be();
}

/// A frame header found in the file.
struct LocatedFrame {
    std::uint64_t offset = 0;
    MpegFrameHeader header;
    std::vector<std::uint8_t> frame_bytes;  // the first frame itself (where VBR headers live)
};

/// Frame count and gapless trim from a Xing/Info/VBRI header in the first frame.
struct VbrHeader {
    std::optional<std::uint64_t> frame_count;
    std::uint64_t trim_frames = 0;  // frames a gapless decoder removes (see parse_xing_header)
};

/// Xing ("VBR") or Info ("CBR") header, optionally followed by a LAME tag with gapless trim.
std::optional<VbrHeader> parse_xing_header(Bytes frame, const MpegFrameHeader& header) {
    const std::size_t offset = 4 + (header.has_crc ? 2 : 0) + header.side_info_bytes();
    if (!has_magic(frame, offset, "Xing") && !has_magic(frame, offset, "Info")) return std::nullopt;
    ByteCursor cursor(frame.subspan(offset + 4));
    const std::uint32_t flags = cursor.u32_be();
    VbrHeader vbr;
    if (flags & kXingHasFrames) vbr.frame_count = cursor.u32_be();
    if (flags & kXingHasBytes) cursor.skip(4);
    if (flags & kXingHasToc) cursor.skip(kXingTocBytes);
    if (flags & kXingHasQuality) cursor.skip(4);
    if (!cursor.ok()) return std::nullopt;
    const Bytes lame_tag = cursor.take(kLameDelayOffset + 3);
    const bool has_encoder_name =
        cursor.ok() && std::all_of(lame_tag.begin(), lame_tag.begin() + 4, [](std::uint8_t byte) {
            return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
        });
    if (has_encoder_name) {
        ByteCursor trim(lame_tag.subspan(kLameDelayOffset));
        const std::uint32_t packed = trim.u24_be();
        // Encoder delay + end padding. The decoder's own delay (528 + 1 frames) is removed from the
        // start, so at least that many frames go at the end too: when the stated padding is
        // shorter, those last frames never leave the decoder (dr_mp3 clamps the same way), and the
        // count must match what a decoder delivers to be exact.
        const std::uint32_t encoder_delay = packed >> 12;
        const std::uint32_t end_padding = packed & 0xFFF;
        vbr.trim_frames = encoder_delay + std::max(end_padding, kDecoderDelayFrames);
    }
    return vbr;
}

/// Fraunhofer VBRI header.
std::optional<VbrHeader> parse_vbri_header(Bytes frame) {
    if (!has_magic(frame, kVbriOffset, "VBRI")) return std::nullopt;
    ByteCursor cursor(frame.subspan(kVbriOffset + 4));
    cursor.skip(2 + 2 + 2 + 4);  // version, delay, quality, stream bytes
    const std::uint32_t frame_count = cursor.u32_be();
    if (!cursor.ok()) return std::nullopt;
    return VbrHeader{frame_count, 0};
}

/// Confirms a candidate by checking that a frame of the same stream follows it (or the file ends).
Result<bool> successor_confirms(BoundedFileReader& reader, Bytes window, std::uint64_t window_offset,
                                std::size_t position, const MpegFrameHeader& header) {
    const std::uint64_t next = window_offset + position + header.frame_bytes;
    if (next + 4 > reader.file_size()) return next <= reader.file_size();  // last frame of the file
    std::uint32_t next_word = 0;
    if (position + header.frame_bytes + 4 <= window.size()) {
        next_word = header_word_at(window, position + header.frame_bytes);
    } else {
        auto bytes = reader.read_exact(next, 4);
        if (!bytes) return std::unexpected(bytes.error());
        next_word = header_word_at(*bytes, 0);
    }
    const auto next_header = decode_mpeg_frame_header(next_word);
    return next_header && next_header->same_stream_as(header);
}

/// Scans forward from `search_offset` for the first confirmed MPEG audio frame.
Result<std::optional<LocatedFrame>> find_first_frame(BoundedFileReader& reader, std::uint64_t search_offset) {
    auto window = reader.read_within_budget(search_offset, kSyncSearchBytes);
    if (!window) return std::unexpected(window.error());
    const Bytes bytes(*window);
    for (std::size_t position = 0; position + 4 <= bytes.size(); ++position) {
        if (bytes[position] != 0xFF) continue;
        const auto header = decode_mpeg_frame_header(header_word_at(bytes, position));
        if (!header) continue;
        const auto confirmed = successor_confirms(reader, bytes, search_offset, position, *header);
        if (!confirmed) return std::unexpected(confirmed.error());
        if (!*confirmed) continue;
        const std::size_t frame_length = std::min<std::size_t>(header->frame_bytes, bytes.size() - position);
        const auto frame = bytes.subspan(position, frame_length);
        return LocatedFrame{search_offset + position, *header, {frame.begin(), frame.end()}};
    }
    if (bytes.size() < kSyncSearchBytes && search_offset + bytes.size() < reader.file_size()) {
        return fail(ErrorCode::ProbeLimit, "probe byte budget reached while searching for an MPEG frame");
    }
    return std::optional<LocatedFrame>{};
}

/// Codec name of an MPEG audio layer.
std::string_view layer_codec(int layer) noexcept {
    if (layer == 1) return "mp1";
    if (layer == 2) return "mp2";
    return "mp3";
}

/// Reads the tags at the end of the stream: an APEv2 tag and/or an ID3v1 tag (APE values win over
/// ID3v1, whose fields are cut at 30 bytes). Returns where the audio data ends. Running out of
/// budget marks the result partial instead of failing the probe.
std::uint64_t read_trailing_tags(BoundedFileReader& reader, std::uint64_t audio_start, ProbeResult& result) {
    std::uint64_t audio_end = reader.file_size();
    std::optional<std::vector<std::uint8_t>> id3v1_tail;
    if (reader.file_size() >= audio_start + kId3v1TagSize) {
        auto tail = reader.read_within_budget(reader.file_size() - kId3v1TagSize, kId3v1TagSize);
        if (!tail || tail->size() != kId3v1TagSize) {
            result.partial = true;
            return audio_end;
        }
        if (has_magic(*tail, 0, "TAG")) {
            audio_end -= kId3v1TagSize;
            id3v1_tail = std::move(*tail);
        }
    }
    auto ape = read_ape_tag(reader, audio_end, result.tags);
    if (!ape) {
        result.partial = true;
    } else if (*ape && **ape >= audio_start) {
        audio_end = **ape;
    }
    if (id3v1_tail) parse_id3v1(*id3v1_tail, result.tags);
    return audio_end;
}

}  // namespace

Result<void> probe_mpeg_audio(BoundedFileReader& reader, std::uint64_t search_offset, ProbeResult& result) {
    auto located = find_first_frame(reader, search_offset);
    if (!located) return std::unexpected(located.error());
    if (!*located) return fail(ErrorCode::CorruptAudio, "no MPEG audio frame found");
    const LocatedFrame& first = **located;
    const MpegFrameHeader& header = first.header;

    result.format = "mp3";
    result.codec = std::string(layer_codec(header.layer));
    if (auto layout = set_stream_layout(result, header.channels, header.sample_rate); !layout) return layout;

    auto vbr = header.layer == 3 ? parse_xing_header(first.frame_bytes, header) : std::nullopt;
    if (!vbr) vbr = parse_vbri_header(first.frame_bytes);
    const std::uint64_t audio_end = read_trailing_tags(reader, first.offset, result);

    if (vbr && vbr->frame_count) {
        const std::uint64_t encoded = *vbr->frame_count * header.samples_per_frame;  // 32-bit count: no overflow
        result.frames = static_cast<std::int64_t>(encoded > vbr->trim_frames ? encoded - vbr->trim_frames : encoded);
        return {};
    }
    // No frame count: estimate from the first frame's bitrate. Never exact (VBR, junk, tags).
    // duration [us] = bytes * 8 [bits] / (kbps * 1000 [bits/s]) * 1e6 = bytes * 8000 / kbps.
    const std::uint64_t audio_bytes = audio_end > first.offset ? audio_end - first.offset : 0;
    if (const auto bits_times_thousand = checked_multiply(audio_bytes, 8 * kMicrosecondsPerSecond / 1000)) {
        result.duration_us =
            static_cast<std::int64_t>(*bits_times_thousand / static_cast<std::uint64_t>(header.bitrate_kbps));
    }
    result.duration_exact = false;
    return {};
}

Result<void> probe_adts(BoundedFileReader& reader, std::uint64_t stream_offset, ProbeResult& result) {
    static constexpr std::int64_t kAdtsSampleRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                                          22050, 16000, 12000, 11025, 8000,  7350};
    result.format = "aac";
    result.codec = "aac";
    auto header = reader.read_exact(stream_offset, 7);
    if (!header) return std::unexpected(header.error());
    ByteCursor cursor(*header);
    cursor.skip(2);  // sync, version, layer, protection
    const std::uint8_t profile_rate_channel = cursor.u8();
    const std::uint8_t channel_low = cursor.u8();
    const std::size_t rate_index = (profile_rate_channel >> 2) & 0xF;
    const int channel_configuration = ((profile_rate_channel & 0x1) << 2) | (channel_low >> 6);
    if (rate_index >= std::size(kAdtsSampleRates)) return fail(ErrorCode::CorruptAudio, "invalid ADTS sample rate");
    if (channel_configuration == 0) {
        result.sample_rate = kAdtsSampleRates[rate_index];  // layout is carried in-band
        return {};
    }
    const std::int64_t channels = channel_configuration == 7 ? 8 : channel_configuration;
    return set_stream_layout(result, channels, kAdtsSampleRates[rate_index]);
}

}  // namespace mso::media::detail
