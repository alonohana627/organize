#pragma once
/// @file
/// MPEG-1/2/2.5 audio frame headers (layers I-III), shared by the MP3 probe and container sniffing.
#include <cstdint>
#include <optional>

namespace mso::media::detail {

/// A decoded 4-byte MPEG audio frame header.
struct MpegFrameHeader {
    enum class Version { Mpeg1, Mpeg2, Mpeg25 };

    Version version = Version::Mpeg1;
    int layer = 3;
    int bitrate_kbps = 0;
    int sample_rate = 0;
    int channels = 2;
    bool has_crc = false;
    std::uint32_t frame_bytes = 0;
    std::uint32_t samples_per_frame = 0;

    /// Bytes of layer III side information after the header (where Xing/Info tags start).
    std::uint32_t side_info_bytes() const noexcept;
    /// True when `other` belongs to the same stream (version, layer and sample rate agree).
    bool same_stream_as(const MpegFrameHeader& other) const noexcept;
};

/// Decodes a big-endian header word; nullopt for anything that is not a valid frame header
/// (bad sync, reserved fields, free-format bitrate).
std::optional<MpegFrameHeader> decode_mpeg_frame_header(std::uint32_t header_word) noexcept;

/// True when the word is an ADTS (raw AAC) header: 12-bit sync and layer 0.
bool is_adts_header(std::uint32_t header_word) noexcept;

}  // namespace mso::media::detail
