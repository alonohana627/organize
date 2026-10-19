// Native FLAC metadata probe.

#include "mso/media/detail/probe_formats.hpp"
#include "mso/media/detail/vorbis_comment.hpp"

namespace mso::media::detail {

namespace {

constexpr std::uint8_t kBlockStreamInfo = 0;
constexpr std::uint8_t kBlockVorbisComment = 4;
constexpr std::uint8_t kBlockInvalid = 127;
constexpr std::uint8_t kLastBlockFlag = 0x80;
constexpr std::size_t kBlockHeaderSize = 4;
constexpr std::size_t kStreamInfoSize = 34;

}  // namespace

Result<void> probe_flac(BoundedFileReader& reader, std::uint64_t stream_offset, ProbeResult& result) {
    result.format = "flac";
    result.codec = "flac";
    bool has_stream_info = false;
    std::uint64_t offset = stream_offset + 4;  // after "fLaC"
    for (int count = 0; count < kMaxChunkCount; ++count) {
        auto header = reader.read_exact(offset, kBlockHeaderSize);
        if (!header) return std::unexpected(header.error());
        ByteCursor cursor(*header);
        const std::uint8_t flags = cursor.u8();
        const std::uint32_t length = cursor.u24_be();
        const std::uint8_t type = flags & 0x7F;
        const std::uint64_t payload_offset = offset + kBlockHeaderSize;
        if (type == kBlockInvalid) return fail(ErrorCode::CorruptAudio, "invalid FLAC metadata block");
        if (type == kBlockStreamInfo) {
            auto streaminfo = reader.read_exact(payload_offset, kStreamInfoSize);
            if (!streaminfo) return std::unexpected(streaminfo.error());
            if (auto parsed = parse_flac_streaminfo(*streaminfo, result); !parsed) return parsed;
            has_stream_info = true;
        } else if (type == kBlockVorbisComment) {
            // Streamed entry by entry: oversized entries (embedded pictures) are skipped unread.
            FileRangeCommentSource comments(reader, payload_offset, length);
            if (auto read = read_vorbis_comment(comments, result.tags); !read) return read;
        }
        if (flags & kLastBlockFlag) break;
        offset = payload_offset + length;  // 24-bit lengths cannot overflow
    }
    if (!has_stream_info) return fail(ErrorCode::CorruptAudio, "FLAC stream has no STREAMINFO block");
    return {};
}

}  // namespace mso::media::detail
