// open_decoder() and decoder_capabilities(): route a probed stream to the backend that decodes it.
#include <algorithm>
#include <format>

#include "mso/media/detail/decoder_backends.hpp"
#include "mso/media/media.hpp"

namespace mso::media {

Result<std::unique_ptr<Decoder>> open_decoder(const std::filesystem::path& file) {
    // A bounded probe identifies the real container and codec; the extension plays no part.
    auto probe = probe_file(file);
    if (!probe) return std::unexpected(probe.error());
    if (probe->codec.empty()) return fail(ErrorCode::UnsupportedCodec, "could not identify the audio codec");

    if (detail::miniaudio_can_decode(*probe)) {
        auto decoder = detail::open_miniaudio_decoder(file, *probe);
#ifdef MSO_HAVE_FFMPEG
        // dr_wav refuses a few valid variants (AIFF-C in24/in32, unusual fmt layouts): let FFmpeg try
        // before reporting the file as damaged.
        if (!decoder && decoder.error().code == ErrorCode::CorruptAudio) {
            if (auto fallback = detail::open_ffmpeg_decoder(file, *probe)) return fallback;
        }
#endif
        return decoder;
    }
    if (probe->format == "ogg" && probe->codec == "vorbis") {
        // stb_vorbis first: FFmpeg's Vorbis decoder drops the first 128 frames of every stream
        // (measured: 43972 of 44100 encoded frames), which cuts the attack of one-shot samples.
        auto decoder = detail::open_vorbis_decoder(file, *probe);
#ifdef MSO_HAVE_FFMPEG
        // Files stb_vorbis refuses (over 2 GiB, or streams it cannot parse) still play through FFmpeg.
        if (!decoder &&
            (decoder.error().code == ErrorCode::CorruptAudio || decoder.error().code == ErrorCode::UnsupportedCodec)) {
            if (auto fallback = detail::open_ffmpeg_decoder(file, *probe)) return fallback;
        }
#endif
        return decoder;
    }
#ifdef MSO_HAVE_FFMPEG
    return detail::open_ffmpeg_decoder(file, *probe);
#else
    return fail(ErrorCode::UnsupportedCodec,
                std::format("{} decoder not built into this version (requires the FFmpeg backend)", probe->codec));
#endif
}

std::vector<std::string> decoder_capabilities() {
    std::vector<std::string> codecs = detail::miniaudio_codecs();
    codecs.emplace_back("vorbis");
#ifdef MSO_HAVE_FFMPEG
    for (std::string& codec : detail::ffmpeg_codecs()) {
        if (std::ranges::find(codecs, codec) == codecs.end()) codecs.push_back(std::move(codec));
    }
#endif
    return codecs;
}

}  // namespace mso::media
