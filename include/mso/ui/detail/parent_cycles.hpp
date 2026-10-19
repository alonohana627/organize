#pragma once
/// @file
/// Finding the members of parent cycles in a flat parent-id list (navigator trees of a damaged
/// catalog: folder A's parent is B and B's is A).
#include <cstdint>
#include <map>
#include <optional>
#include <set>

namespace mso::ui::detail {

/// Ids in `parent_of` (id -> parent id, nullopt for a top-level node) that lie on a parent cycle,
/// self-parenting included. A node whose ancestors lead into a cycle without being part of it is
/// not reported: once the cycle's members are attached to the top level, its chain ends there.
/// Parents that are not keys of `parent_of` end a chain. Each id is visited once (linear time).
std::set<std::int64_t> nodes_on_parent_cycles(const std::map<std::int64_t, std::optional<std::int64_t>>& parent_of);

}  // namespace mso::ui::detail
