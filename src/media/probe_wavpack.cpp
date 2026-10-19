// WavPack metadata probe (first block header) and its trailing APEv2 tag.
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
constexpr int kSampleRateShift = 23;
constexpr std::uint32_t kSampleRateMask = 0xF;
constexpr std::int64_t kSampleRates[15] = {6000,  8000,  9600,  11025, 12000, 16000, 22050, 24000,
                                           32000, 44100, 48000, 64000, 88200, 96000, 192000};

}  // namespace

Result<void> probe_wavpack(BoundedFileReader& reader, ProbeResult& result) {
    result.format = "wv";
    result.codec = "wavpack";
    auto header = reader.read_exact(0, kBlockHeaderSize);
    if (!header) return std::unexpected(header.error());
    ByteCursor cursor(*header);
    cursor.skip(4 + 4 + 2);  // "wvpk", block size, version
    cursor.skip(1);          // block index (upper 8 bits)
    const std::uint64_t total_samples_high = cursor.u8();
    const std::uint32_t total_samples_low = cursor.u32_le();
    cursor.skip(4 + 4);  // block index, block samples
    const std::uint32_t flags = cursor.u32_le();

    const std::uint32_t rate_index = (flags >> kSampleRateShift) & kSampleRateMask;
    if (rate_index < std::size(kSampleRates)) result.sample_rate = kSampleRates[rate_index];  // 15: custom rate
    // A single block that is both initial and final describes the whole (mono or stereo) layout;
    // multichannel files spread channels over several blocks and need metadata sub-blocks.
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

    // Tags: APEv2 at the end of the file, possibly followed by an ID3v1 tag.
    std::uint64_t tag_end = reader.file_size();
    if (reader.file_size() > kBlockHeaderSize + kId3v1TagSize) {
        auto tail = reader.read_within_budget(reader.file_size() - kId3v1TagSize, kId3v1TagSize);
        if (!tail) return std::unexpected(tail.error());
        if (has_magic(*tail, 0, "TAG")) tag_end -= kId3v1TagSize;
    }
    auto ape = read_ape_tag(reader, tag_end, result.tags);
    if (!ape) return std::unexpected(ape.error());
    return {};
}

}  // namespace mso::media::detail
