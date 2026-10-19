/// @file
/// Versioned little-endian binary encoding of waveform overviews (derived/waveforms segment files).
///
/// Layout, all integers and floats little-endian:
///
///     offset  size  field
///          0     8  magic "MSOWAVE\0"
///          8     4  u32 format version (1)
///         12     4  u32 flags (bit 0: complete; other bits must be zero)
///         16     4  u32 bin count N
///         20     4  u32 sample rate
///         24     8  i64 frames per bin
///         32     8  i64 frames covered
///         40     8  i64 total frames (0 = unknown)
///         48   4*N  f32 minimum per bin
///              4*N  f32 maximum per bin
///              4*N  f32 RMS per bin
///      end-32   32  SHA-256 of every preceding byte
#include <picosha2.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

#include "mso/analysis/analysis.hpp"
#include "mso/analysis/byte_order.hpp"
#include "mso/analysis/limits.hpp"

namespace mso::analysis {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'M', 'S', 'O', 'W', 'A', 'V', 'E', '\0'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kFlagComplete = 1U << 0;
constexpr std::uint32_t kKnownFlags = kFlagComplete;
constexpr std::size_t kHeaderSize = 48;
constexpr std::size_t kChecksumSize = 32;
constexpr std::size_t kBytesPerBin = 3 * sizeof(float);

/// Appends the segment's fixed-width fields to a byte vector (byte order from byte_order.hpp).
class SegmentWriter {
public:
    explicit SegmentWriter(std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    void write_bytes(std::span<const std::uint8_t> data) { bytes_.insert(bytes_.end(), data.begin(), data.end()); }
    void write_u32(std::uint32_t value) { append_little_endian(bytes_, value); }
    void write_i64(std::int64_t value) { append_little_endian(bytes_, static_cast<std::uint64_t>(value)); }
    void write_f32(float value) { write_u32(std::bit_cast<std::uint32_t>(value)); }

private:
    std::vector<std::uint8_t>& bytes_;
};

/// Reads the segment's fixed-width fields from a byte span whose size the caller has validated.
class SegmentReader {
public:
    explicit SegmentReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    std::span<const std::uint8_t> read_bytes(std::size_t count) {
        const auto data = bytes_.subspan(offset_, count);
        offset_ += count;
        return data;
    }
    std::uint32_t read_u32() { return load_little_endian<std::uint32_t>(read_bytes(4).first<4>()); }
    std::int64_t read_i64() {
        return static_cast<std::int64_t>(load_little_endian<std::uint64_t>(read_bytes(8).first<8>()));
    }
    float read_f32() { return std::bit_cast<float>(read_u32()); }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};

/// SHA-256 of `bytes`, the segment's trailing checksum.
std::array<std::uint8_t, 32> checksum_of(std::span<const std::uint8_t> bytes) {
    std::array<std::uint8_t, 32> digest{};
    picosha2::hash256(bytes.begin(), bytes.end(), digest.begin(), digest.end());
    return digest;
}

/// values[index], or NaN when a per-bin vector is shorter than `minimum` (an inconsistent overview),
/// so the encoded file fails validation instead of silently dropping data.
float value_or_nan(const std::vector<float>& values, std::size_t index) {
    return index < values.size() ? values[index] : std::numeric_limits<float>::quiet_NaN();
}

/// Every bin finite, peaks within [-1, 1] with minimum <= maximum, RMS within [0, 1].
Result<void> validate_bins(const WaveformOverview& overview) {
    for (std::size_t bin = 0; bin < overview.minimum.size(); ++bin) {
        const float minimum = overview.minimum[bin];
        const float maximum = overview.maximum[bin];
        const float rms = overview.rms[bin];
        if (!std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(rms))
            return fail(ErrorCode::ParseError, "waveform bin value is not finite");
        if (minimum < -1.0F || maximum > 1.0F || minimum > maximum)
            return fail(ErrorCode::ParseError, "waveform bin peaks out of range");
        if (rms < 0.0F || rms > 1.0F) return fail(ErrorCode::ParseError, "waveform bin RMS out of range");
    }
    return {};
}

/// Header fields consistent with each other and with the bin count.
Result<void> validate_counts(const WaveformOverview& overview) {
    const auto bin_count = static_cast<std::int64_t>(overview.minimum.size());
    if (overview.sample_rate <= 0 || overview.sample_rate > kMaximumSampleRate)
        return fail(ErrorCode::ParseError, "waveform sample rate out of range");
    if (overview.frames_per_bin < 1) return fail(ErrorCode::ParseError, "waveform frames per bin must be positive");
    if (overview.frames_covered < 0 || overview.total_frames < 0)
        return fail(ErrorCode::ParseError, "waveform frame counts must not be negative");
    // Bins tile the covered frames exactly: bin_count == ceil(frames_covered / frames_per_bin).
    const std::int64_t expected_bins =
        overview.frames_covered / overview.frames_per_bin + (overview.frames_covered % overview.frames_per_bin != 0);
    if (bin_count != expected_bins) return fail(ErrorCode::ParseError, "waveform bin count does not match frames");
    if (overview.complete && overview.total_frames != overview.frames_covered)
        return fail(ErrorCode::ParseError, "complete waveform must cover all frames");
    if (!overview.complete && overview.total_frames != 0 && overview.total_frames < overview.frames_covered)
        return fail(ErrorCode::ParseError, "partial waveform covers more than its total");
    return {};
}

}  // namespace

std::uint64_t encoded_waveform_size(std::size_t bins) noexcept {
    return kHeaderSize + static_cast<std::uint64_t>(bins) * kBytesPerBin + kChecksumSize;
}

std::vector<std::uint8_t> encode_waveform(const WaveformOverview& overview) {
    const std::size_t bin_count = overview.minimum.size();
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kHeaderSize + bin_count * kBytesPerBin + kChecksumSize);

    SegmentWriter writer(bytes);
    writer.write_bytes(kMagic);
    writer.write_u32(kFormatVersion);
    writer.write_u32(overview.complete ? kFlagComplete : 0U);
    writer.write_u32(static_cast<std::uint32_t>(bin_count));
    writer.write_u32(static_cast<std::uint32_t>(overview.sample_rate));
    writer.write_i64(overview.frames_per_bin);
    writer.write_i64(overview.frames_covered);
    writer.write_i64(overview.total_frames);
    for (std::size_t bin = 0; bin < bin_count; ++bin)
        writer.write_f32(overview.minimum[bin]);
    for (std::size_t bin = 0; bin < bin_count; ++bin)
        writer.write_f32(value_or_nan(overview.maximum, bin));
    for (std::size_t bin = 0; bin < bin_count; ++bin)
        writer.write_f32(value_or_nan(overview.rms, bin));

    writer.write_bytes(checksum_of(bytes));
    return bytes;
}

Result<WaveformOverview> decode_waveform(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kHeaderSize + kChecksumSize) return fail(ErrorCode::ParseError, "waveform data truncated");

    const std::span<const std::uint8_t> all_bytes(bytes);
    const auto body = all_bytes.first(bytes.size() - kChecksumSize);
    SegmentReader reader(body);
    const auto magic = reader.read_bytes(kMagic.size());
    if (!std::equal(magic.begin(), magic.end(), kMagic.begin()))
        return fail(ErrorCode::ParseError, "not a waveform file");
    if (reader.read_u32() != kFormatVersion) return fail(ErrorCode::ParseError, "unsupported waveform format version");
    const std::uint32_t flags = reader.read_u32();
    if ((flags & ~kKnownFlags) != 0) return fail(ErrorCode::ParseError, "unknown waveform flags");
    const std::uint32_t bin_count = reader.read_u32();
    if (bin_count > static_cast<std::uint32_t>(kMaximumWaveformBins))
        return fail(ErrorCode::ParseError, "waveform bin count exceeds limit");
    if (bytes.size() != kHeaderSize + std::size_t{bin_count} * kBytesPerBin + kChecksumSize)
        return fail(ErrorCode::ParseError, "waveform size does not match bin count");

    const auto stored_checksum = all_bytes.last(kChecksumSize);
    const auto computed_checksum = checksum_of(body);
    if (!std::equal(stored_checksum.begin(), stored_checksum.end(), computed_checksum.begin()))
        return fail(ErrorCode::ParseError, "waveform checksum mismatch");

    WaveformOverview overview;
    overview.complete = (flags & kFlagComplete) != 0;
    const std::uint32_t sample_rate = reader.read_u32();
    if (sample_rate == 0 || sample_rate > static_cast<std::uint32_t>(kMaximumSampleRate))
        return fail(ErrorCode::ParseError, "waveform sample rate out of range");
    overview.sample_rate = static_cast<int>(sample_rate);
    overview.frames_per_bin = reader.read_i64();
    overview.frames_covered = reader.read_i64();
    overview.total_frames = reader.read_i64();

    overview.minimum.resize(bin_count);
    overview.maximum.resize(bin_count);
    overview.rms.resize(bin_count);
    for (float& value : overview.minimum)
        value = reader.read_f32();
    for (float& value : overview.maximum)
        value = reader.read_f32();
    for (float& value : overview.rms)
        value = reader.read_f32();

    if (auto counts = validate_counts(overview); !counts) return std::unexpected(counts.error());
    if (auto values = validate_bins(overview); !values) return std::unexpected(values.error());
    return overview;
}

}  // namespace mso::analysis
