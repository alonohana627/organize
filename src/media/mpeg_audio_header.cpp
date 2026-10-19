#include "mso/media/mpeg_audio_header.hpp"

namespace mso::media::detail {

namespace {

// Bitrates in kbit/s by [MPEG-1 or not][layer - 1][index]; index 0 (free format) and 15 are invalid.
constexpr int kBitrates[2][3][15] = {
    {
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
    },
    {
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
    },
};

constexpr int kMpeg1SampleRates[3] = {44100, 48000, 32000};
constexpr int kChannelModeMono = 3;

}  // namespace

std::uint32_t MpegFrameHeader::side_info_bytes() const noexcept {
    if (version == Version::Mpeg1) return channels == 1 ? 17 : 32;
    return channels == 1 ? 9 : 17;
}

bool MpegFrameHeader::same_stream_as(const MpegFrameHeader& other) const noexcept {
    return version == other.version && layer == other.layer && sample_rate == other.sample_rate;
}

std::optional<MpegFrameHeader> decode_mpeg_frame_header(std::uint32_t header_word) noexcept {
    if ((header_word & 0xFFE00000u) != 0xFFE00000u) return std::nullopt;
    const std::uint32_t version_bits = (header_word >> 19) & 0x3;
    const std::uint32_t layer_bits = (header_word >> 17) & 0x3;
    const std::uint32_t bitrate_index = (header_word >> 12) & 0xF;
    const std::uint32_t rate_index = (header_word >> 10) & 0x3;
    if (version_bits == 1 || layer_bits == 0 || bitrate_index == 0 || bitrate_index == 15 || rate_index == 3) {
        return std::nullopt;
    }
    MpegFrameHeader header;
    header.version = version_bits == 3   ? MpegFrameHeader::Version::Mpeg1
                     : version_bits == 2 ? MpegFrameHeader::Version::Mpeg2
                                         : MpegFrameHeader::Version::Mpeg25;
    header.layer = 4 - static_cast<int>(layer_bits);
    const bool is_mpeg1 = header.version == MpegFrameHeader::Version::Mpeg1;
    header.bitrate_kbps = kBitrates[is_mpeg1 ? 0 : 1][header.layer - 1][bitrate_index];
    const int rate_divisor = is_mpeg1 ? 1 : (header.version == MpegFrameHeader::Version::Mpeg2 ? 2 : 4);
    header.sample_rate = kMpeg1SampleRates[rate_index] / rate_divisor;
    header.has_crc = ((header_word >> 16) & 0x1) == 0;
    header.channels = ((header_word >> 6) & 0x3) == kChannelModeMono ? 1 : 2;
    const std::uint32_t padding = (header_word >> 9) & 0x1;
    const auto bitrate = static_cast<std::uint32_t>(header.bitrate_kbps) * 1000;
    const auto rate = static_cast<std::uint32_t>(header.sample_rate);
    if (header.layer == 1) {
        header.samples_per_frame = 384;
        header.frame_bytes = (12 * bitrate / rate + padding) * 4;
    } else {
        header.samples_per_frame = header.layer == 3 && !is_mpeg1 ? 576 : 1152;
        header.frame_bytes = header.samples_per_frame / 8 * bitrate / rate + padding;
    }
    return header;
}

bool is_adts_header(std::uint32_t header_word) noexcept {
    return (header_word & 0xFFF60000u) == 0xFFF00000u;
}

}  // namespace mso::media::detail
