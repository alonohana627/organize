// FFmpeg streaming decoder (only built with MSO_HAVE_FFMPEG): AAC, Opus, ALAC, WMA, WavPack, ...
#ifdef MSO_HAVE_FFMPEG

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <vector>

#include "mso/media/decoder_backends.hpp"
#include "mso/media/ffmpeg_input.hpp"
#include "mso/media/file_handle.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

namespace mso::media::detail {

namespace {

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const noexcept { avcodec_free_context(&context); }
};
struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};
struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};
struct ResamplerDeleter {
    void operator()(SwrContext* resampler) const noexcept { swr_free(&resampler); }
};

/// FFmpeg decoder ids we route here, with the names decoder_capabilities() reports.
constexpr std::pair<AVCodecID, std::string_view> kRoutedCodecs[] = {
    {AV_CODEC_ID_AAC, "aac"},
    {AV_CODEC_ID_OPUS, "opus"},
    {AV_CODEC_ID_ALAC, "alac"},
    {AV_CODEC_ID_WMAV1, "wma"},
    {AV_CODEC_ID_WMAV2, "wma"},
    {AV_CODEC_ID_WMAPRO, "wmapro"},
    {AV_CODEC_ID_WMALOSSLESS, "wmalossless"},
    {AV_CODEC_ID_WAVPACK, "wavpack"},
    {AV_CODEC_ID_AC3, "ac3"},
    {AV_CODEC_ID_EAC3, "eac3"},
    {AV_CODEC_ID_ADPCM_IMA_QT, "adpcm_ima_qt"},
    {AV_CODEC_ID_APE, "ape"},
    {AV_CODEC_ID_TTA, "tta"},
    {AV_CODEC_ID_SPEEX, "speex"},
};

/// Samples per second as an FFmpeg rational time base.
AVRational sample_time_base(int sample_rate) {
    return AVRational{1, sample_rate};
}

/// Streams interleaved float32 frames from the default audio stream of a file.
class FfmpegDecoder final : public Decoder {
public:
    explicit FfmpegDecoder(FileHandle file) : file_(std::move(file)) {}
    FfmpegDecoder(const FfmpegDecoder&) = delete;
    FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;

    Result<void> open(const ProbeResult& probe) {
        const auto size = file_.size();
        if (!size) return std::unexpected(size.error());
        auto input = FfmpegInput::open(
            [this](std::uint64_t offset, std::span<std::uint8_t> destination) {
                return file_.read_at(offset, destination);
            },
            *size, kDefaultProbeBytes);
        if (!input) return std::unexpected(input.error());
        input_ = std::move(*input);
        stream_ = input_->audio_stream();
        if (stream_ == nullptr) return fail(ErrorCode::UnsupportedCodec, "file has no audio stream");
        if (auto opened = open_codec(); !opened) return opened;

        // Decode the first frame now: HE-AAC and similar streams only reveal their true output
        // rate and layout once decoding starts.
        auto first = decode_next_frame();
        if (!first) return std::unexpected(first.error());
        if (!*first) return fail(ErrorCode::CorruptAudio, "audio stream contains no decodable frames");
        // Some decoders (WMA) emit output whose first sample sits off the container's zero; the
        // first frame defines output position 0 and seeks are translated through this origin.
        timeline_origin_ = current_frame_position_.value_or(0);
        current_frame_position_ = 0;
        next_frame_position_ = static_cast<std::int64_t>(pending_frames());
        determine_length(probe);
        return {};
    }

    int channels() const override { return channels_; }
    int sample_rate() const override { return sample_rate_; }
    std::optional<std::int64_t> total_frames() const override { return total_frames_; }
    bool frames_exact() const override { return frames_exact_; }
    std::int64_t position() const override { return position_; }
    std::string codec() const override { return avcodec_get_name(stream_->codecpar->codec_id); }

    Result<std::size_t> read(float* out, std::size_t frames) override {
        std::size_t delivered = 0;
        while (delivered < frames) {
            if (pending_offset_ >= pending_frames()) {
                auto decoded = decode_next_frame();
                if (!decoded) return std::unexpected(decoded.error());
                if (!*decoded) break;
                continue;
            }
            const std::size_t count = std::min(frames - delivered, pending_frames() - pending_offset_);
            const auto channel_count = static_cast<std::size_t>(channels_);
            std::memcpy(out + delivered * channel_count, pending_.data() + pending_offset_ * channel_count,
                        count * channel_count * sizeof(float));
            pending_offset_ += count;
            delivered += count;
        }
        position_ += static_cast<std::int64_t>(delivered);
        return delivered;
    }

    /// Container seeks are approximate (unindexed ASF, VBR streams): when the demuxer lands after
    /// the target, aim earlier with a doubling margin, then decode forward to the exact frame.
    Result<void> seek(std::int64_t frame) override {
        if (frame < 0) return fail(ErrorCode::InvalidArgument, "seek position is negative");
        std::int64_t margin = 0;
        while (true) {
            const std::int64_t aim = std::max<std::int64_t>(0, frame - margin);
            if (auto moved = reposition(aim); !moved) return moved;
            auto decoded = decode_next_frame();
            if (!decoded) return std::unexpected(decoded.error());
            const bool at_or_before_target = !*decoded || !current_frame_position_ || *current_frame_position_ <= frame;
            if (at_or_before_target || aim == 0) return skip_to(frame, *decoded);
            margin = margin == 0 ? sample_rate_ : margin * 2;
        }
    }

private:
    static constexpr std::int64_t kDefaultProbeBytes = 5'000'000;  // FFmpeg's own default

    std::size_t pending_frames() const noexcept {
        return pending_.size() / static_cast<std::size_t>(std::max(channels_, 1));
    }

    Result<void> open_codec() {
        const AVCodec* decoder = avcodec_find_decoder(stream_->codecpar->codec_id);
        if (decoder == nullptr) {
            return fail(ErrorCode::UnsupportedCodec, std::format("no decoder for {} in this FFmpeg build",
                                                                 avcodec_get_name(stream_->codecpar->codec_id)));
        }
        codec_.reset(avcodec_alloc_context3(decoder));
        packet_.reset(av_packet_alloc());
        frame_.reset(av_frame_alloc());
        if (!codec_ || !packet_ || !frame_)
            return fail(ErrorCode::ResourceLimit, "cannot allocate FFmpeg decoder state");
        if (avcodec_parameters_to_context(codec_.get(), stream_->codecpar) < 0) {
            return fail(ErrorCode::CorruptAudio, "invalid codec parameters");
        }
        codec_->pkt_timebase = stream_->time_base;
        const int status = avcodec_open2(codec_.get(), decoder, nullptr);
        if (status < 0)
            return fail(ErrorCode::CorruptAudio, std::format("cannot open decoder: {}", ffmpeg_error_text(status)));
        return {};
    }

    /// Exact counts from the native probe when it had one (ALAC, PCM); otherwise the container's
    /// duration, which for lossy codecs is an estimate.
    void determine_length(const ProbeResult& probe) {
        if (probe.frames && probe.sample_rate == sample_rate_) {
            total_frames_ = probe.frames;
            frames_exact_ = true;
        } else if (stream_->duration != AV_NOPTS_VALUE && stream_->duration > 0) {
            total_frames_ = av_rescale_q(stream_->duration, stream_->time_base, sample_time_base(sample_rate_));
        } else if (probe.duration_us) {
            total_frames_ = av_rescale(*probe.duration_us, sample_rate_, 1'000'000);
        }
    }

    /// Converts the decoded frame in frame_ to interleaved float32 in pending_.
    Result<void> convert_frame() {
        if (!resampler_ || frame_->format != resampler_format_ || frame_->sample_rate != sample_rate_ ||
            frame_->ch_layout.nb_channels != channels_) {
            if (auto configured = configure_resampler(); !configured) return configured;
        }
        const int capacity = swr_get_out_samples(resampler_.get(), frame_->nb_samples);
        if (capacity < 0) return fail(ErrorCode::CorruptAudio, "resampler rejected the decoded frame");
        pending_.resize(static_cast<std::size_t>(capacity) * static_cast<std::size_t>(channels_));
        auto* output = reinterpret_cast<std::uint8_t*>(pending_.data());
        const int converted = swr_convert(resampler_.get(), &output, capacity,
                                          const_cast<const std::uint8_t**>(frame_->extended_data), frame_->nb_samples);
        if (converted < 0) return fail(ErrorCode::CorruptAudio, "sample format conversion failed");
        pending_.resize(static_cast<std::size_t>(converted) * static_cast<std::size_t>(channels_));
        pending_offset_ = 0;
        return {};
    }

    /// Format conversion only: same rate and channel layout in, interleaved float out.
    Result<void> configure_resampler() {
        AVChannelLayout layout{};
        if (frame_->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_default(&layout, frame_->ch_layout.nb_channels);
        } else if (av_channel_layout_copy(&layout, &frame_->ch_layout) < 0) {
            return fail(ErrorCode::ResourceLimit, "cannot copy channel layout");
        }
        SwrContext* resampler = nullptr;
        const int status =
            swr_alloc_set_opts2(&resampler, &layout, AV_SAMPLE_FMT_FLT, frame_->sample_rate, &layout,
                                static_cast<AVSampleFormat>(frame_->format), frame_->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&layout);
        resampler_.reset(resampler);
        if (status < 0 || swr_init(resampler_.get()) < 0)
            return fail(ErrorCode::CorruptAudio, "cannot configure sample conversion");
        if (frame_->ch_layout.nb_channels < 1 || frame_->sample_rate < 1) {
            return fail(ErrorCode::CorruptAudio, "decoded frame has no channels or sample rate");
        }
        resampler_format_ = frame_->format;
        channels_ = frame_->ch_layout.nb_channels;
        sample_rate_ = frame_->sample_rate;
        return {};
    }

    /// Source-frame position of the decoded frame. Decoded audio is contiguous, so once one frame
    /// is placed the next follows it exactly; timestamps (which several codecs repeat for every
    /// frame of a packet) are only used to place the first frame after opening or seeking.
    std::optional<std::int64_t> frame_position() const {
        if (next_frame_position_) return next_frame_position_;
        const auto timeline = timeline_position();
        if (!timeline) return std::nullopt;
        return *timeline - timeline_origin_;
    }

    /// Position of the decoded frame on the container's timeline, in samples.
    std::optional<std::int64_t> timeline_position() const {
        const std::int64_t timestamp = frame_->best_effort_timestamp;
        if (timestamp == AV_NOPTS_VALUE) return std::nullopt;
        const std::int64_t start = stream_->start_time == AV_NOPTS_VALUE ? 0 : stream_->start_time;
        return av_rescale_q(timestamp - start, stream_->time_base, sample_time_base(sample_rate_));
    }

    /// Decodes the next frame of the audio stream into pending_; false at end of stream.
    Result<bool> decode_next_frame() {
        while (true) {
            const int received = avcodec_receive_frame(codec_.get(), frame_.get());
            if (received == 0) {
                auto converted = convert_frame();
                const auto start = frame_position();
                current_frame_position_ = start;
                if (start) next_frame_position_ = *start + static_cast<std::int64_t>(pending_frames());
                av_frame_unref(frame_.get());
                if (!converted) return std::unexpected(converted.error());
                return true;
            }
            if (received == AVERROR_EOF) return false;
            if (received != AVERROR(EAGAIN)) {
                return fail(ErrorCode::CorruptAudio, std::format("decoding failed: {}", ffmpeg_error_text(received)));
            }
            if (draining_) return false;
            if (auto fed = feed_decoder(); !fed) return std::unexpected(fed.error());
        }
    }

    /// Sends the next packet of the audio stream (or the end-of-stream flush) to the decoder.
    Result<void> feed_decoder() {
        while (true) {
            const int status = av_read_frame(input_->context(), packet_.get());
            if (status == AVERROR_EOF) {
                avcodec_send_packet(codec_.get(), nullptr);
                draining_ = true;
                return {};
            }
            if (status < 0) {
                if (input_->read_error()) return std::unexpected(*input_->read_error());
                return fail(ErrorCode::CorruptAudio,
                            std::format("reading the stream failed: {}", ffmpeg_error_text(status)));
            }
            const bool ours = packet_->stream_index == stream_->index;
            const int sent = ours ? avcodec_send_packet(codec_.get(), packet_.get()) : 0;
            av_packet_unref(packet_.get());
            if (!ours || sent == AVERROR_INVALIDDATA) continue;  // other streams; damaged packets are skipped
            if (sent < 0 && sent != AVERROR(EAGAIN)) {
                return fail(ErrorCode::CorruptAudio, std::format("decoding failed: {}", ffmpeg_error_text(sent)));
            }
            return {};
        }
    }

    /// Asks the demuxer for the packet containing `frame` and resets decoding state.
    Result<void> reposition(std::int64_t frame) {
        const std::int64_t start = stream_->start_time == AV_NOPTS_VALUE ? 0 : stream_->start_time;
        const std::int64_t timestamp =
            start + av_rescale_q(frame + timeline_origin_, sample_time_base(sample_rate_), stream_->time_base);
        const int status = av_seek_frame(input_->context(), stream_->index, timestamp, AVSEEK_FLAG_BACKWARD);
        if (status < 0) return fail(ErrorCode::CorruptAudio, std::format("seek failed: {}", ffmpeg_error_text(status)));
        avcodec_flush_buffers(codec_.get());
        pending_.clear();
        pending_offset_ = 0;
        draining_ = false;
        next_frame_position_.reset();
        return {};
    }

    /// Drops decoded samples before `target`, starting from the frame already in pending_
    /// (`have_frame` false means the stream already ended).
    Result<void> skip_to(std::int64_t target, bool have_frame) {
        while (have_frame) {
            const std::int64_t start = current_frame_position_.value_or(target);
            const auto frames = static_cast<std::int64_t>(pending_frames());
            if (start + frames > target) {
                pending_offset_ = static_cast<std::size_t>(std::clamp<std::int64_t>(target - start, 0, frames));
                break;
            }
            auto decoded = decode_next_frame();
            if (!decoded) return std::unexpected(decoded.error());
            have_frame = *decoded;
        }
        if (!have_frame) pending_.clear();  // past the end: the next read returns 0
        position_ = target;
        return {};
    }

    FileHandle file_;  // declared first: the FFmpeg input reads through it until destroyed
    std::unique_ptr<FfmpegInput> input_;
    AVStream* stream_ = nullptr;
    std::unique_ptr<AVCodecContext, CodecContextDeleter> codec_;
    std::unique_ptr<AVPacket, PacketDeleter> packet_;
    std::unique_ptr<AVFrame, FrameDeleter> frame_;
    std::unique_ptr<SwrContext, ResamplerDeleter> resampler_;
    int resampler_format_ = -1;
    int channels_ = 0;
    int sample_rate_ = 0;
    std::vector<float> pending_;
    std::size_t pending_offset_ = 0;
    bool draining_ = false;
    std::int64_t timeline_origin_ = 0;  // container timeline position of output frame 0
    std::optional<std::int64_t> current_frame_position_;
    std::optional<std::int64_t> next_frame_position_;
    std::optional<std::int64_t> total_frames_;
    bool frames_exact_ = false;
    std::int64_t position_ = 0;
};

}  // namespace

Result<std::unique_ptr<Decoder>> open_ffmpeg_decoder(const std::filesystem::path& file, const ProbeResult& probe) {
    auto handle = FileHandle::open_for_read(file);
    if (!handle) return std::unexpected(handle.error());
    auto decoder = std::make_unique<FfmpegDecoder>(std::move(*handle));
    if (auto opened = decoder->open(probe); !opened) return std::unexpected(opened.error());
    return std::unique_ptr<Decoder>(std::move(decoder));
}

std::vector<std::string> ffmpeg_codecs() {
    std::vector<std::string> codecs;
    for (const auto& [codec_id, name] : kRoutedCodecs) {
        if (avcodec_find_decoder(codec_id) != nullptr && std::ranges::find(codecs, name) == codecs.end()) {
            codecs.emplace_back(name);
        }
    }
    return codecs;
}

}  // namespace mso::media::detail

#endif  // MSO_HAVE_FFMPEG
