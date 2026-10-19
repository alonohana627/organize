#include "mso/media/container_detect.hpp"

#include <algorithm>

#include "mso/media/id3.hpp"
#include "mso/media/mpeg_audio_header.hpp"

namespace mso::media::detail {

namespace {

constexpr std::uint8_t kWave64RiffGuid[16] = {0x72, 0x69, 0x66, 0x66, 0x2E, 0x91, 0xCF, 0x11,
                                              0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00};
constexpr std::uint8_t kWave64WaveGuid[16] = {0x77, 0x61, 0x76, 0x65, 0xF3, 0xAC, 0xD3, 0x11,
                                              0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A};
constexpr std::uint8_t kAsfHeaderGuid[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                             0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};

bool has_bytes(Bytes head, std::size_t offset, const std::uint8_t (&expected)[16]) {
    return head.size() >= offset + 16 && std::ranges::equal(head.subspan(offset, 16), expected);
}

}  // namespace

ContainerKind identify_container(Bytes head) noexcept {
    if (has_magic(head, 0, "RIFF") && has_magic(head, 8, "WAVE")) return ContainerKind::RiffWave;
    if (has_magic(head, 0, "RIFF") && has_magic(head, 8, "RMID")) return ContainerKind::Midi;
    if ((has_magic(head, 0, "RF64") || has_magic(head, 0, "BW64")) && has_magic(head, 8, "WAVE")) {
        return ContainerKind::Rf64Wave;
    }
    if (has_bytes(head, 0, kWave64RiffGuid) && has_bytes(head, 24, kWave64WaveGuid)) return ContainerKind::Wave64;
    if (has_magic(head, 0, "FORM") && (has_magic(head, 8, "AIFF") || has_magic(head, 8, "AIFC"))) {
        return ContainerKind::Aiff;
    }
    if (has_magic(head, 0, "fLaC")) return ContainerKind::Flac;
    if (has_magic(head, 0, "OggS")) return ContainerKind::Ogg;
    if (has_magic(head, 4, "ftyp")) return ContainerKind::IsoMedia;
    if (has_magic(head, 0, "caff")) return ContainerKind::Caf;
    if (has_magic(head, 0, "wvpk")) return ContainerKind::WavPack;
    if (has_magic(head, 0, "MThd")) return ContainerKind::Midi;
    if (has_bytes(head, 0, kAsfHeaderGuid)) return ContainerKind::Asf;
    if (id3v2_tag_size(head)) return ContainerKind::Id3Tagged;
    if (head.size() >= 4) {
        ByteCursor cursor(head);
        const std::uint32_t word = cursor.u32_be();
        if (is_adts_header(word)) return ContainerKind::Adts;
        if (decode_mpeg_frame_header(word)) return ContainerKind::MpegAudio;
    }
    return ContainerKind::Unknown;
}

}  // namespace mso::media::detail
