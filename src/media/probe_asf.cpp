// ASF (WMA) metadata probe: header objects only, never the data packets. Objects are located
// with header-sized reads and only the ones the probe needs are loaded; inside the Extended Content
// Description, binary descriptors (WM/Picture) are skipped unread.
#include <algorithm>
#include <array>
#include <format>

#include "mso/media/detail/probe_formats.hpp"
#include "mso/media/detail/tag_mapping.hpp"
#include "mso/media/detail/text_decoding.hpp"

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
/// The fixed-size objects the probe reads are a few hundred bytes; anything larger is cut here.
constexpr std::size_t kMaxPropertiesObjectBytes = 4096;
// Extended Content Description value types.
constexpr std::uint16_t kDescriptorUnicode = 0;
constexpr std::uint16_t kDescriptorDword = 3;
constexpr std::uint16_t kDescriptorQword = 4;
constexpr std::uint16_t kDescriptorWord = 5;
constexpr std::uint64_t kHundredNanosecondsPerMicrosecond = 10;
constexpr std::uint64_t kMicrosecondsPerMillisecond = 1000;

/// True when `bytes` is exactly `guid`.
bool guid_equals(Bytes bytes, const Guid& guid) {
    return bytes.size() == guid.size() && std::ranges::equal(bytes, guid);
}

/// Codec name for the WAVEFORMATEX tag of an ASF audio stream.
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
            return std::format("wave_format_0x{:04x}", format_tag);  // recognised container, unknown codec
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

/// EmbeddedTags field of a Windows Media attribute name.
std::optional<TagField> field_for_windows_media_name(std::string_view name) {
    if (name == "WM/AlbumTitle") return TagField::Album;
    if (name == "WM/Genre") return TagField::Genre;
    if (name == "WM/BeatsPerMinute") return TagField::Bpm;
    if (name == "WM/InitialKey") return TagField::Key;
    return std::nullopt;
}

/// Stores one Extended Content Description value: Unicode text, or a WORD/DWORD/QWORD (tempo).
void store_descriptor(TagField field, std::uint16_t value_type, Bytes value, EmbeddedTags& tags) {
    if (value_type == kDescriptorUnicode) {
        store_tag(tags, field, decode_text(value, TextEncoding::Utf16LittleEndian));
        return;
    }
    if (field != TagField::Bpm) return;
    ByteCursor cursor(value);
    std::uint64_t number = 0;
    if (value_type == kDescriptorWord)
        number = cursor.u16_le();
    else if (value_type == kDescriptorDword)
        number = cursor.u32_le();
    else if (value_type == kDescriptorQword)
        number = cursor.u64_le();
    else
        return;
    if (cursor.ok() && !tags.bpm_milli) tags.bpm_milli = milli_bpm_from_value(static_cast<double>(number));
}

/// Extended Content Description: named descriptors such as WM/Genre and WM/BeatsPerMinute, walked
/// in the file. Only descriptors with a wanted name are loaded; WM/Picture is skipped unread.
Result<void> read_extended_content_description(BoundedFileReader& reader, std::uint64_t offset, std::uint64_t end,
                                               EmbeddedTags& tags) {
    if (end - offset < 2) return {};
    auto count_bytes = reader.read_exact(offset, 2);
    if (!count_bytes) return std::unexpected(count_bytes.error());
    const std::uint16_t descriptor_count = ByteCursor(*count_bytes).u16_le();
    std::uint64_t position = offset + 2;
    for (std::uint16_t index = 0; index < descriptor_count && end - position >= 2; ++index) {
        auto name_length_bytes = reader.read_exact(position, 2);
        if (!name_length_bytes) return std::unexpected(name_length_bytes.error());
        const std::uint16_t name_length = ByteCursor(*name_length_bytes).u16_le();
        if (end - position < 2u + name_length + 4u) break;
        auto name_and_type = reader.read_exact(position + 2, name_length + 4u);
        if (!name_and_type) return std::unexpected(name_and_type.error());
        ByteCursor cursor(*name_and_type);
        const std::string name = decode_text(cursor.take(name_length), TextEncoding::Utf16LittleEndian);
        const std::uint16_t value_type = cursor.u16_le();
        const std::uint16_t value_length = cursor.u16_le();
        const std::uint64_t value_offset = position + 2 + name_length + 4;
        if (value_length > end - value_offset) break;
        if (const auto field = field_for_windows_media_name(name)) {
            auto value = reader.read_exact(value_offset, value_length);
            if (!value) return std::unexpected(value.error());
            store_descriptor(*field, value_type, *value, tags);
        }
        position = value_offset + value_length;
    }
    return {};
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
    const std::uint64_t header_end = std::min(header_size, reader.file_size());

    bool found_audio = false;
    std::uint64_t position = kHeaderObjectPrefix;
    for (int count = 0; count < kMaxChunkCount && header_end - position >= kObjectHeaderSize; ++count) {
        auto object_header = reader.read_exact(position, kObjectHeaderSize);
        if (!object_header) return std::unexpected(object_header.error());
        ByteCursor cursor(*object_header);
        const Bytes guid = cursor.take(16);
        const std::uint64_t object_size = cursor.u64_le();
        if (object_size < kObjectHeaderSize) break;
        const std::uint64_t payload_offset = position + kObjectHeaderSize;
        const std::uint64_t payload_end = std::min(checked_add(position, object_size).value_or(~0ull), header_end);
        const auto load_payload = [&]() {
            return reader.read_available(payload_offset, static_cast<std::size_t>(std::min<std::uint64_t>(
                                                             payload_end - payload_offset, kMaxPropertiesObjectBytes)));
        };
        if (guid_equals(guid, kContentEncryptionGuid) || guid_equals(guid, kExtendedContentEncryptionGuid)) {
            return fail(ErrorCode::UnsupportedCodec, "DRM-protected Windows Media audio is not supported");
        }
        if (guid_equals(guid, kFilePropertiesGuid)) {
            auto payload = load_payload();
            if (!payload) return std::unexpected(payload.error());
            parse_file_properties(*payload, result);
        } else if (guid_equals(guid, kStreamPropertiesGuid) && !found_audio) {
            auto payload = load_payload();
            if (!payload) return std::unexpected(payload.error());
            auto parsed = parse_stream_properties(*payload, result);
            if (!parsed) return std::unexpected(parsed.error());
            found_audio = *parsed;
        } else if (guid_equals(guid, kContentDescriptionGuid)) {
            auto payload = reader.read_available(payload_offset, static_cast<std::size_t>(std::min<std::uint64_t>(
                                                                     payload_end - payload_offset, kMaxTextItemBytes)));
            if (!payload) return std::unexpected(payload.error());
            parse_content_description(*payload, result.tags);
        } else if (guid_equals(guid, kExtendedContentDescriptionGuid)) {
            if (auto read = read_extended_content_description(reader, payload_offset, payload_end, result.tags);
                !read) {
                return read;
            }
        }
        if (payload_end <= position) break;
        position = payload_end;
    }
    if (!found_audio) return fail(ErrorCode::UnsupportedCodec, "ASF file has no audio stream");
    return {};
}

}  // namespace mso::media::detail
