#pragma once
/// @file
/// Helpers shared by the catalog's translation units. Not part of the public catalog API.
/// Every function here runs inside a write transaction owned by the caller unless stated otherwise.
#include <cstdint>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "mso/catalog/sqlite.hpp"
#include "mso/catalog/types.hpp"

namespace mso::catalog::detail {

/// Version of outbox envelopes written by this build.
inline constexpr int kOutboxEnvelopeVersion = 1;
/// Version of history patch payloads written by this build.
inline constexpr int kHistoryCommandVersion = 1;

/// Steps a prepared statement through all rows, calling `row` for each.
Result<void> for_each_row(sqlite::PreparedStatement& statement,
                          const std::function<Result<void>(sqlite::PreparedStatement&)>& row);

/// Increments library_state.catalog_revision and appends the outbox envelope for it.
/// Returns the new revision. Must be called exactly once per committed mutation.
Result<std::int64_t> advance_revision(sqlite::Connection& connection, const nlohmann::json& envelope);

/// Recomputes the lexical search projection (and through triggers the FTS index) for samples.
/// Tombstoned samples are removed from the projection.
Result<void> refresh_search_projection(sqlite::Connection& connection, const std::set<std::int64_t>& sample_ids);

/// Tag and category trees share the same shape; this selects the table.
enum class Taxonomy { Tags, Categories };

/// Splits "Drums / Kick" into trimmed, non-empty segments; rejects depth > 16 or names > 128
/// code points.
Result<std::vector<std::string>> split_taxonomy_path(std::string_view path);

/// Resolves a path to a node id, creating missing segments when `create` is set (otherwise
/// NOT_FOUND). Tag aliases (merges) are followed to the canonical tag.
Result<std::int64_t> resolve_taxonomy_path(sqlite::Connection& connection, Taxonomy taxonomy, std::string_view path,
                                           bool create);

/// Display path ("Drums/Kick") of a node.
Result<std::string> taxonomy_path(sqlite::Connection& connection, Taxonomy taxonomy, std::int64_t id);

/// Follows tag_aliases from a tag to its canonical tag (bounded, cycle-safe).
Result<std::int64_t> canonical_tag(sqlite::Connection& connection, std::int64_t tag_id);

/// Records an open review item (conflicts, ambiguous relinks, changed content).
Result<void> add_review_item(sqlite::Connection& connection, std::string_view kind,
                             std::optional<std::int64_t> sample_id, const nlohmann::json& evidence);

/// Recomputes effective BPM/key for a sample from overrides and valid observations using the
/// precedence: user value → user unknown → container → analysis → filename → unknown.
Result<void> recompute_effective_musical_facts(sqlite::Connection& connection, std::int64_t sample_id);

}  // namespace mso::catalog::detail
