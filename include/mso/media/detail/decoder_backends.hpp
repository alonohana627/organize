#pragma once
/// @file
/// Decoder backends behind open_decoder(). Each takes the bounded probe result that identified
/// the container, so backends never guess the format from the extension.
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "mso/domain/error.hpp"
#include "mso/media/media.hpp"

namespace mso::media::detail {

/// miniaudio's built-in decoders: dr_wav (WAV, RF64, Wave64, AIFF, AIFF-C), dr_flac and dr_mp3.
Result<std::unique_ptr<Decoder>> open_miniaudio_decoder(const std::filesystem::path& file, const ProbeResult& probe);
/// True when the probed stream is one miniaudio decodes.
bool miniaudio_can_decode(const ProbeResult& probe);
/// Codec names the miniaudio backend decodes.
std::vector<std::string> miniaudio_codecs();

/// Ogg Vorbis through stb_vorbis in pull mode (exact length, page-based seeking). Files over 2 GiB
/// are refused with UnsupportedCodec (stb_vorbis is 32-bit); open_decoder() then tries FFmpeg.
Result<std::unique_ptr<Decoder>> open_vorbis_decoder(const std::filesystem::path& file, const ProbeResult& probe);

#ifdef MSO_HAVE_FFMPEG
/// FFmpeg (libavformat/libavcodec/libswresample) for AAC, Opus, Vorbis, ALAC, WMA, WavPack and
/// others. The output layout and rate are fixed by the first decoded frame.
Result<std::unique_ptr<Decoder>> open_ffmpeg_decoder(const std::filesystem::path& file, const ProbeResult& probe);
/// Codec names the linked FFmpeg build can decode, from a fixed list of audio codecs we route to it.
std::vector<std::string> ffmpeg_codecs();
#endif

}  // namespace mso::media::detail
