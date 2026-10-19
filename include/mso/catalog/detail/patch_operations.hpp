#pragma once
/// @file
/// Reversible, compare-and-set patch operations: the unit of durable undo/redo
/// (docs/03_DATA_AND_SEARCH.md §9). Every operation records the value before and after; applying
/// it in either direction first checks that the current value equals the expected one, so an
/// undo never clobbers a later edit (HISTORY_CONFLICT instead). Internal to the catalog module.
#include <cstdint>
#include <span>
#include <string_view>

#include "mso/catalog/detail/catalog_internal.hpp"

namespace mso::catalog::detail {

/// Which way an operation is applied.
enum class PatchDirection { Forward, Inverse };

/// Builders for the stored operation shapes ("before"/"after" are the compare-and-set values):
///   field           {sample, field, before, after}   field ∈ display_name, rating, favorite, notes,
///                                                    category_id, bpm_override, key_override, deleted
///   tag_link        {sample, tag, before: bool, after: bool}
///   tag_links       {tag, samples: [id…], before: bool, after: bool}   (set-based, version 2)
///   collection_link {collection, sample, before: ordinal|null, after: ordinal|null}
///   tag_row         {tag, field, before, after}         field ∈ name, parent_id, alias_of, deleted
///   category_row    {category, field, before, after}    field ∈ name, parent_id, deleted
///   collection_row  {collection, field, before, after}  field ∈ name, deleted
namespace patch {
/// A sample field change.
nlohmann::json field(std::int64_t sample_id, std::string_view field, nlohmann::json before, nlohmann::json after);
/// One sample's membership in one tag.
nlohmann::json tag_link(std::int64_t sample_id, std::int64_t tag_id, bool before, bool after);
/// Many samples' membership in one tag, applied with set-based statements.
nlohmann::json tag_links(std::int64_t tag_id, std::span<const std::int64_t> sample_ids, bool before, bool after);
/// A sample's position in a manual collection (null = not a member).
nlohmann::json collection_link(std::int64_t collection_id, std::int64_t sample_id, nlohmann::json before,
                               nlohmann::json after);
/// A field of a tag row.
nlohmann::json tag_row(std::int64_t tag_id, std::string_view field, nlohmann::json before, nlohmann::json after);
/// A field of a category row.
nlohmann::json category_row(std::int64_t category_id, std::string_view field, nlohmann::json before,
                            nlohmann::json after);
/// A field of a collection row.
nlohmann::json collection_row(std::int64_t collection_id, std::string_view field, nlohmann::json before,
                              nlohmann::json after);
}  // namespace patch

/// Side effects of applied operations that the outbox envelope needs beyond the changed samples
/// (those are recorded in the change tracking table as operations apply).
struct PatchEffects {
    bool structure_changed = false;  ///< tags, categories, collections or memberships changed
};

/// Reads the current value an operation compares against. DB_CORRUPT for a malformed operation.
/// For `tag_links` the value is true/false when all samples agree, null when they are mixed.
Result<nlohmann::json> read_current_value(sqlite::Connection& connection, const nlohmann::json& operation);

/// Applies an operation in the given direction after the compare-and-set check. Returns
/// HISTORY_CONFLICT (with the offending operation in the detail) when the current value differs from
/// the expected one, DB_CORRUPT when the stored operation is malformed. Changed samples are recorded
/// with mark_sample_changed; structural effects are reported in `effects`.
Result<void> apply_operation(sqlite::Connection& connection, const nlohmann::json& operation, PatchDirection direction,
                             PatchEffects& effects);

}  // namespace mso::catalog::detail
