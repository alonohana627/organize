#pragma once
/// @file
/// Canonical decoded-PCM hash (algorithm AlgorithmVersions::kPcmHash).
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace mso::analysis {

/// Converts a decoded float sample to signed 24-bit: round(sample * 2^23) clamped to
/// [-2^23, 2^23 - 1]. NaN maps to 0. An integer source of up to 24 bits decodes to floats that
/// are exact multiples of 2^-23, so its samples survive this conversion bit-exactly.
std::int32_t quantize_to_s24(float sample) noexcept;

/// Streams decoded audio into a SHA-256 over a canonical representation:
///
///     "sha256-pcm-s24-1" | u32 LE channels | u32 LE sample rate
///     | interleaved samples as 3-byte little-endian two's complement
///     | u64 LE frame count
///
/// The same audio stored as 16-bit, 24-bit or float WAV therefore hashes equal, while any change
/// of a sample, the sample rate, the channel count or the length changes the hash. Floats with
/// more than 24 significant bits collapse to their nearest 24-bit value. The SHA-256 state lives
/// behind a pointer so this header does not expose picosha2.
class PcmHasher {
public:
    PcmHasher(int channels, int sample_rate);
    ~PcmHasher();
    PcmHasher(PcmHasher&&) noexcept;
    PcmHasher& operator=(PcmHasher&&) noexcept;
    PcmHasher(const PcmHasher&) = delete;
    PcmHasher& operator=(const PcmHasher&) = delete;

    /// Adds interleaved samples (whole frames; the caller never splits a frame across calls).
    void add(std::span<const float> interleaved_samples);

    /// Finalizes and returns the lowercase hex digest. The hasher must not be used afterwards.
    std::string finish();

private:
    class Digest;  ///< the picosha2 streaming state

    std::unique_ptr<Digest> digest_;
    int channels_;
    std::uint64_t sample_count_ = 0;
    std::vector<std::uint8_t> packed_samples_;  ///< reused scratch for one block of 24-bit samples
};

}  // namespace mso::analysis
