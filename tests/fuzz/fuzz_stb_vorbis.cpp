// libFuzzer target: the vendored stb_vorbis decoder (deps/miniaudio/extras/stb_vorbis.c) on damaged
// Ogg Vorbis bytes, through stb_vorbis_open_memory().
//
// The preview and the waveform decode every Vorbis file with stb_vorbis first (src/media/decoder.cpp),
// in the desktop process, so its header parser and its packet decoder face whatever a sample folder
// holds. The target mirrors what src/media/decoder_vorbis.cpp does with a stream: open it, read the
// stream info and length, decode interleaved float frames, seek, and decode again. Decoding is capped
// so one input stays fast; the cap is far above what the seed files need to reach every code path.
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

namespace {

/// Closes a stb_vorbis stream.
struct VorbisCloser {
    void operator()(stb_vorbis* stream) const noexcept { stb_vorbis_close(stream); }
};

/// Frames decoded per input at most (about 1.5 s at 44.1 kHz).
constexpr int kMaximumFramesPerInput = 65'536;
/// Interleaved frames per decode call, as the decoder's block size.
constexpr int kFramesPerCall = 1024;
/// stb_vorbis supports at most this many channels in one stream (STB_VORBIS_MAX_CHANNELS).
constexpr int kMaximumChannels = 16;

/// Decodes up to `frame_budget` frames from the stream's current position.
void decode_frames(stb_vorbis* stream, int channels, int frame_budget) {
    static std::array<float, static_cast<std::size_t>(kFramesPerCall) * kMaximumChannels> buffer{};
    while (frame_budget > 0) {
        const int request = frame_budget < kFramesPerCall ? frame_budget : kFramesPerCall;
        const int frames =
            stb_vorbis_get_samples_float_interleaved(stream, channels, buffer.data(), request * channels);
        if (frames <= 0) return;
        frame_budget -= frames;
    }
}

}  // namespace

/// libFuzzer entry point: opens `size` bytes at `data` as an Ogg Vorbis stream and decodes it.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) return 0;
    int error = 0;
    const std::unique_ptr<stb_vorbis, VorbisCloser> stream(
        stb_vorbis_open_memory(data, static_cast<int>(size), &error, nullptr));
    if (!stream) return 0;
    const stb_vorbis_info info = stb_vorbis_get_info(stream.get());
    if (info.channels < 1 || info.channels > kMaximumChannels) return 0;
    const unsigned int length = stb_vorbis_stream_length_in_samples(stream.get());
    decode_frames(stream.get(), info.channels, kMaximumFramesPerInput / 2);
    // Seek into the middle (the length may be wrong in a damaged stream; that is the point) and decode on.
    if (stb_vorbis_seek(stream.get(), length / 2) != 0)
        decode_frames(stream.get(), info.channels, kMaximumFramesPerInput / 2);
    return 0;
}
