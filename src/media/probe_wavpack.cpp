// WavPack metadata probe (first block header and its metadata sub-blocks) and its trailing tags.
#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "mso/media/detail/ape_tag.hpp"
#include "mso/media/detail/id3.hpp"
#include "mso/media/detail/probe_formats.hpp"

namespace mso::media::detail {

namespace {

constexpr std::size_t kBlockHeaderSize = 32;
constexpr std::uint32_t kBytesPerSampleMask = 0x3;
constexpr std::uint32_t kMonoFlag = 0x4;
constexpr std::uint32_t kHybridFlag = 0x8;
constexpr std::uint32_t kFloatFlag = 0x80;
constexpr std::uint32_t kInitialBlockFlag = 0x800;
constexpr std::uint32_t kFinalBlockFlag = 0x1000;
constexpr std::uint32_t kDsdFlag = 0x80000000;  // DSD audio: the stated rate is not a PCM rate
constexpr int kSampleRateShift = 23;
constexpr std::uint32_t kSampleRateMask = 0xF;
constexpr std::int64_t kSampleRates[15] = {6000,  8000,  9600,  11025, 12000, 16000, 22050, 24000,
                                           32000, 44100, 48000, 64000, 88200, 96000, 192000};

// Metadata sub-block identifiers (wavpack.h).
constexpr std::uint8_t kSubBlockFunctionMask = 0x3F;  // ID_UNIQUE
constexpr std::uint8_t kSubBlockOddSize = 0x40;       // ID_ODD_SIZE: the last byte is padding
constexpr std::uint8_t kSubBlockLarge = 0x80;         // ID_LARGE: 24-bit size
constexpr std::uint8_t kSubBlockBitstream = 0x0A;     // ID_WV_BITSTREAM: audio data, no metadata after it
constexpr std::uint8_t kSubBlockChannelInfo = 0x0D;   // ID_CHANNEL_INFO
constexpr std::uint8_t kSubBlockSampleRate = 0x27;    // ID_SAMPLE_RATE (an ID_OPTIONAL_DATA item)
/// Metadata sub-blocks precede the audio in the first block and are small; this much is enough.
constexpr std::size_t kMaxMetadataBytes = 4096;

/// Layout facts from the metadata sub-blocks of the first block.
struct BlockMetadata {
    std::optional<std::int64_t> channels;     ///< from ID_CHANNEL_INFO (multichannel files)
    std::optional<std::int64_t> sample_rate;  ///< from ID_SAMPLE_RATE (rates outside the table)
};

/// Walks the sub-blocks after the block header up to the audio bitstream. The payloads follow
/// read_channel_info() and read_sample_rate() in WavPack's open_utils.c.
BlockMetadata parse_metadata_sub_blocks(Bytes sub_blocks) {
    BlockMetadata metadata;
    ByteCursor cursor(sub_blocks);
    while (cursor.has(2)) {
        const std::uint8_t identifier = cursor.u8();
        std::uint64_t length = cursor.u8();
        if (identifier & kSubBlockLarge) length |= static_cast<std::uint64_t>(cursor.u16_le()) << 8;
        length *= 2;  // sizes count 16-bit words
        const bool odd_size = (identifier & kSubBlockOddSize) != 0 && length > 0;
        if (odd_size) --length;
        const std::uint8_t function = identifier & kSubBlockFunctionMask;
        if (!cursor.ok() || function == kSubBlockBitstream) break;
        const Bytes payload = cursor.take(length);
        if (!cursor.ok()) break;  // runs past what was read: keep what was found
        if (odd_size) cursor.skip(1);
        if (function == kSubBlockChannelInfo && !payload.empty()) {
            // Six or more bytes: the WavPack 5 layout, a 12-bit channel count minus one.
            const std::int64_t channels =
                payload.size() >= 6 ? (payload[0] | ((payload[2] & 0xF) << 8)) + 1 : payload[0];
            if (channels >= 1 && channels <= kMaxChannels) metadata.channels = channels;
        } else if (function == kSubBlockSampleRate && (payload.size() == 3 || payload.size() == 4)) {
            std::int64_t rate = payload[0] | (payload[1] << 8) | (payload[2] << 16);
            if (payload.size() == 4) rate |= static_cast<std::int64_t>(payload[3] & 0x7F) << 24;
            if (rate >= 1 && rate <= kMaxSampleRate) metadata.sample_rate = rate;
        }
    }
    return metadata;
}

}  // namespace

Result<void> probe_wavpack(BoundedFileReader& reader, ProbeResult& result) {
    result.format = "wv";
    result.codec = "wavpack";
    auto header = reader.read_exact(0, kBlockHeaderSize);
    if (!header) return std::unexpected(header.error());
    ByteCursor cursor(*header);
    cursor.skip(4);                                    // "wvpk"
    const std::uint32_t block_size = cursor.u32_le();  // bytes after this field
    cursor.skip(2);                                    // version
    cursor.skip(1);                                    // block index (upper 8 bits)
    const std::uint64_t total_samples_high = cursor.u8();
    const std::uint32_t total_samples_low = cursor.u32_le();
    cursor.skip(4 + 4);  // block index, block samples
    const std::uint32_t flags = cursor.u32_le();

    const std::uint32_t rate_index = (flags >> kSampleRateShift) & kSampleRateMask;
    if (rate_index < std::size(kSampleRates)) result.sample_rate = kSampleRates[rate_index];  // 15: custom rate
    // A single block that is both initial and final describes the whole (mono or stereo) layout;
    // multichannel files spread channels over several blocks and state the count in metadata.
    if ((flags & kInitialBlockFlag) && (flags & kFinalBlockFlag)) result.channels = (flags & kMonoFlag) ? 1 : 2;
    const bool is_float = flags & kFloatFlag;
    result.float_samples = is_float;
    if (!(flags & kHybridFlag))
        result.bit_depth = is_float ? 32 : static_cast<std::int64_t>(((flags & kBytesPerSampleMask) + 1) * 8);
    // WavPack 5 extends the 32-bit count with an 8-bit high part, defined (GET_TOTAL_SAMPLES in
    // wavpack.h) as low + (high << 32) - high; a low part of all ones means "unknown".
    if (total_samples_low != 0xFFFFFFFFu) {
        result.frames = static_cast<std::int64_t>(total_samples_low + (total_samples_high << 32) - total_samples_high);
    }

    // Custom sample rates and multichannel layouts are stated in sub-blocks before the audio.
    const std::uint64_t block_end = std::min<std::uint64_t>(std::uint64_t{block_size} + 8, reader.file_size());
    if (block_end > kBlockHeaderSize && (flags & kDsdFlag) == 0) {
        const auto length =
            static_cast<std::size_t>(std::min<std::uint64_t>(block_end - kBlockHeaderSize, kMaxMetadataBytes));
        auto sub_blocks = reader.read_within_budget(kBlockHeaderSize, length);
        if (!sub_blocks) return std::unexpected(sub_blocks.error());
        const BlockMetadata metadata = parse_metadata_sub_blocks(*sub_blocks);
        if (metadata.channels) result.channels = metadata.channels;
        if (rate_index >= std::size(kSampleRates) && metadata.sample_rate) result.sample_rate = metadata.sample_rate;
    }

    // Tags: APEv2 at the end of the file, possibly followed by an ID3v1 tag (APE values win).
    std::uint64_t tag_end = reader.file_size();
    std::optional<std::vector<std::uint8_t>> id3v1_tail;
    if (reader.file_size() > kBlockHeaderSize + kId3v1TagSize) {
        auto tail = reader.read_within_budget(reader.file_size() - kId3v1TagSize, kId3v1TagSize);
        if (!tail) return std::unexpected(tail.error());
        if (tail->size() == kId3v1TagSize && has_magic(*tail, 0, "TAG")) {
            tag_end -= kId3v1TagSize;
            id3v1_tail = std::move(*tail);
        }
    }
    auto ape = read_ape_tag(reader, tag_end, result.tags);
    if (!ape) return std::unexpected(ape.error());
    if (id3v1_tail) parse_id3v1(*id3v1_tail, result.tags);
    return {};
}

}  // namespace mso::media::detail
