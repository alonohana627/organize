/// @file
/// Canonical signed 24-bit PCM hashing.
#include "mso/analysis/pcm_hasher.hpp"

#include <picosha2.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <string_view>

#include "mso/analysis/analysis.hpp"
#include "mso/analysis/byte_order.hpp"

namespace mso::analysis {
namespace {

constexpr double kFullScale = 8388608.0;  ///< 2^23
constexpr std::int32_t kMinimumS24 = -8388608;
constexpr std::int32_t kMaximumS24 = 8388607;
constexpr std::size_t kBytesPerSample = 3;

}  // namespace

/// The incremental SHA-256 state.
class PcmHasher::Digest {
public:
    /// Feeds bytes into the hash.
    void process(std::span<const std::uint8_t> bytes) { hasher_.process(bytes.begin(), bytes.end()); }

    /// Finalizes and returns the lowercase hex digest.
    std::string finish() {
        hasher_.finish();
        return picosha2::get_hash_hex_string(hasher_);
    }

private:
    picosha2::hash256_one_by_one hasher_;
};

std::int32_t quantize_to_s24(float sample) noexcept {
    if (std::isnan(sample)) return 0;
    const double scaled = std::round(static_cast<double>(sample) * kFullScale);
    if (scaled <= kMinimumS24) return kMinimumS24;
    if (scaled >= kMaximumS24) return kMaximumS24;
    return static_cast<std::int32_t>(scaled);
}

PcmHasher::PcmHasher(int channels, int sample_rate)
    : digest_(std::make_unique<Digest>()), channels_(std::max(channels, 1)) {
    const std::string_view domain = AlgorithmVersions::kPcmHash;
    digest_->process(std::span(reinterpret_cast<const std::uint8_t*>(domain.data()), domain.size()));
    digest_->process(little_endian_bytes(static_cast<std::uint32_t>(channels)));
    digest_->process(little_endian_bytes(static_cast<std::uint32_t>(sample_rate)));
}

PcmHasher::~PcmHasher() = default;
PcmHasher::PcmHasher(PcmHasher&&) noexcept = default;
PcmHasher& PcmHasher::operator=(PcmHasher&&) noexcept = default;

void PcmHasher::add(std::span<const float> interleaved_samples) {
    packed_samples_.resize(interleaved_samples.size() * kBytesPerSample);
    auto destination = packed_samples_.begin();
    for (const float sample : interleaved_samples) {
        // Two's complement in 32 bits; the low three bytes are the 24-bit value.
        const auto bytes = little_endian_bytes(static_cast<std::uint32_t>(quantize_to_s24(sample)));
        destination = std::copy_n(bytes.begin(), kBytesPerSample, destination);
    }
    digest_->process(packed_samples_);
    sample_count_ += interleaved_samples.size();
}

std::string PcmHasher::finish() {
    digest_->process(little_endian_bytes(sample_count_ / static_cast<std::uint64_t>(channels_)));
    return digest_->finish();
}

}  // namespace mso::analysis
