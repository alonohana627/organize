#pragma once
/// @file
/// Read-only taxonomy lookups for other modules (search compiles tag/category filters with them).
#include <cstdint>
#include <optional>
#include <string_view>

#include "mso/catalog/sqlite.hpp"

namespace mso::catalog {

/// Which taxonomy tree to search.
enum class TaxonomyKind { Tags, Categories };

/// Resolves a display path ("Drums/Kick") to its node id, following tag merges to the canonical
/// tag. Returns nullopt when the path does not exist. Never creates anything.
Result<std::optional<std::int64_t>> find_taxonomy_node(sqlite::Connection& connection, TaxonomyKind kind,
                                                       std::string_view path);

}  // namespace mso::catalog
