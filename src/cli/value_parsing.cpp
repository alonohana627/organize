#include "mso/cli/value_parsing.hpp"

#include <charconv>
#include <cmath>
#include <format>

namespace mso::cli {

namespace {

/// Accepted BPM range of an override, in BPM.
constexpr double kMinimumBpm = 1.0;
constexpr double kMaximumBpm = 999.0;

}  // namespace

Result<std::int64_t> parse_bpm_milli(std::string_view text) {
    double value = 0;
    const auto* end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value, std::chars_format::fixed);
    if (text.empty() || parsed.ec != std::errc() || parsed.ptr != end || !std::isfinite(value) || value < kMinimumBpm ||
        value > kMaximumBpm)
        return fail(ErrorCode::InvalidArgument,
                    std::format("'{}' is not a BPM between {} and {}", text, kMinimumBpm, kMaximumBpm));
    return static_cast<std::int64_t>(std::llround(value * 1000.0));
}

}  // namespace mso::cli
