// Ogg (Vorbis, Opus, FLAC, Speex) metadata probe.
#include <algorithm>
#include <array>
#include <vector>

#include "mso/media/detail/probe_formats.hpp"
#include "mso/media/detail/vorbis_comment.hpp"

namespace mso::media::detail {

namespace {

constexpr std::size_t kPageHeaderSize = 27;
constexpr std::size_t kChecksumOffset = 22;
constexpr std::uint8_t kBeginningOfStream = 0x02;
constexpr std::uint64_t kNoGranulePosition = ~0ull;
/// Largest possible Ogg page: header, 255 lacing values, 255 segments of 255 bytes.
constexpr std::uint64_t kMaxPageSize = kPageHeaderSize + 255 + 255 * 255;
/// Pages visited while looking for the header packets (other multiplexed streams are skipped).
constexpr int kMaxHeaderPages = 512;
/// Identification headers are tiny (Vorbis 30 bytes, Opus at most 276, FLAC 51, Speex 80).
constexpr std::size_t kMaxIdentificationBytes = 512;
constexpr std::int64_t kOpusDecodeRate = 48000;
constexpr std::uint32_t kOggChecksumPolynomial = 0x04C11DB7;

/// CRC lookup table for the Ogg page checksum (polynomial 0x04C11DB7, MSB first).
constexpr std::array<std::uint32_t, 256> make_checksum_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < 256; ++index) {
        std::uint32_t remainder = index << 24;
        for (int bit = 0; bit < 8; ++bit) {
            remainder = (remainder & 0x80000000u) ? (remainder << 1) ^ kOggChecksumPolynomial : remainder << 1;
        }
        table[index] = remainder;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kChecksumTable = make_checksum_table();

/// Reads the packets of the logical stream that starts the file, page by page. Only the bytes
/// asked for are read: skipped packet data (cover art in a comment packet) costs nothing but the
/// page headers. Implements CommentByteSource over the current packet.
class OggPacketStream final : public CommentByteSource {
public:
    explicit OggPacketStream(BoundedFileReader& reader) noexcept : reader_(reader) {}

    /// Serial number of the stream being read (valid once a page has been loaded).
    std::uint32_t serial() const noexcept { return serial_; }

    /// Reads up to `count` bytes of the current packet; fewer only at the packet's end.
    Result<std::vector<std::uint8_t>> read(std::size_t count) override {
        std::vector<std::uint8_t> bytes;
        while (bytes.size() < count) {
            auto available = ensure_segment();
            if (!available) return std::unexpected(available.error());
            if (!*available) break;
            const std::size_t segment_remaining = segments_[segment_index_] - offset_in_segment_;
            const std::size_t take = std::min(count - bytes.size(), segment_remaining);
            auto chunk = reader_.read_exact(segment_file_offset_ + offset_in_segment_, take);
            if (!chunk) return std::unexpected(chunk.error());
            bytes.insert(bytes.end(), chunk->begin(), chunk->end());
            offset_in_segment_ += take;
        }
        return bytes;
    }

    /// Skips up to `count` bytes of the current packet without reading them.
    Result<bool> skip(std::uint64_t count) override {
        while (count > 0) {
            auto available = ensure_segment();
            if (!available) return std::unexpected(available.error());
            if (!*available) return false;
            const std::size_t segment_remaining = segments_[segment_index_] - offset_in_segment_;
            const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(count, segment_remaining));
            offset_in_segment_ += take;
            count -= take;
        }
        return true;
    }

    /// Moves to the start of the next packet; false when the stream has no further packet.
    Result<bool> next_packet() {
        while (true) {
            auto skipped = skip(~0ull);
            if (!skipped) return std::unexpected(skipped.error());
            if (packet_ended_) break;
        }
        if (segment_index_ >= segments_.size()) return false;  // the stream itself ended
        // Step over the segment that ended the packet.
        segment_file_offset_ += segments_[segment_index_];
        ++segment_index_;
        offset_in_segment_ = 0;
        packet_ended_ = false;
        return true;
    }

private:
    /// Makes sure unread bytes of the current packet are available in the current segment,
    /// loading the next page when a packet continues across pages; false at the packet's end.
    Result<bool> ensure_segment() {
        while (!packet_ended_) {
            if (segment_index_ < segments_.size()) {
                const std::uint8_t size = segments_[segment_index_];
                if (offset_in_segment_ < size) return true;
                if (size < 255) {
                    packet_ended_ = true;  // a lacing value below 255 ends the packet
                    break;
                }
                segment_file_offset_ += size;
                ++segment_index_;
                offset_in_segment_ = 0;
                continue;
            }
            auto loaded = load_next_page();
            if (!loaded) return std::unexpected(loaded.error());
            if (!*loaded) {
                packet_ended_ = true;  // file ends inside the packet: parse what was read
                break;
            }
        }
        return false;
    }

    /// Loads the next page of the stream's serial; false when the file holds no further page.
    Result<bool> load_next_page() {
        for (; pages_visited_ < kMaxHeaderPages; ++pages_visited_) {
            if (reader_.file_size() - std::min(next_page_offset_, reader_.file_size()) < kPageHeaderSize) return false;
            auto header = reader_.read_exact(next_page_offset_, kPageHeaderSize);
            if (!header) return std::unexpected(header.error());
            ByteCursor cursor(*header);
            if (cursor.fourcc() != "OggS" || cursor.u8() != 0) {
                return fail(ErrorCode::CorruptAudio, "invalid Ogg page header");
            }
            const std::uint8_t header_type = cursor.u8();
            cursor.skip(8);  // granule position
            const std::uint32_t serial = cursor.u32_le();
            cursor.skip(8);  // sequence number, checksum
            const std::uint8_t segment_count = cursor.u8();
            auto lacing = reader_.read_exact(next_page_offset_ + kPageHeaderSize, segment_count);
            if (!lacing) return std::unexpected(lacing.error());
            const std::uint64_t payload_offset = next_page_offset_ + kPageHeaderSize + segment_count;
            std::uint64_t payload_size = 0;
            for (const std::uint8_t size : *lacing)
                payload_size += size;
            if (!have_serial_) {
                if (!(header_type & kBeginningOfStream)) {
                    return fail(ErrorCode::CorruptAudio, "Ogg stream has no start page");
                }
                serial_ = serial;
                have_serial_ = true;
            }
            next_page_offset_ = payload_offset + payload_size;
            if (serial != serial_) continue;  // another multiplexed stream
            segments_ = std::move(*lacing);
            segment_index_ = 0;
            offset_in_segment_ = 0;
            segment_file_offset_ = payload_offset;
            ++pages_visited_;
            return true;
        }
        return false;
    }

    BoundedFileReader& reader_;
    std::uint64_t next_page_offset_ = 0;
    bool have_serial_ = false;
    std::uint32_t serial_ = 0;
    std::vector<std::uint8_t> segments_;     ///< lacing values of the current page
    std::size_t segment_index_ = 0;          ///< current segment within the page
    std::size_t offset_in_segment_ = 0;      ///< bytes of the current segment already consumed
    std::uint64_t segment_file_offset_ = 0;  ///< file offset of the current segment's first byte
    bool packet_ended_ = false;
    int pages_visited_ = 0;
};

/// True when `bytes` holds, at `position`, a complete Ogg page whose checksum verifies.
bool is_valid_page_at(Bytes bytes, std::size_t position) {
    if (!has_magic(bytes, position, "OggS") || bytes.size() - position < kPageHeaderSize || bytes[position + 4] != 0) {
        return false;
    }
    const std::uint8_t segment_count = bytes[position + kPageHeaderSize - 1];
    const std::size_t lacing_end = position + kPageHeaderSize + segment_count;
    if (lacing_end > bytes.size()) return false;
    std::size_t page_size = kPageHeaderSize + segment_count;
    for (std::size_t index = position + kPageHeaderSize; index < lacing_end; ++index)
        page_size += bytes[index];
    if (page_size > bytes.size() - position) return false;  // the page does not fit: not a real page
    const Bytes page = bytes.subspan(position, page_size);
    ByteCursor stored(page.subspan(kChecksumOffset, 4));
    return stored.u32_le() == ogg_page_checksum(page);
}

/// Granule position of the last valid page of `serial` in the file tail; nullopt when the tail
/// window holds none. `budget_limited` reports a window shrunk by the probe budget. Candidate
/// pages must fit in the window and pass the checksum, so an "OggS" pattern inside packet data
/// is never mistaken for a page.
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
        if (!is_valid_page_at(bytes, position)) continue;
        ByteCursor cursor(bytes.subspan(position + 6));
        const std::uint64_t granule = cursor.u64_le();
        const std::uint32_t page_serial = cursor.u32_le();
        if (page_serial == serial && granule != kNoGranulePosition) return std::optional<std::uint64_t>{granule};
    }
    return std::optional<std::uint64_t>{};
}

/// Recognised first packets ("codec identification headers").
enum class OggCodec { Vorbis, Opus, Flac, Speex, Unknown };

/// Identifies the codec from the stream's first packet.
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
        case OggCodec::Speex: {
            result.format = "ogg";
            result.codec = "speex";
            cursor.skip(8 + 20 + 4 + 4);  // "Speex   ", version string, version id, header size
            const std::int64_t sample_rate = cursor.u32_le();
            cursor.skip(4 + 4);  // mode, mode bitstream version
            const std::int64_t channels = cursor.u32_le();
            if (cursor.ok()) {
                if (auto layout = set_stream_layout(result, channels, sample_rate); !layout)
                    return std::unexpected(layout.error());
            }
            return 0;
        }
        case OggCodec::Unknown:
            break;
    }
    return fail(ErrorCode::UnsupportedCodec, "unrecognised codec in Ogg stream");
}

/// Reads the comment packet the cursor is positioned on: checks the codec's packet prefix, then
/// streams the Vorbis comment block (cover art entries are skipped unread).
Result<void> read_comment_packet(OggCodec codec, OggPacketStream& stream, EmbeddedTags& tags) {
    std::size_t prefix_size = 0;
    switch (codec) {
        case OggCodec::Vorbis:
            prefix_size = 7;  // 0x03 "vorbis"
            break;
        case OggCodec::Opus:
            prefix_size = 8;  // "OpusTags"
            break;
        case OggCodec::Flac:
            prefix_size = 4;  // metadata block header of type VORBIS_COMMENT
            break;
        case OggCodec::Speex:
        case OggCodec::Unknown:
            prefix_size = 0;  // Speex comments carry no prefix
            break;
    }
    auto prefix = stream.read(prefix_size);
    if (!prefix) return std::unexpected(prefix.error());
    const Bytes bytes(*prefix);
    const bool expected_prefix =
        (codec == OggCodec::Vorbis && bytes.size() == 7 && bytes[0] == 0x03 && has_magic(bytes, 1, "vorbis")) ||
        (codec == OggCodec::Opus && has_magic(bytes, 0, "OpusTags")) ||
        (codec == OggCodec::Flac && bytes.size() == 4 && (bytes[0] & 0x7F) == 4) || codec == OggCodec::Speex;
    if (!expected_prefix) return {};  // not a comment packet: no tags
    return read_vorbis_comment(stream, tags);
}

}  // namespace

std::uint32_t ogg_page_checksum(Bytes page) noexcept {
    std::uint32_t checksum = 0;
    for (std::size_t index = 0; index < page.size(); ++index) {
        const bool in_checksum_field = index >= kChecksumOffset && index < kChecksumOffset + 4;
        const std::uint8_t byte = in_checksum_field ? 0 : page[index];
        checksum = (checksum << 8) ^ kChecksumTable[((checksum >> 24) ^ byte) & 0xFF];
    }
    return checksum;
}

Result<void> probe_ogg(BoundedFileReader& reader, ProbeResult& result) {
    result.format = "ogg";
    OggPacketStream stream(reader);
    auto identification = stream.read(kMaxIdentificationBytes);
    if (!identification) return std::unexpected(identification.error());
    if (identification->empty()) return fail(ErrorCode::CorruptAudio, "Ogg stream has no packets");

    const OggCodec codec = identify_codec(*identification);
    const auto pre_skip = parse_identification(codec, *identification, result);
    if (!pre_skip) return std::unexpected(pre_skip.error());
    auto has_comments = stream.next_packet();
    if (!has_comments) return std::unexpected(has_comments.error());
    if (*has_comments) {
        if (auto comments = read_comment_packet(codec, stream, result.tags); !comments) return comments;
    }
    if (codec == OggCodec::Speex) return {};

    bool budget_limited = false;
    const auto granule = last_granule_position(reader, stream.serial(), budget_limited);
    if (!granule) return std::unexpected(granule.error());
    if (*granule && **granule >= *pre_skip) {
        result.frames = static_cast<std::int64_t>(**granule - *pre_skip);
    } else if (budget_limited) {
        result.partial = true;
    }
    return {};
}

}  // namespace mso::media::detail
