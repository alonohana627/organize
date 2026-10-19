// Ogg (Vorbis, Opus, FLAC) metadata probe.
#include <algorithm>
#include <vector>

#include "mso/media/probe_formats.hpp"
#include "mso/media/vorbis_comment.hpp"

namespace mso::media::detail {

namespace {

constexpr std::size_t kPageHeaderSize = 27;
constexpr std::uint8_t kBeginningOfStream = 0x02;
constexpr std::uint64_t kNoGranulePosition = ~0ull;
/// Largest possible Ogg page: header, 255 lacing values, 255 segments of 255 bytes.
constexpr std::uint64_t kMaxPageSize = kPageHeaderSize + 255 + 255 * 255;
constexpr int kMaxHeaderPages = 512;
constexpr std::int64_t kOpusDecodeRate = 48000;

/// One parsed Ogg page header with its segment table.
struct OggPage {
    std::uint8_t header_type = 0;
    std::uint64_t granule_position = 0;
    std::uint32_t serial = 0;
    std::vector<std::uint8_t> segment_sizes;
    std::uint64_t payload_offset = 0;
    std::uint64_t payload_size = 0;
};

Result<OggPage> read_page(BoundedFileReader& reader, std::uint64_t offset) {
    auto header = reader.read_exact(offset, kPageHeaderSize);
    if (!header) return std::unexpected(header.error());
    ByteCursor cursor(*header);
    if (cursor.fourcc() != "OggS" || cursor.u8() != 0) return fail(ErrorCode::CorruptAudio, "invalid Ogg page header");
    OggPage page;
    page.header_type = cursor.u8();
    page.granule_position = cursor.u64_le();
    page.serial = cursor.u32_le();
    cursor.skip(8);  // sequence number, checksum
    const std::uint8_t segment_count = cursor.u8();
    auto lacing = reader.read_exact(offset + kPageHeaderSize, segment_count);
    if (!lacing) return std::unexpected(lacing.error());
    page.segment_sizes = std::move(*lacing);
    page.payload_offset = offset + kPageHeaderSize + segment_count;
    for (const std::uint8_t size : page.segment_sizes)
        page.payload_size += size;
    return page;
}

/// The first packets of the logical stream that starts the file.
struct HeaderPackets {
    std::uint32_t serial = 0;
    std::vector<std::vector<std::uint8_t>> packets;
};

/// Reassembles up to `wanted` packets from the first logical stream. A packet larger than
/// kMaxTagBytes (embedded cover art) is cut there and counted as complete.
Result<HeaderPackets> read_header_packets(BoundedFileReader& reader, std::size_t wanted) {
    HeaderPackets result;
    std::vector<std::uint8_t> current;
    std::uint64_t offset = 0;
    bool first_page = true;
    for (int page_count = 0; page_count < kMaxHeaderPages && result.packets.size() < wanted; ++page_count) {
        if (offset >= reader.file_size()) break;
        auto page = read_page(reader, offset);
        if (!page) return std::unexpected(page.error());
        if (first_page) {
            if (!(page->header_type & kBeginningOfStream))
                return fail(ErrorCode::CorruptAudio, "Ogg stream has no start page");
            result.serial = page->serial;
            first_page = false;
        }
        offset = page->payload_offset + page->payload_size;
        if (page->serial != result.serial) continue;  // another multiplexed stream
        auto payload = reader.read_exact(page->payload_offset, static_cast<std::size_t>(page->payload_size));
        if (!payload) return std::unexpected(payload.error());
        std::size_t position = 0;
        for (const std::uint8_t segment_size : page->segment_sizes) {
            const std::size_t room = kMaxTagBytes - std::min(kMaxTagBytes, current.size());
            const auto segment = Bytes(*payload).subspan(position, segment_size);
            current.insert(current.end(), segment.begin(),
                           segment.begin() + std::min<std::size_t>(room, segment.size()));
            position += segment_size;
            if (segment_size < 255 || current.size() >= kMaxTagBytes) {
                result.packets.push_back(std::move(current));
                current.clear();
                if (result.packets.size() == wanted) break;
            }
        }
    }
    return result;
}

/// Granule position of the last page of `serial` found in the file tail; nullopt when the
/// tail window holds none. `budget_limited` reports a window shrunk by the probe budget.
Result<std::optional<std::uint64_t>> last_granule_position(BoundedFileReader& reader, std::uint32_t serial,
                                                           bool& budget_limited) {
    const std::uint64_t wanted = std::min(reader.file_size(), kMaxPageSize);
    const std::uint64_t window = std::min(wanted, reader.remaining_budget());
    budget_limited = window < wanted;
    if (window < kPageHeaderSize) return std::optional<std::uint64_t>{};
    auto tail = reader.read_exact(reader.file_size() - window, static_cast<std::size_t>(window));
    if (!tail) return std::unexpected(tail.error());
    const Bytes bytes(*tail);
    for (std::size_t position = bytes.size() - kPageHeaderSize + 1; position-- > 0;) {
        if (!has_magic(bytes, position, "OggS") || bytes[position + 4] != 0) continue;
        ByteCursor cursor(bytes.subspan(position + 6));
        const std::uint64_t granule = cursor.u64_le();
        const std::uint32_t page_serial = cursor.u32_le();
        if (page_serial == serial && granule != kNoGranulePosition) return std::optional<std::uint64_t>{granule};
    }
    return std::optional<std::uint64_t>{};
}

/// Recognised first packets ("codec identification headers").
enum class OggCodec { Vorbis, Opus, Flac, Speex, Unknown };

OggCodec identify_codec(Bytes packet) {
    if (packet.size() >= 7 && packet[0] == 0x01 && has_magic(packet, 1, "vorbis")) return OggCodec::Vorbis;
    if (has_magic(packet, 0, "OpusHead")) return OggCodec::Opus;
    if (packet.size() >= 5 && packet[0] == 0x7F && has_magic(packet, 1, "FLAC")) return OggCodec::Flac;
    if (has_magic(packet, 0, "Speex   ")) return OggCodec::Speex;
    return OggCodec::Unknown;
}

/// Reads the identification header; returns the pre-skip (Opus) to subtract from granules.
Result<std::uint64_t> parse_identification(OggCodec codec, Bytes packet, ProbeResult& result) {
    ByteCursor cursor(packet);
    switch (codec) {
        case OggCodec::Vorbis: {
            result.format = "ogg";
            result.codec = "vorbis";
            cursor.skip(7 + 4);  // packet type, "vorbis", version
            const std::int64_t channels = cursor.u8();
            const std::int64_t sample_rate = cursor.u32_le();
            if (!cursor.ok()) return fail(ErrorCode::CorruptAudio, "Vorbis identification header is too short");
            if (auto layout = set_stream_layout(result, channels, sample_rate); !layout)
                return std::unexpected(layout.error());
            return 0;
        }
        case OggCodec::Opus: {
            result.format = "opus";
            result.codec = "opus";
            cursor.skip(8 + 1);  // "OpusHead", version
            const std::int64_t channels = cursor.u8();
            const std::uint16_t pre_skip = cursor.u16_le();
            if (!cursor.ok()) return fail(ErrorCode::CorruptAudio, "Opus identification header is too short");
            // Opus always decodes at 48 kHz; the header's input rate is informational only.
            if (auto layout = set_stream_layout(result, channels, kOpusDecodeRate); !layout)
                return std::unexpected(layout.error());
            return pre_skip;
        }
        case OggCodec::Flac: {
            result.format = "ogg";
            constexpr std::size_t kStreamInfoOffset = 9 + 4 + 4;  // mapping header, "fLaC", block header
            if (packet.size() < kStreamInfoOffset) return fail(ErrorCode::CorruptAudio, "Ogg FLAC header is too short");
            if (auto parsed = parse_flac_streaminfo(packet.subspan(kStreamInfoOffset), result); !parsed) {
                return std::unexpected(parsed.error());
            }
            return 0;
        }
        case OggCodec::Speex:
            result.format = "ogg";
            result.codec = "speex";
            return 0;
        case OggCodec::Unknown:
            break;
    }
    return fail(ErrorCode::UnsupportedCodec, "unrecognised codec in Ogg stream");
}

void parse_comment_packet(OggCodec codec, Bytes packet, EmbeddedTags& tags) {
    if (codec == OggCodec::Vorbis && packet.size() >= 7 && packet[0] == 0x03 && has_magic(packet, 1, "vorbis")) {
        parse_vorbis_comment(packet.subspan(7), tags);
    } else if (codec == OggCodec::Opus && has_magic(packet, 0, "OpusTags")) {
        parse_vorbis_comment(packet.subspan(8), tags);
    } else if (codec == OggCodec::Flac && packet.size() >= 4 && (packet[0] & 0x7F) == 4) {
        parse_vorbis_comment(packet.subspan(4), tags);
    }
}

}  // namespace

Result<void> probe_ogg(BoundedFileReader& reader, ProbeResult& result) {
    result.format = "ogg";
    auto headers = read_header_packets(reader, 2);
    if (!headers) return std::unexpected(headers.error());
    if (headers->packets.empty()) return fail(ErrorCode::CorruptAudio, "Ogg stream has no packets");

    const OggCodec codec = identify_codec(headers->packets[0]);
    const auto pre_skip = parse_identification(codec, headers->packets[0], result);
    if (!pre_skip) return std::unexpected(pre_skip.error());
    if (headers->packets.size() > 1) parse_comment_packet(codec, headers->packets[1], result.tags);
    if (codec == OggCodec::Speex) return {};

    bool budget_limited = false;
    const auto granule = last_granule_position(reader, headers->serial, budget_limited);
    if (!granule) return std::unexpected(granule.error());
    if (*granule && **granule >= *pre_skip) {
        result.frames = static_cast<std::int64_t>(**granule - *pre_skip);
    } else if (budget_limited) {
        result.partial = true;
    }
    return {};
}

}  // namespace mso::media::detail
