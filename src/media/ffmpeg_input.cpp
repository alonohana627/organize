#ifdef MSO_HAVE_FFMPEG

#include "mso/media/ffmpeg_input.hpp"

#include <cstdio>
#include <format>
#include <limits>
#include <mutex>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
}

namespace mso::media::detail {

namespace {

constexpr int kIoBufferSize = 64 * 1024;

/// Demuxers FFmpeg may use: audio containers only, so no playlist (hls, concat), image or
/// network-oriented demuxer can ever be selected.
constexpr const char* kAllowedDemuxers =
    "aac,ac3,aiff,amr,ape,asf,au,caf,dsf,dts,eac3,flac,matroska,mov,mp3,mpc,mpc8,ogg,shn,tak,tta,voc,w64,wav,wv";

/// Refuses every secondary open (MOV external data references, playlist entries, ...).
int refuse_nested_open(AVFormatContext* /*context*/, AVIOContext** /*io*/, const char* /*url*/, int /*flags*/,
                       AVDictionary** /*options*/) {
    return AVERROR(EPERM);
}

}  // namespace

std::string ffmpeg_error_text(int status) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(status, text, sizeof(text));
    return text;
}

int FfmpegInput::read_packet(void* opaque, std::uint8_t* buffer, int buffer_size) {
    auto* input = static_cast<FfmpegInput*>(opaque);
    if (buffer_size <= 0 || input->position_ >= input->size_) return AVERROR_EOF;
    const auto received = input->read_at_(input->position_, {buffer, static_cast<std::size_t>(buffer_size)});
    if (!received) {
        input->read_error_ = received.error();
        return AVERROR(EIO);
    }
    if (*received == 0) return AVERROR_EOF;
    input->position_ += *received;
    return static_cast<int>(*received);
}

std::int64_t FfmpegInput::seek(void* opaque, std::int64_t offset, int whence) {
    auto* input = static_cast<FfmpegInput*>(opaque);
    if (whence & AVSEEK_SIZE) return static_cast<std::int64_t>(input->size_);
    std::int64_t base = 0;
    switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = static_cast<std::int64_t>(input->position_);
            break;
        case SEEK_END:
            base = static_cast<std::int64_t>(input->size_);
            break;
        default:
            return AVERROR(EINVAL);
    }
    std::int64_t target = 0;
    if (__builtin_add_overflow(base, offset, &target) || target < 0) return AVERROR(EINVAL);
    input->position_ = static_cast<std::uint64_t>(target);
    return target;
}

Result<std::unique_ptr<FfmpegInput>> FfmpegInput::open(ReadAtFunction read_at, std::uint64_t size,
                                                       std::int64_t probe_bytes) {
    // Failures are reported through Result; FFmpeg must not write to the console of a GUI app.
    static std::once_flag logging_silenced;
    std::call_once(logging_silenced, [] { av_log_set_level(AV_LOG_QUIET); });
    std::unique_ptr<FfmpegInput> input(new FfmpegInput(std::move(read_at), size));
    auto* buffer = static_cast<unsigned char*>(av_malloc(kIoBufferSize));
    if (buffer == nullptr) return fail(ErrorCode::ResourceLimit, "cannot allocate FFmpeg I/O buffer");
    input->io_ = avio_alloc_context(buffer, kIoBufferSize, 0, input.get(), &FfmpegInput::read_packet, nullptr,
                                    &FfmpegInput::seek);
    if (input->io_ == nullptr) {
        av_free(buffer);
        return fail(ErrorCode::ResourceLimit, "cannot allocate FFmpeg I/O context");
    }
    input->format_ = avformat_alloc_context();
    if (input->format_ == nullptr) return fail(ErrorCode::ResourceLimit, "cannot allocate FFmpeg format context");
    input->format_->pb = input->io_;
    input->format_->flags |= AVFMT_FLAG_CUSTOM_IO;
    input->format_->io_open = &refuse_nested_open;
    input->format_->probesize = std::max<std::int64_t>(probe_bytes, 32);

    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "format_whitelist", kAllowedDemuxers, 0);
    const int status = avformat_open_input(&input->format_, "", nullptr, &options);
    av_dict_free(&options);
    if (status < 0) {
        input->format_ = nullptr;  // freed by avformat_open_input on failure
        if (input->read_error_) return std::unexpected(*input->read_error_);
        if (status == AVERROR_INVALIDDATA) return fail(ErrorCode::UnsupportedCodec, "not a recognised audio file");
        return fail(ErrorCode::CorruptAudio,
                    std::format("FFmpeg could not open the file: {}", ffmpeg_error_text(status)));
    }
    return input;
}

FfmpegInput::~FfmpegInput() {
    if (format_ != nullptr) avformat_close_input(&format_);
    if (io_ != nullptr) {
        av_freep(&io_->buffer);
        avio_context_free(&io_);
    }
}

AVStream* FfmpegInput::audio_stream() const noexcept {
    const int index = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    return index < 0 ? nullptr : format_->streams[index];
}

}  // namespace mso::media::detail

#endif  // MSO_HAVE_FFMPEG
