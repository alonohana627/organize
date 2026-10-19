// Ogg Vorbis streaming decoder on stb_vorbis (compiled into deps/miniaudio_impl.c).
//
// miniaudio's own Vorbis path cannot open wide-character paths and falls back to stb_vorbis push
// mode (no length, brute-force seeking), so this backend drives stb_vorbis in pull mode directly
// over a FILE* opened with a Unicode-safe call.
#include <cerrno>
#include <cstdio>
#include <format>
#include <limits>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#ifdef _WIN32
#include <share.h>
#endif

#include "mso/media/decoder_backends.hpp"

namespace mso::media::detail {

namespace {

/// Opens the file for reading without locking it against other applications.
Result<std::FILE*> open_stdio_file(const std::filesystem::path& file) {
#ifdef _WIN32
    std::FILE* stream = _wfsopen(file.c_str(), L"rb", _SH_DENYNO);
#else
    std::FILE* stream = std::fopen(file.c_str(), "rb");
#endif
    if (stream != nullptr) return stream;
    if (errno == ENOENT) return fail(ErrorCode::FileMissing, "file does not exist");
    return fail(ErrorCode::AccessDenied, "cannot open the file for decoding");
}

struct StbVorbisCloser {
    void operator()(stb_vorbis* stream) const noexcept { stb_vorbis_close(stream); }
};

/// Streams float32 frames from an Ogg Vorbis file.
class VorbisDecoder final : public Decoder {
public:
    explicit VorbisDecoder(std::unique_ptr<stb_vorbis, StbVorbisCloser> stream) : stream_(std::move(stream)) {
        const stb_vorbis_info info = stb_vorbis_get_info(stream_.get());
        channels_ = info.channels;
        sample_rate_ = static_cast<int>(info.sample_rate);
        const unsigned int length = stb_vorbis_stream_length_in_samples(stream_.get());
        if (length > 0) total_frames_ = length;  // 0 means the last granule could not be found
    }

    int channels() const override { return channels_; }
    int sample_rate() const override { return sample_rate_; }
    std::optional<std::int64_t> total_frames() const override { return total_frames_; }
    bool frames_exact() const override { return total_frames_.has_value(); }
    std::int64_t position() const override { return position_; }
    std::string codec() const override { return "vorbis"; }

    Result<std::size_t> read(float* out, std::size_t frames) override {
        const std::size_t max_frames = static_cast<std::size_t>(std::numeric_limits<int>::max()) / channels_;
        const auto request = static_cast<int>(std::min(frames, max_frames) * static_cast<std::size_t>(channels_));
        const int frames_read = stb_vorbis_get_samples_float_interleaved(stream_.get(), channels_, out, request);
        if (frames_read < 0) return fail(ErrorCode::CorruptAudio, "Vorbis decoding failed");
        position_ += frames_read;
        return static_cast<std::size_t>(frames_read);
    }

    Result<void> seek(std::int64_t frame) override {
        if (frame < 0) return fail(ErrorCode::InvalidArgument, "seek position is negative");
        if (total_frames_ && frame > *total_frames_) {
            return fail(ErrorCode::InvalidArgument, "seek position is past the end of the stream");
        }
        if (frame > std::numeric_limits<unsigned int>::max()) {
            return fail(ErrorCode::InvalidArgument, "seek position exceeds the Vorbis decoder's range");
        }
        if (!stb_vorbis_seek(stream_.get(), static_cast<unsigned int>(frame))) {
            return fail(ErrorCode::CorruptAudio,
                        std::format("Vorbis seek failed (error {})", stb_vorbis_get_error(stream_.get())));
        }
        position_ = frame;
        return {};
    }

private:
    std::unique_ptr<stb_vorbis, StbVorbisCloser> stream_;
    int channels_ = 0;
    int sample_rate_ = 0;
    std::optional<std::int64_t> total_frames_;
    std::int64_t position_ = 0;
};

}  // namespace

Result<std::unique_ptr<Decoder>> open_vorbis_decoder(const std::filesystem::path& file, const ProbeResult& /*probe*/) {
    auto stream = open_stdio_file(file);
    if (!stream) return std::unexpected(stream.error());
    int error = 0;
    // close_handle_on_close = 1: stb_vorbis owns the FILE* from here on, including on failure.
    stb_vorbis* vorbis = stb_vorbis_open_file(*stream, 1, &error, nullptr);
    if (vorbis == nullptr) return fail(ErrorCode::CorruptAudio, std::format("invalid Vorbis stream (error {})", error));
    std::unique_ptr<stb_vorbis, StbVorbisCloser> owned(vorbis);
    if (stb_vorbis_get_info(vorbis).channels < 1) return fail(ErrorCode::CorruptAudio, "Vorbis stream has no channels");
    return std::unique_ptr<Decoder>(std::make_unique<VorbisDecoder>(std::move(owned)));
}

}  // namespace mso::media::detail
