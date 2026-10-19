// ASF (WMA) metadata probe: header object only, never the data packets.
#include <algorithm>
#include <array>
#include <cstring>

#include "mso/media/probe_formats.hpp"
#include "mso/media/tag_mapping.hpp"
#include "mso/media/text_decoding.hpp"

namespace mso::media::detail {

namespace {

using Guid = std::array<std::uint8_t, 16>;

// Object GUIDs in their on-disk (little-endian field) byte order.
constexpr Guid kFilePropertiesGuid = {0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
                                      0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr Guid kStreamPropertiesGuid = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11,
                                        0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr Guid kAudioMediaGuid = {0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11,
                                  0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};
constexpr Guid kContentDescriptionGuid = {0x33, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                          0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr Guid kExtendedContentDescriptionGuid = {0x40, 0xA4, 0xD0, 0xD2, 0x07, 0xE3, 0xD2, 0x11,
                                                  0x97, 0xF0, 0x00, 0xA0, 0xC9, 0x5E, 0xA8, 0x50};
constexpr Guid kContentEncryptionGuid = {0xFB, 0xB3, 0x11, 0x22, 0x23, 0xBD, 0xD2, 0x11,
                                         0xB4, 0xB7, 0x00, 0xA0, 0xC9, 0x55, 0xFC, 0x6E};
constexpr Guid kExtendedContentEncryptionGuid = {0x14, 0xE6, 0x8A, 0x29, 0x22, 0x26, 0x17, 0x4C,
                                                 0xB9, 0x35, 0xDA, 0xE0, 0x7E, 0xE9, 0x28, 0x9C};

constexpr std::size_t kHeaderObjectPrefix = 30;  // GUID, size, object count, two reserved bytes
constexpr std::size_t kObjectHeaderSize = 24;    // GUID, size
constexpr std::uint16_t kDescriptorUnicode = 0;
constexpr std::uint64_t kHundredNanosecondsPerMicrosecond = 10;
constexpr std::uint64_t kMicrosecondsPerMillisecond = 1000;

bool guid_equals(Bytes bytes, const Guid& guid) {
    return bytes.size() == guid.size() && std::ranges::equal(bytes, guid);
}

std::string codec_for_wave_format_tag(std::uint16_t format_tag) {
    switch (format_tag) {
        case 0x0160:
        case 0x0161:
            return "wma";
        case 0x0162:
            return "wmapro";
        case 0x0163:
            return "wmalossless";
        case 0x000A:
            return "wmavoice";
        case 0x0055:
            return "mp3";
        case 0x0001:
            return "pcm";
        default:
            return "wma";
    }
}

/// File Properties: play duration (100 ns units) minus the preroll (milliseconds).
void parse_file_properties(Bytes payload, ProbeResult& result) {
    ByteCursor cursor(payload);
    cursor.skip(16 + 8 + 8 + 8);  // file id, file size, creation date, packet count
    const std::uint64_t play_duration = cursor.u64_le();
    cursor.skip(8);  // send duration
    const std::uint64_t preroll_ms = cursor.u64_le();
    if (!cursor.ok()) return;
    const std::uint64_t play_us = play_duration / kHundredNanosecondsPerMicrosecond;
    const auto preroll_us = checked_multiply(preroll_ms, kMicrosecondsPerMillisecond);
    if (preroll_us && play_us > *preroll_us) {
        result.duration_us = static_cast<std::int64_t>(play_us - *preroll_us);
        result.duration_exact = false;
    }
}

/// Stream Properties of the first audio stream: a WAVEFORMATEX describing the codec.
Result<bool> parse_stream_properties(Bytes payload, ProbeResult& result) {
    ByteCursor cursor(payload);
    if (!guid_equals(cursor.take(16), kAudioMediaGuid)) return false;
    cursor.skip(16 + 8);  // error correction type, time offset
    const std::uint32_t format_length = cursor.u32_le();
    cursor.skip(4 + 2 + 4);  // error correction length, flags, reserved
    ByteCursor format(cursor.take(format_length));
    const std::uint16_t format_tag = format.u16_le();
    const std::int64_t channels = format.u16_le();
    const std::int64_t sample_rate = format.u32_le();
    format.skip(4 + 2);  // average bytes per second, block align
    const std::int64_t bits_per_sample = format.u16_le();
    if (!format.ok()) return fail(ErrorCode::CorruptAudio, "ASF audio stream properties are too short");
    if (auto layout = set_stream_layout(result, channels, sample_rate); !layout) return std::unexpected(layout.error());
    result.codec = codec_for_wave_format_tag(format_tag);
    if (result.codec == "wmalossless" || result.codec == "pcm") result.bit_depth = bits_per_sample;
    return true;
}

/// Content Description: fixed title/author/copyright/description/rating UTF-16 strings.
void parse_content_description(Bytes payload, EmbeddedTags& tags) {
    ByteCursor cursor(payload);
    std::uint16_t lengths[5] = {};
    for (auto& length : lengths)
        length = cursor.u16_le();
    const Bytes title = cursor.take(lengths[0]);
    const Bytes author = cursor.take(lengths[1]);
    cursor.skip(lengths[2]);  // copyright
    const Bytes description = cursor.take(lengths[3]);
    if (!cursor.ok()) return;
    store_tag(tags, TagField::Title, decode_text(title, TextEncoding::Utf16LittleEndian));
    store_tag(tags, TagField::Artist, decode_text(author, TextEncoding::Utf16LittleEndian));
    store_tag(tags, TagField::Comment, decode_text(description, TextEncoding::Utf16LittleEndian));
}

std::optional<TagField> field_for_windows_media_name(std::string_view name) {
    if (name == "WM/AlbumTitle") return TagField::Album;
    if (name == "WM/Genre") return TagField::Genre;
    if (name == "WM/BeatsPerMinute") return TagField::Bpm;
    if (name == "WM/InitialKey") return TagField::Key;
    return std::nullopt;
}

/// Extended Content Description: named descriptors such as WM/Genre and WM/BeatsPerMinute.
void parse_extended_content_description(Bytes payload, EmbeddedTags& tags) {
    ByteCursor cursor(payload);
    const std::uint16_t descriptor_count = cursor.u16_le();
    for (std::uint16_t index = 0; index < descriptor_count && cursor.has(6); ++index) {
        const std::string name = decode_text(cursor.take(cursor.u16_le()), TextEncoding::Utf16LittleEndian);
        const std::uint16_t value_type = cursor.u16_le();
        const Bytes value = cursor.take(cursor.u16_le());
        if (!cursor.ok()) return;
        const auto field = field_for_windows_media_name(name);
        if (field && value_type == kDescriptorUnicode) {
            store_tag(tags, *field, decode_text(value, TextEncoding::Utf16LittleEndian));
        }
    }
}

}  // namespace

Result<void> probe_asf(BoundedFileReader& reader, ProbeResult& result) {
    result.format = "wma";
    result.codec = "wma";
    auto prefix = reader.read_exact(0, kHeaderObjectPrefix);
    if (!prefix) return std::unexpected(prefix.error());
    ByteCursor prefix_cursor(*prefix);
    prefix_cursor.skip(16);
    const std::uint64_t header_size = prefix_cursor.u64_le();
    if (header_size < kHeaderObjectPrefix) return fail(ErrorCode::CorruptAudio, "invalid ASF header object size");
    const auto body_size =
        static_cast<std::size_t>(std::min<std::uint64_t>(header_size - kHeaderObjectPrefix, kMaxTagBytes));
    auto body = reader.read_available(kHeaderObjectPrefix, body_size);
    if (!body) return std::unexpected(body.error());

    ByteCursor cursor(*body);
    bool found_audio = false;
    while (cursor.has(kObjectHeaderSize)) {
        const Bytes guid = cursor.take(16);
        const std::uint64_t object_size = cursor.u64_le();
        if (object_size < kObjectHeaderSize) break;
        const Bytes payload = cursor.take(std::min<std::uint64_t>(object_size - kObjectHeaderSize, cursor.remaining()));
        if (guid_equals(guid, kContentEncryptionGuid) || guid_equals(guid, kExtendedContentEncryptionGuid)) {
            return fail(ErrorCode::UnsupportedCodec, "DRM-protected Windows Media audio is not supported");
        }
        if (guid_equals(guid, kFilePropertiesGuid)) {
            parse_file_properties(payload, result);
        } else if (guid_equals(guid, kStreamPropertiesGuid) && !found_audio) {
            auto parsed = parse_stream_properties(payload, result);
            if (!parsed) return std::unexpected(parsed.error());
            found_audio = *parsed;
        } else if (guid_equals(guid, kContentDescriptionGuid)) {
            parse_content_description(payload, result.tags);
        } else if (guid_equals(guid, kExtendedContentDescriptionGuid)) {
            parse_extended_content_description(payload, result.tags);
        }
    }
    if (!found_audio) return fail(ErrorCode::UnsupportedCodec, "ASF file has no audio stream");
    return {};
}

}  // namespace mso::media::detail
