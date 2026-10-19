#pragma once
/// @file
/// Locked-down FFmpeg demuxer input (only built when MSO_HAVE_FFMPEG is defined).
///
/// FFmpeg never opens anything by name here: bytes come from a caller-supplied callback (our own
/// file handle or the probe's bounded reader), nested opens such as playlist entries or MOV
/// external references are refused, the protocol whitelist is "file" only, and only audio
/// demuxers are allowed. A file containing a URL therefore cannot cause any network access.
#ifdef MSO_HAVE_FFMPEG

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

#include "mso/domain/error.hpp"

struct AVFormatContext;
struct AVIOContext;
struct AVStream;

namespace mso::media::detail {

/// Pulls bytes at an absolute offset; returns how many were read (0 at end of file).
using ReadAtFunction = std::function<Result<std::size_t>(std::uint64_t offset, std::span<std::uint8_t> destination)>;

/// Owns an AVFormatContext reading through a custom AVIOContext.
class FfmpegInput {
public:
    /// Opens a demuxer over `read_at`. `probe_bytes` bounds FFmpeg's format probing.
    /// Errors: the callback's own error when it failed (e.g. ProbeLimit), UnsupportedCodec when no
    /// allowed demuxer recognises the data, CorruptAudio for damaged headers.
    static Result<std::unique_ptr<FfmpegInput>> open(ReadAtFunction read_at, std::uint64_t size,
                                                     std::int64_t probe_bytes);

    FfmpegInput(const FfmpegInput&) = delete;
    FfmpegInput& operator=(const FfmpegInput&) = delete;
    ~FfmpegInput();

    AVFormatContext* context() const noexcept { return format_; }
    /// The default (or best) audio stream; nullptr when the file has none.
    AVStream* audio_stream() const noexcept;
    /// The error the read callback last reported, if any.
    const std::optional<Error>& read_error() const noexcept { return read_error_; }

private:
    FfmpegInput(ReadAtFunction read_at, std::uint64_t size) : read_at_(std::move(read_at)), size_(size) {}

    static int read_packet(void* opaque, std::uint8_t* buffer, int buffer_size);
    static std::int64_t seek(void* opaque, std::int64_t offset, int whence);

    ReadAtFunction read_at_;
    std::uint64_t size_ = 0;
    std::uint64_t position_ = 0;
    std::optional<Error> read_error_;
    AVIOContext* io_ = nullptr;
    AVFormatContext* format_ = nullptr;
};

/// Text for an FFmpeg error code.
std::string ffmpeg_error_text(int status);

}  // namespace mso::media::detail

#endif  // MSO_HAVE_FFMPEG
