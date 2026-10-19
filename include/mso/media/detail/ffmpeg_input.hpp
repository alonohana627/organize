#pragma once
/// @file
/// Locked-down FFmpeg demuxer input (only built when MSO_HAVE_FFMPEG is defined).
///
/// FFmpeg never opens anything by name here: bytes come from a caller-supplied callback (our own
/// file handle or the probe's bounded reader), nested opens such as playlist entries or MOV
/// external references are refused, the protocol whitelist is "file" only, and only audio
/// demuxers are allowed. A file containing a URL therefore cannot cause any network access.
#ifdef MSO_HAVE_FFMPEG

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

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
    /// Opens a demuxer over `read_at`. `probe_bytes` bounds FFmpeg's format probing. With a
    /// `deadline`, FFmpeg's own loops (stream analysis decodes packets) are interrupted once it
    /// passes, so CPU work is bounded by time as well as by the bytes the callback hands out.
    /// Errors: the callback's own error when it failed (e.g. ProbeLimit), UnsupportedCodec when no
    /// allowed demuxer recognises the data, CorruptAudio for damaged headers, ProbeLimit when the
    /// deadline interrupted the open.
    [[nodiscard]] static Result<std::unique_ptr<FfmpegInput>> open(
        ReadAtFunction read_at, std::uint64_t size, std::int64_t probe_bytes,
        std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

    FfmpegInput(const FfmpegInput&) = delete;
    FfmpegInput& operator=(const FfmpegInput&) = delete;
    ~FfmpegInput();

    /// The demuxer context (owned by this object).
    AVFormatContext* context() const noexcept { return format_; }
    /// The default (or best) audio stream; nullptr when the file has none.
    AVStream* audio_stream() const noexcept;
    /// Reads packets until every stream's parameters and durations are known, within the probe
    /// size given to open(). Errors: the read callback's error (e.g. ProbeLimit) when it failed,
    /// ProbeLimit when the deadline given to open() interrupted it, CorruptAudio when FFmpeg could
    /// not analyse the streams.
    [[nodiscard]] Result<void> find_stream_info();
    /// The error the read callback last reported, if any. A callback failure is not always fatal
    /// to FFmpeg (it may report a short stream instead), so callers check this after every step.
    /// It stays set until clear_read_error(): after a failed read FFmpeg's I/O context reports the
    /// end of the file until the next seek, so every later step really is affected by it.
    const std::optional<Error>& read_error() const noexcept { return read_error_; }
    /// Forgets the last read error, here and in FFmpeg's I/O context (which otherwise keeps failing
    /// every read after one failure, seek or not). The decoder calls it when it repositions the
    /// demuxer, so the file reads again once a drive is back, and a failure that is over is not
    /// reported at the next end of the stream.
    void clear_read_error() noexcept;

private:
    FfmpegInput(ReadAtFunction read_at, std::uint64_t size) : read_at_(std::move(read_at)), size_(size) {}

    /// AVIOContext read callback.
    static int read_packet(void* opaque, std::uint8_t* buffer, int buffer_size);
    /// AVIOContext seek callback (also answers AVSEEK_SIZE).
    static std::int64_t seek(void* opaque, std::int64_t offset, int whence);
    /// AVIOInterruptCB callback: non-zero once the deadline has passed.
    static int deadline_passed(void* opaque);
    /// True when a deadline was given and has passed.
    [[nodiscard]] bool past_deadline() const noexcept;

    ReadAtFunction read_at_;
    std::uint64_t size_ = 0;
    std::uint64_t position_ = 0;
    std::optional<Error> read_error_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;  ///< interrupts FFmpeg's loops
    AVIOContext* io_ = nullptr;
    AVFormatContext* format_ = nullptr;
};

/// Text for an FFmpeg error code.
std::string ffmpeg_error_text(int status);

}  // namespace mso::media::detail

#endif  // MSO_HAVE_FFMPEG
