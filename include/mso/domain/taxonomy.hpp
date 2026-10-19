#pragma once
/// @file
/// Rules for hierarchical tag and category paths ("Drums/Kick/Acoustic"), shared by the catalog
/// (which stores the trees) and search (which matches paths in SQL and in memory).
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "mso/domain/error.hpp"

namespace mso {

/// Deepest tag or category hierarchy (segments in one path).
inline constexpr std::size_t kMaxTaxonomyDepth = 16;
/// Longest tag or category name (one segment), in Unicode code points.
inline constexpr std::size_t kMaxTaxonomyNameCodePoints = 128;

/// Splits a '/'-separated tag or category path into trimmed display segments. INVALID_ARGUMENT for
/// an empty segment ("a//b", "a/"), a segment longer than 128 code points or not valid UTF-8, or
/// more than 16 segments.
[[nodiscard]] Result<std::vector<std::string>> split_taxonomy_path(std::string_view path);

}  // namespace mso
