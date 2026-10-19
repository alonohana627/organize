// Ogg Vorbis streaming decoder on stb_vorbis (compiled into deps/miniaudio_impl.c). Preferred over
// FFmpeg for Vorbis because FFmpeg's decoder drops the first 128 frames of a stream.
//
// miniaudio's own Vorbis path falls back to stb_vorbis push mode (no length, brute-force seeking),
// so this backend drives stb_vorbis in pull mode directly. stb_vorbis only reads from a FILE*, so
// the file is opened as a FileHandle (Unicode and long paths, sharing read/write/delete) and the
// handle is then handed to a C stdio stream.
#include <algorithm>
#include <cstdio>
#include <format>
#include <limits>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#include "mso/media/detail/decoder_backends.hpp"
#include "mso/media/detail/file_handle.hpp"

namespace mso::media::detail {

namespace {

/// stb_vorbis measures files with ftell() (a 32-bit long on Windows) and stores sample counts in
/// 32 bits, so larger files are refused instead of being misread.
constexpr std::uint64_t kMaxStbVorbisFileBytes = std::numeric_limits<std::int32_t>::max();

/// Closes an stb_vorbis stream (and the FILE* it owns).
struct StbVorbisCloser {
    void operator()(stb_vorbis* stream) const noexcept { stb_vorbis_close(stream); }
};

/// Streams float32 frames from an Ogg Vorbis file.
class VorbisDecoder final : public Decoder {
public:
    explicit VorbisDecoder(std::unique_ptr<stb_vorbis, StbVorbisCloser> stream) : stream_(std::move(stream)) {
        const stb_vorbis_info info = stb_vorbis_get_info(stream_.get());
        channels_ = info.channels;
        sample_rate_ = static_cast<int>(std::min<unsigned int>(info.sample_rate, std::numeric_limits<int>::max()));
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
        if (frames == 0 || at_end_) return std::size_t{0};
        const auto channel_count = static_cast<std::size_t>(channels_);
        const std::size_t max_frames = static_cast<std::size_t>(std::numeric_limits<int>::max()) / channel_count;
        const auto request = static_cast<int>(std::min(frames, max_frames) * channel_count);
        const int frames_read = stb_vorbis_get_samples_float_interleaved(stream_.get(), channels_, out, request);
        if (frames_read < 0) return fail(ErrorCode::CorruptAudio, "Vorbis decoding failed");
        position_ += frames_read;
        return static_cast<std::size_t>(frames_read);
    }

    Result<void> seek(std::int64_t frame) override {
        if (frame < 0) return fail(ErrorCode::InvalidArgument, "seek position is negative");
        at_end_ = false;
        if (total_frames_ && frame >= *total_frames_) {
            at_end_ = true;  // at or past the exact end: positioned at the end, reads return 0
            position_ = *total_frames_;
            return {};
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
    bool at_end_ = false;  ///< positioned at the end by a seek; read() returns 0 until the next seek
    int channels_ = 0;
    int sample_rate_ = 0;
    std::optional<std::int64_t> total_frames_;
    std::int64_t position_ = 0;
};

}  // namespace

Result<std::unique_ptr<Decoder>> open_vorbis_decoder(const std::filesystem::path& file, const ProbeResult& /*probe*/) {
    auto handle = FileHandle::open_for_read(file);
    if (!handle) return std::unexpected(handle.error());
    const auto size = handle->size();
    if (!size) return std::unexpected(size.error());
    if (*size > kMaxStbVorbisFileBytes) {
        return fail(ErrorCode::UnsupportedCodec,
                    "the built-in Vorbis decoder reads files up to 2 GiB (larger ones need the FFmpeg backend)");
    }
    auto stream = handle->release_to_stdio();
    if (!stream) return std::unexpected(stream.error());
    int error = 0;
    // close_on_free = 1: stb_vorbis owns the FILE* from here on, including when opening fails.
    stb_vorbis* vorbis = stb_vorbis_open_file(*stream, 1, &error, nullptr);
    if (vorbis == nullptr) return fail(ErrorCode::CorruptAudio, std::format("invalid Vorbis stream (error {})", error));
    std::unique_ptr<stb_vorbis, StbVorbisCloser> owned(vorbis);
    const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    if (info.channels < 1 || info.sample_rate < 1) return fail(ErrorCode::CorruptAudio, "Vorbis stream has no audio");
    return std::unique_ptr<Decoder>(std::make_unique<VorbisDecoder>(std::move(owned)));
}

}  // namespace mso::media::detail
