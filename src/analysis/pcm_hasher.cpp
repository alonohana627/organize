/// @file
/// Canonical signed 24-bit PCM hashing.
#include "mso/analysis/pcm_hasher.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "mso/analysis/analysis.hpp"

namespace mso::analysis {
namespace {

constexpr double kFullScale = 8388608.0;  ///< 2^23
constexpr std::int32_t kMinimumS24 = -8388608;
constexpr std::int32_t kMaximumS24 = 8388607;

template <std::size_t ByteCount>
std::array<std::uint8_t, ByteCount> little_endian(std::uint64_t value) {
    std::array<std::uint8_t, ByteCount> bytes{};
    for (std::size_t byte = 0; byte < ByteCount; ++byte)
        bytes[byte] = static_cast<std::uint8_t>(value >> (8 * byte));
    return bytes;
}

}  // namespace

std::int32_t quantize_to_s24(float sample) noexcept {
    if (std::isnan(sample)) return 0;
    const double scaled = std::round(static_cast<double>(sample) * kFullScale);
    if (scaled <= kMinimumS24) return kMinimumS24;
    if (scaled >= kMaximumS24) return kMaximumS24;
    return static_cast<std::int32_t>(scaled);
}

PcmHasher::PcmHasher(int channels, int sample_rate) : channels_(std::max(channels, 1)) {
    const std::string_view domain = AlgorithmVersions::kPcmHash;
    hasher_.process(domain.begin(), domain.end());
    const auto channel_bytes = little_endian<4>(static_cast<std::uint32_t>(channels));
    const auto rate_bytes = little_endian<4>(static_cast<std::uint32_t>(sample_rate));
    hasher_.process(channel_bytes.begin(), channel_bytes.end());
    hasher_.process(rate_bytes.begin(), rate_bytes.end());
}

void PcmHasher::add(std::span<const float> interleaved_samples) {
    packed_samples_.resize(interleaved_samples.size() * 3);
    std::size_t offset = 0;
    for (const float sample : interleaved_samples) {
        const auto bits = static_cast<std::uint32_t>(quantize_to_s24(sample));
        packed_samples_[offset++] = static_cast<std::uint8_t>(bits);
        packed_samples_[offset++] = static_cast<std::uint8_t>(bits >> 8);
        packed_samples_[offset++] = static_cast<std::uint8_t>(bits >> 16);
    }
    hasher_.process(packed_samples_.begin(), packed_samples_.end());
    sample_count_ += interleaved_samples.size();
}

std::string PcmHasher::finish() {
    const auto frame_bytes = little_endian<8>(sample_count_ / static_cast<std::uint64_t>(channels_));
    hasher_.process(frame_bytes.begin(), frame_bytes.end());
    hasher_.finish();
    return picosha2::get_hash_hex_string(hasher_);
}

}  // namespace mso::analysis
