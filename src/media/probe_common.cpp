#include "mso/media/detail/probe_formats.hpp"

#include <cstdint>
#include <limits>

namespace mso::media::detail {

std::optional<std::string> pcm_codec_name(std::int64_t bits_per_sample, bool is_float, bool is_unsigned) {
    if (is_float) {
        if (bits_per_sample == 32) return "pcm_f32";
        if (bits_per_sample == 64) return "pcm_f64";
        return std::nullopt;
    }
    if (bits_per_sample < 1 || bits_per_sample > 32) return std::nullopt;
    if (bits_per_sample <= 8) return is_unsigned ? "pcm_u8" : "pcm_s8";
    if (bits_per_sample <= 16) return "pcm_s16";
    if (bits_per_sample <= 24) return "pcm_s24";
    return "pcm_s32";
}

Result<void> set_stream_layout(ProbeResult& result, std::int64_t channels, std::int64_t sample_rate) {
    if (channels < 1 || channels > kMaxChannels) return fail(ErrorCode::CorruptAudio, "invalid channel count");
    if (sample_rate < 1 || sample_rate > kMaxSampleRate) return fail(ErrorCode::CorruptAudio, "invalid sample rate");
    result.channels = channels;
    result.sample_rate = sample_rate;
    return {};
}

std::optional<std::int64_t> units_to_microseconds(std::uint64_t units, std::uint64_t units_per_second) noexcept {
    constexpr std::uint64_t kMicrosecondsPerSecond = 1'000'000;
    if (units_per_second == 0) return std::nullopt;
    const std::uint64_t whole_seconds = units / units_per_second;
    const std::uint64_t remainder = units % units_per_second;
    const auto whole = checked_multiply(whole_seconds, kMicrosecondsPerSecond);
    // remainder < units_per_second <= 2^64, so do the fractional part in 128 bits.
    const auto fraction = static_cast<std::uint64_t>(static_cast<unsigned __int128>(remainder) *
                                                     kMicrosecondsPerSecond / units_per_second);
    const auto total = whole ? checked_add(*whole, fraction) : std::nullopt;
    if (!total || *total > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
    return static_cast<std::int64_t>(*total);
}

Result<void> parse_flac_streaminfo(Bytes streaminfo, ProbeResult& result) {
    constexpr std::size_t kStreamInfoSize = 34;
    if (streaminfo.size() < kStreamInfoSize) return fail(ErrorCode::CorruptAudio, "FLAC STREAMINFO is too short");
    // Bytes 10..17 pack: sample rate (20 bits), channels-1 (3), bits per sample-1 (5), total samples (36).
    ByteCursor cursor(streaminfo.subspan(10, 8));
    const std::uint64_t packed = cursor.u64_be();
    const auto sample_rate = static_cast<std::int64_t>(packed >> 44);
    const auto channels = static_cast<std::int64_t>((packed >> 41) & 0x7) + 1;
    const auto bits_per_sample = static_cast<std::int64_t>((packed >> 36) & 0x1F) + 1;
    const std::uint64_t total_samples = packed & 0xFFFFFFFFFull;
    if (auto layout = set_stream_layout(result, channels, sample_rate); !layout) return layout;
    result.codec = "flac";
    result.bit_depth = bits_per_sample;
    if (total_samples > 0) result.frames = static_cast<std::int64_t>(total_samples);  // 0 means unknown
    return {};
}

}  // namespace mso::media::detail
