#pragma once
/// @file
/// Strict, locale-independent parsing of the values the command line accepts as text.
#include <cstdint>
#include <string_view>

#include "mso/domain/error.hpp"

namespace mso::cli {

/// Parses a BPM value ("128", "92.5") into milli-BPM: the whole text must be a plain decimal
/// number (no sign, exponent or unit), finite and within 1..999 BPM. Locale-independent
/// (std::from_chars). INVALID_ARGUMENT otherwise.
[[nodiscard]] Result<std::int64_t> parse_bpm_milli(std::string_view text);

}  // namespace mso::cli
