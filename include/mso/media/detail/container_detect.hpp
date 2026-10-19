#pragma once
/// @file
/// Identifies the actual container from its leading bytes; the file extension is only a hint.
#include <cstddef>

#include "mso/media/detail/byte_cursor.hpp"

namespace mso::media::detail {

/// Containers the probe recognises by signature.
enum class ContainerKind {
    Unknown,
    RiffWave,   ///< "RIFF....WAVE"
    Rf64Wave,   ///< "RF64....WAVE" / "BW64....WAVE"
    Wave64,     ///< Sony Wave64 GUID header
    Aiff,       ///< "FORM....AIFF" / "FORM....AIFC"
    Flac,       ///< "fLaC"
    MpegAudio,  ///< MPEG audio frame sync at offset 0
    Adts,       ///< ADTS AAC frame sync at offset 0
    Ogg,        ///< "OggS"
    IsoMedia,   ///< "....ftyp" (MP4, M4A, MOV)
    Caf,        ///< "caff"
    WavPack,    ///< "wvpk"
    Asf,        ///< ASF header object GUID (WMA)
    Id3Tagged,  ///< ID3v2 tag in front of an MPEG, ADTS or FLAC stream
    Midi,       ///< Standard MIDI file or RIFF MIDI: not audio
};

/// Number of leading bytes identify_container() looks at.
inline constexpr std::size_t kSignatureBytes = 64;

/// Classifies the first bytes of a file (fewer than kSignatureBytes is fine).
ContainerKind identify_container(Bytes head) noexcept;

}  // namespace mso::media::detail
