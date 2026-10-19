#pragma once
/// @file
/// Reversible, compare-and-set patch operations: the unit of durable undo/redo
/// (docs/03_DATA_AND_SEARCH.md §9). Every operation records the value before and after; applying
/// it in either direction first checks that the current value equals the expected one, so an
/// undo never clobbers a later edit (HISTORY_CONFLICT instead).
#include <cstdint>
#include <set>

#include "mso/catalog/catalog_internal.hpp"

namespace mso::catalog::detail {

/// Which way an operation is applied.
enum class PatchDirection { Forward, Inverse };

/// Kinds of operations and their JSON shape ("before"/"after" are the CAS values):
///   field           {sample, field, before, after}   field ∈ display_name, rating, favorite, notes,
///                                                    category_id, bpm_override, key_override, deleted
///   tag_link        {sample, tag, before: bool, after: bool}
///   collection_link {collection, sample, before: ordinal|null, after: ordinal|null}
///   tag_row         {tag, field, before, after}         field ∈ name, parent_id, alias_of, deleted
///   category_row    {category, field, before, after}    field ∈ name, parent_id
///   collection_row  {collection, field, before, after}  field ∈ name, deleted
namespace patch {
nlohmann::json field(std::int64_t sample_id, std::string_view field, nlohmann::json before, nlohmann::json after);
nlohmann::json tag_link(std::int64_t sample_id, std::int64_t tag_id, bool before, bool after);
nlohmann::json collection_link(std::int64_t collection_id, std::int64_t sample_id, nlohmann::json before,
                               nlohmann::json after);
nlohmann::json tag_row(std::int64_t tag_id, std::string_view field, nlohmann::json before, nlohmann::json after);
nlohmann::json category_row(std::int64_t category_id, std::string_view field, nlohmann::json before,
                            nlohmann::json after);
nlohmann::json collection_row(std::int64_t collection_id, std::string_view field, nlohmann::json before,
                              nlohmann::json after);
}  // namespace patch

/// Reads the current value an operation would compare against.
Result<nlohmann::json> read_current_value(sqlite::Connection& connection, const nlohmann::json& operation);

/// Applies an operation in the given direction after the compare-and-set check.
/// Returns HISTORY_CONFLICT (with the offending operation in the detail) when the current value
/// differs from the expected one. Samples whose search projection must be refreshed are added to
/// `affected_samples`.
Result<void> apply_operation(sqlite::Connection& connection, const nlohmann::json& operation, PatchDirection direction,
                             std::set<std::int64_t>& affected_samples);

/// Samples whose search projection depends on a tag or category subtree (for renames/moves).
Result<std::set<std::int64_t>> samples_in_subtree(sqlite::Connection& connection, Taxonomy taxonomy,
                                                  std::int64_t root_id);

}  // namespace mso::catalog::detail
