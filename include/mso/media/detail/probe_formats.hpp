#pragma once
/// @file
/// Per-container metadata parsers used by probe_file(). Each fills the ProbeResult as it goes, so
/// when a read fails with ErrorCode::ProbeLimit the fields gathered so far survive as a partial
/// record. Malformed essentials fail with CorruptAudio; damaged optional metadata is skipped.
/// Parsers that tell variants apart by signature receive the bytes probe_file() already read.
///
/// The parsers are hand-written on purpose (owner rule 1 judgement, recorded in
/// IMPLEMENTATION_STATUS.md): they read through BoundedFileReader, which charges every byte against
/// the probe budget and deadline, walk tag structures with header-sized reads so cover art is never
/// loaded, and keep everything learned before a limit is hit. TagLib and the dr_libs metadata APIs
/// read whole tags or chunks into memory and cannot report a partial result.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "mso/domain/error.hpp"
#include "mso/media/detail/bounded_reader.hpp"
#include "mso/media/detail/byte_cursor.hpp"
#include "mso/media/media.hpp"

namespace mso::media::detail {

/// Upper bound for one block of tag data loaded into memory at once (a whole ID3v2 tag only when
/// it is unsynchronised as a whole, an Ogg identification packet, an ASF header object).
inline constexpr std::size_t kMaxTagBytes = 1024 * 1024;
/// Upper bound for one text item loaded into memory (an ID3 text frame, an MP4 ilst value, a RIFF
/// INFO list, an AIFF NAME chunk). Larger items are skipped or cut; stored values keep 1 KiB.
inline constexpr std::size_t kMaxTextItemBytes = 64 * 1024;
/// Upper bound on chunks/boxes/blocks visited per file, so hostile files cannot spin the probe.
inline constexpr int kMaxChunkCount = 4096;
/// Channel counts above this are treated as a corrupt header rather than a real layout.
inline constexpr std::int64_t kMaxChannels = 256;
/// Sample rates outside 1 Hz .. this bound are treated as a corrupt header.
inline constexpr std::int64_t kMaxSampleRate = 2'822'400 * 4;

/// Codec name for linear PCM ("pcm_s16", "pcm_u8", "pcm_f32", ...); nullopt for bit depths that
/// do not form a valid PCM sample.
std::optional<std::string> pcm_codec_name(std::int64_t bits_per_sample, bool is_float, bool is_unsigned);

/// Validates and records channel count and sample rate; CorruptAudio when either is impossible.
Result<void> set_stream_layout(ProbeResult& result, std::int64_t channels, std::int64_t sample_rate);

/// Converts a count of `units_per_second` ticks to microseconds without intermediate overflow;
/// nullopt when units_per_second is 0 or the result does not fit.
std::optional<std::int64_t> units_to_microseconds(std::uint64_t units, std::uint64_t units_per_second) noexcept;

/// Parses a 34-byte FLAC STREAMINFO block (native FLAC and FLAC-in-Ogg).
Result<void> parse_flac_streaminfo(Bytes streaminfo, ProbeResult& result);

/// Ogg page checksum (CRC-32, polynomial 0x04C11DB7, MSB first, initial value 0, no final xor)
/// over a complete page, with the checksum field (bytes 22-25) taken as zero.
std::uint32_t ogg_page_checksum(Bytes page) noexcept;

/// RIFF/WAVE, RF64 and BW64 (ds64), including LIST/INFO, acid, smpl and id3 chunks.
Result<void> probe_riff_wave(BoundedFileReader& reader, Bytes signature, ProbeResult& result);
/// Sony Wave64 (GUID chunk identifiers, 64-bit sizes), including its LIST/INFO chunk.
Result<void> probe_wave64(BoundedFileReader& reader, ProbeResult& result);
/// AIFF and AIFF-C, including NAME/AUTH/ANNO, MARK/INST loops and ID3 chunks.
Result<void> probe_aiff(BoundedFileReader& reader, Bytes signature, ProbeResult& result);
/// Native FLAC starting at `stream_offset` (after any leading ID3v2 tag).
Result<void> probe_flac(BoundedFileReader& reader, std::uint64_t stream_offset, ProbeResult& result);
/// MPEG-1/2/2.5 audio layers I-III; scans forward from `search_offset` for the first frame. Reads
/// trailing APEv2 and ID3v1 tags.
Result<void> probe_mpeg_audio(BoundedFileReader& reader, std::uint64_t search_offset, ProbeResult& result);
/// Raw AAC in ADTS framing starting at `stream_offset`.
Result<void> probe_adts(BoundedFileReader& reader, std::uint64_t stream_offset, ProbeResult& result);
/// Ogg Vorbis, Ogg Opus, Ogg FLAC and Ogg Speex; duration from the last page's granule position.
Result<void> probe_ogg(BoundedFileReader& reader, ProbeResult& result);
/// ISO base media / QuickTime (MP4, M4A): first sound track and the iTunes ilst tags.
Result<void> probe_iso_media(BoundedFileReader& reader, ProbeResult& result);
/// Apple Core Audio Format.
Result<void> probe_caf(BoundedFileReader& reader, ProbeResult& result);
/// WavPack (first block header) and its trailing APEv2 tag.
Result<void> probe_wavpack(BoundedFileReader& reader, ProbeResult& result);
/// ASF (WMA): file and audio stream properties; DRM-protected files are refused.
Result<void> probe_asf(BoundedFileReader& reader, ProbeResult& result);

#ifdef MSO_HAVE_FFMPEG
/// Fallback for containers without a native parser: FFmpeg's demuxer and stream analysis through
/// the bounded reader (so the same byte budget and deadline apply). Durations are never exact.
Result<void> probe_with_ffmpeg(BoundedFileReader& reader, ProbeResult& result);
#endif

}  // namespace mso::media::detail
