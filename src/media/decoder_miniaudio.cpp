// miniaudio-backed streaming decoder (dr_wav, dr_flac, dr_mp3).
#include <algorithm>
#include <array>
#include <format>

#include <miniaudio.h>

#include "mso/media/decoder_backends.hpp"

namespace mso::media::detail {

namespace {

/// Codecs dr_wav decodes from WAV-family and AIFF containers.
constexpr std::array<std::string_view, 11> kWaveCodecs = {
    "pcm_u8",  "pcm_s8",   "pcm_s16",   "pcm_s24",  "pcm_s32",   "pcm_f32",
    "pcm_f64", "pcm_alaw", "pcm_mulaw", "adpcm_ms", "adpcm_ima",
};
constexpr std::array<std::string_view, 5> kWaveContainers = {"wav", "rf64", "w64", "aiff", "aifc"};
constexpr std::array<std::string_view, 3> kMpegCodecs = {"mp3", "mp2", "mp1"};

bool contains(auto& list, std::string_view value) {
    return std::ranges::find(list, value) != list.end();
}

/// The miniaudio decoding backend for a probed stream; ma_encoding_format_unknown if none.
ma_encoding_format encoding_for(const ProbeResult& probe) {
    if (contains(kWaveContainers, probe.format) && contains(kWaveCodecs, probe.codec)) return ma_encoding_format_wav;
    if (probe.format == "flac" && probe.codec == "flac") return ma_encoding_format_flac;
    if (probe.format == "mp3" && contains(kMpegCodecs, probe.codec)) return ma_encoding_format_mp3;
    return ma_encoding_format_unknown;
}

Error error_for(ma_result result, std::string_view action) {
    switch (result) {
        case MA_DOES_NOT_EXIST:
            return fail(ErrorCode::FileMissing, "file does not exist").error();
        case MA_ACCESS_DENIED:
            return fail(ErrorCode::AccessDenied, "access to the file was denied").error();
        case MA_OUT_OF_MEMORY:
            return fail(ErrorCode::ResourceLimit, "out of memory while decoding").error();
        default:
            return fail(ErrorCode::CorruptAudio, std::format("{} failed: {}", action, ma_result_description(result)))
                .error();
    }
}

/// Streams float32 frames from an ma_decoder. Not movable: miniaudio keeps internal pointers.
class MiniaudioDecoder final : public Decoder {
public:
    MiniaudioDecoder() = default;
    MiniaudioDecoder(const MiniaudioDecoder&) = delete;
    MiniaudioDecoder& operator=(const MiniaudioDecoder&) = delete;
    ~MiniaudioDecoder() override {
        if (initialized_) ma_decoder_uninit(&decoder_);
    }

    /// Opens the file with the backend chosen from the probe, at native rate and channel count.
    Result<void> open(const std::filesystem::path& file, const ProbeResult& probe) {
        const ma_encoding_format encoding = encoding_for(probe);
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        config.encodingFormat = encoding;
#ifdef _WIN32
        const ma_result opened = ma_decoder_init_file_w(file.c_str(), &config, &decoder_);
#else
        const ma_result opened = ma_decoder_init_file(file.c_str(), &config, &decoder_);
#endif
        if (opened != MA_SUCCESS) return std::unexpected(error_for(opened, "opening the decoder"));
        initialized_ = true;

        ma_format format = ma_format_unknown;
        ma_uint32 channels = 0;
        ma_uint32 sample_rate = 0;
        const ma_result described = ma_decoder_get_data_format(&decoder_, &format, &channels, &sample_rate, nullptr, 0);
        if (described != MA_SUCCESS || channels == 0 || sample_rate == 0) {
            return fail(ErrorCode::CorruptAudio, "decoder reported no audio format");
        }
        channels_ = static_cast<int>(channels);
        sample_rate_ = static_cast<int>(sample_rate);
        codec_ = probe.codec;
        determine_length(encoding, probe);
        return {};
    }

    int channels() const override { return channels_; }
    int sample_rate() const override { return sample_rate_; }
    std::optional<std::int64_t> total_frames() const override { return total_frames_; }
    bool frames_exact() const override { return frames_exact_; }
    std::int64_t position() const override { return position_; }
    std::string codec() const override { return codec_; }

    Result<std::size_t> read(float* out, std::size_t frames) override {
        if (frames == 0) return std::size_t{0};
        ma_uint64 frames_read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder_, out, frames, &frames_read);
        if (result != MA_SUCCESS && result != MA_AT_END) return std::unexpected(error_for(result, "decoding"));
        position_ += static_cast<std::int64_t>(frames_read);
        return static_cast<std::size_t>(frames_read);
    }

    Result<void> seek(std::int64_t frame) override {
        if (frame < 0) return fail(ErrorCode::InvalidArgument, "seek position is negative");
        if (frames_exact_ && total_frames_ && frame > *total_frames_) {
            return fail(ErrorCode::InvalidArgument, "seek position is past the end of the stream");
        }
        const ma_result result = ma_decoder_seek_to_pcm_frame(&decoder_, static_cast<ma_uint64>(frame));
        if (result != MA_SUCCESS) return std::unexpected(error_for(result, "seeking"));
        position_ = frame;
        return {};
    }

private:
    /// WAV and FLAC lengths come from headers. For MP3, asking dr_mp3 without a Xing/Info frame
    /// count would scan the whole file, so the probe's exact count or estimate is used instead.
    void determine_length(ma_encoding_format encoding, const ProbeResult& probe) {
        if (encoding == ma_encoding_format_mp3) {
            if (probe.frames) {
                total_frames_ = probe.frames;
                frames_exact_ = true;
            } else if (probe.duration_us) {
                total_frames_ = *probe.duration_us * sample_rate_ / 1'000'000;
            }
            return;
        }
        ma_uint64 length = 0;
        if (ma_decoder_get_length_in_pcm_frames(&decoder_, &length) != MA_SUCCESS) return;
        if (length == 0 && encoding == ma_encoding_format_flac) return;  // STREAMINFO total 0: unknown
        total_frames_ = static_cast<std::int64_t>(length);
        frames_exact_ = true;
    }

    ma_decoder decoder_{};
    bool initialized_ = false;
    int channels_ = 0;
    int sample_rate_ = 0;
    std::optional<std::int64_t> total_frames_;
    bool frames_exact_ = false;
    std::int64_t position_ = 0;
    std::string codec_;
};

}  // namespace

bool miniaudio_can_decode(const ProbeResult& probe) {
    return encoding_for(probe) != ma_encoding_format_unknown;
}

std::vector<std::string> miniaudio_codecs() {
    std::vector<std::string> codecs(kWaveCodecs.begin(), kWaveCodecs.end());
    codecs.emplace_back("flac");
    codecs.insert(codecs.end(), kMpegCodecs.begin(), kMpegCodecs.end());
    return codecs;
}

Result<std::unique_ptr<Decoder>> open_miniaudio_decoder(const std::filesystem::path& file, const ProbeResult& probe) {
    auto decoder = std::make_unique<MiniaudioDecoder>();
    if (auto opened = decoder->open(file, probe); !opened) return std::unexpected(opened.error());
    return std::unique_ptr<Decoder>(std::move(decoder));
}

}  // namespace mso::media::detail
