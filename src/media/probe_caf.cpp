// Apple Core Audio Format (CAF) metadata probe.
#include <algorithm>
#include <bit>
#include <cmath>

#include "mso/domain/core.hpp"
#include "mso/media/probe_formats.hpp"
#include "mso/media/tag_mapping.hpp"
#include "mso/media/text_decoding.hpp"

namespace mso::media::detail {

namespace {

constexpr std::size_t kFileHeaderSize = 8;
constexpr std::size_t kChunkHeaderSize = 12;
constexpr std::size_t kDescriptionSize = 32;
constexpr std::size_t kPacketTableHeaderSize = 24;
constexpr std::uint64_t kSizeToEndOfFile = ~0ull;
constexpr std::uint32_t kLinearPcmFloatFlag = 0x1;

/// The "desc" chunk (CAFAudioDescription).
struct AudioDescription {
    double sample_rate = 0.0;
    std::string format_id;
    std::uint32_t format_flags = 0;
    std::uint32_t bytes_per_packet = 0;
    std::uint32_t channels = 0;
    std::uint32_t bits_per_channel = 0;
};

Result<AudioDescription> parse_description(Bytes payload) {
    ByteCursor cursor(payload);
    AudioDescription description;
    description.sample_rate = std::bit_cast<double>(cursor.u64_be());
    description.format_id = cursor.fourcc();
    description.format_flags = cursor.u32_be();
    description.bytes_per_packet = cursor.u32_be();
    cursor.skip(4);  // frames per packet
    description.channels = cursor.u32_be();
    description.bits_per_channel = cursor.u32_be();
    if (!cursor.ok()) return fail(ErrorCode::CorruptAudio, "CAF desc chunk is too short");
    if (!std::isfinite(description.sample_rate) || description.sample_rate < 1.0 ||
        description.sample_rate > static_cast<double>(kMaxSampleRate)) {
        return fail(ErrorCode::CorruptAudio, "invalid CAF sample rate");
    }
    return description;
}

/// ALAC stores its source bit depth in the format flags (1 = 16, 2 = 20, 3 = 24, 4 = 32 bits).
std::optional<std::int64_t> alac_bit_depth(std::uint32_t format_flags) {
    static constexpr std::int64_t kDepths[] = {16, 20, 24, 32};
    if (format_flags < 1 || format_flags > 4) return std::nullopt;
    return kDepths[format_flags - 1];
}

Result<void> apply_description(const AudioDescription& description, ProbeResult& result) {
    if (auto layout = set_stream_layout(result, description.channels, std::llround(description.sample_rate)); !layout) {
        return layout;
    }
    const std::string& id = description.format_id;
    if (id == "lpcm") {
        const bool is_float = description.format_flags & kLinearPcmFloatFlag;
        const auto codec = pcm_codec_name(description.bits_per_channel, is_float, false);
        if (!codec) return fail(ErrorCode::CorruptAudio, "unsupported CAF PCM sample size");
        result.codec = *codec;
        result.float_samples = is_float;
        result.bit_depth = description.bits_per_channel;
    } else if (id == "alac") {
        result.codec = "alac";
        result.bit_depth = alac_bit_depth(description.format_flags);
    } else if (id == "aac ") {
        result.codec = "aac";
    } else if (id == "ulaw") {
        result.codec = "pcm_mulaw";
    } else if (id == "alaw") {
        result.codec = "pcm_alaw";
    } else {
        result.codec = to_lower_ascii(trim(id));
    }
    return {};
}

/// The "info" chunk: a count followed by NUL-terminated key/value string pairs.
void parse_information(Bytes payload, EmbeddedTags& tags) {
    ByteCursor cursor(payload);
    const std::uint32_t entry_count = cursor.u32_be();
    Bytes rest = cursor.take_rest();
    const auto next_string = [&rest]() {
        const std::size_t end = find_terminator(rest, TextEncoding::Utf8);
        const std::string text = decode_legacy_text(rest.first(end));
        rest = rest.subspan(std::min(rest.size(), end + 1));
        return text;
    };
    for (std::uint32_t index = 0; index < entry_count && !rest.empty(); ++index) {
        const std::string key = next_string();
        const std::string value = next_string();
        if (const auto field = field_for_tag_name(key)) store_tag(tags, *field, value);
    }
}

}  // namespace

Result<void> probe_caf(BoundedFileReader& reader, ProbeResult& result) {
    result.format = "caf";
    std::optional<AudioDescription> description;
    std::optional<std::uint64_t> audio_bytes;
    std::optional<std::uint64_t> valid_frames;
    std::uint64_t offset = kFileHeaderSize;
    for (int count = 0; count < kMaxChunkCount; ++count) {
        if (reader.file_size() - std::min(offset, reader.file_size()) < kChunkHeaderSize) break;
        auto header = reader.read_exact(offset, kChunkHeaderSize);
        if (!header) return std::unexpected(header.error());
        ByteCursor cursor(*header);
        const std::string type = cursor.fourcc();
        std::uint64_t size = cursor.u64_be();
        const std::uint64_t payload_offset = offset + kChunkHeaderSize;
        if (size == kSizeToEndOfFile) size = reader.file_size() - payload_offset;

        if (type == "desc") {
            auto payload = reader.read_exact(payload_offset, kDescriptionSize);
            if (!payload) return std::unexpected(payload.error());
            auto parsed = parse_description(*payload);
            if (!parsed) return std::unexpected(parsed.error());
            if (auto applied = apply_description(*parsed, result); !applied) return applied;
            description = *parsed;
        } else if (type == "data") {
            const std::uint64_t present = std::min(size, reader.file_size() - payload_offset);
            audio_bytes = present > 4 ? present - 4 : 0;  // leading edit count
        } else if (type == "pakt") {
            auto payload = reader.read_exact(payload_offset, kPacketTableHeaderSize);
            if (!payload) return std::unexpected(payload.error());
            ByteCursor table(*payload);
            table.skip(8);  // packet count
            const std::uint64_t frames = table.u64_be();
            if (frames <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) valid_frames = frames;
        } else if (type == "info") {
            auto payload = reader.read_available(payload_offset,
                                                 static_cast<std::size_t>(std::min<std::uint64_t>(size, kMaxTagBytes)));
            if (!payload) return std::unexpected(payload.error());
            parse_information(*payload, result.tags);
        }
        const auto next = checked_add(payload_offset, size);
        if (!next) break;
        offset = *next;
    }
    if (!description) return fail(ErrorCode::CorruptAudio, "CAF file has no desc chunk");
    if (valid_frames) {
        result.frames = static_cast<std::int64_t>(*valid_frames);
    } else if (audio_bytes && description->format_id == "lpcm" && description->bytes_per_packet > 0) {
        result.frames = static_cast<std::int64_t>(*audio_bytes / description->bytes_per_packet);
    }
    return {};
}

}  // namespace mso::media::detail
