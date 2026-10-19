#pragma once
/// @file
/// Helpers shared by the catalog's translation units. Not part of the public catalog API.
/// Every function here runs inside a write transaction owned by the caller unless stated otherwise.
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "mso/catalog/detail/json_fields.hpp"
#include "mso/catalog/sqlite.hpp"
#include "mso/catalog/types.hpp"
#include "mso/domain/taxonomy.hpp"

namespace mso::catalog::detail {

/// Version of outbox envelopes written by this build. Version 2 always carries "samples" and
/// "structure"; older envelopes make consumers reload everything.
inline constexpr int kOutboxEnvelopeVersion = 2;
/// Version of history patch payloads written by this build (2 added the set-based `tag_links`).
inline constexpr int kHistoryCommandVersion = 2;
/// Oldest history payload version this build can still undo and redo.
inline constexpr int kOldestReadableHistoryCommandVersion = 1;
/// Envelopes listing more changed samples than this say "all_samples" instead of listing them.
inline constexpr std::int64_t kMaximumEnvelopeSamples = 10'000;
/// Outbox backlog cap (docs/02_ARCHITECTURE.md: 256 MiB); beyond it the oldest envelopes are pruned
/// and lagging consumers are flagged baseline-required.
inline constexpr std::int64_t kMaximumOutboxBytes = std::int64_t{256} * 1024 * 1024;
/// The backlog size is measured once every this many revisions.
inline constexpr std::int64_t kOutboxSizeCheckInterval = 256;

/// Steps a prepared statement through all rows, calling `row` for each. The statement is always
/// reset afterwards (bindings cleared), so a loop may rebind and run it again.
Result<void> for_each_row(sqlite::PreparedStatement& statement,
                          const std::function<Result<void>(sqlite::PreparedStatement&)>& row);

/// Parses a UUID stored in the catalog; DB_CORRUPT (naming `what`) when it is malformed.
Result<uuids::uuid> parse_stored_uuid(std::string_view text, std::string_view what);

// ---------------------------------------------------------------- change tracking and outbox

/// Creates the writer connection's per-transaction change tracking table (TEMP, disk-backed):
/// samples whose row data changed in the open transaction, and whether their search text did.
/// Called once after the writer connection is opened writable.
Result<void> prepare_change_tracking(sqlite::Connection& connection);

/// Records that a sample's row data changed; `search_text_changed` also queues its lexical
/// search projection (name, paths, tags, category, notes, embedded text) for refresh.
Result<void> mark_sample_changed(sqlite::Connection& connection, std::int64_t sample_id, bool search_text_changed);

/// Set-based variant for an explicit list of sample ids (bound as one JSON array).
Result<void> mark_sample_list_changed(sqlite::Connection& connection, std::span<const std::int64_t> sample_ids,
                                      bool search_text_changed);

/// Set-based variant: `select_sample_ids_sql` is a SELECT whose rows carry a column named
/// `sample_id`; `parameters` bind ?1, ?2, … inside it.
Result<void> mark_samples_changed(sqlite::Connection& connection, std::string_view select_sample_ids_sql,
                                  std::span<const std::int64_t> parameters, bool search_text_changed);

/// Recomputes the lexical search projection (and through triggers the FTS index) of every sample
/// queued by mark_sample(s)_changed, in two set-based statements. Tombstoned samples are removed.
Result<void> refresh_search_projection(sqlite::Connection& connection);

/// What one committed mutation changed, for its outbox envelope. The changed samples come from the
/// change tracking table, so no call site can forget them.
struct ChangeEnvelope {
    std::string_view kind;                              ///< e.g. "edit", "scan_batch", "root_availability"
    bool structure_changed = false;                     ///< roots/folders/tags/categories/collections changed
    nlohmann::json details = nlohmann::json::object();  ///< extra kind-specific fields
};

/// Flushes the search projection, increments library_state.catalog_revision and appends the outbox
/// envelope {kind, samples, structure, …details} for it, then clears the change tracking table.
/// Returns the new revision. Must be called exactly once per committed mutation.
Result<std::int64_t> advance_revision(sqlite::Connection& connection, const ChangeEnvelope& envelope);

// ---------------------------------------------------------------- taxonomy

/// Tag and category trees share the same shape; this selects the table.
enum class Taxonomy { Tags, Categories };

/// The table holding a taxonomy ("tags" or "categories").
std::string_view taxonomy_table(Taxonomy taxonomy) noexcept;

/// Resolves a path to a node id, creating missing segments when `create` is set (otherwise
/// NOT_FOUND). Tag aliases (merges) are followed to the canonical tag. Ids of nodes created by this
/// call are appended to `created` when it is given (so edits can record them as undoable).
Result<std::int64_t> resolve_taxonomy_path(sqlite::Connection& connection, Taxonomy taxonomy, std::string_view path,
                                           bool create, std::vector<std::int64_t>* created = nullptr);

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

// ---------------------------------------------------------------- analysis queue reachability

/// SQL condition (correlated on `jobs`) that is true when a job's sample has a present location on
/// a live root that is not offline or denied, i.e. a worker could read its audio.
inline constexpr std::string_view kJobSampleReachableSql =
    "EXISTS(SELECT 1 FROM locations reachable_location JOIN roots reachable_root "
    "ON reachable_root.id=reachable_location.root_id WHERE reachable_location.sample_id=jobs.sample_id "
    "AND reachable_location.state='present' AND reachable_root.deleted=0 "
    "AND reachable_root.availability NOT IN ('offline','denied'))";

/// Marks queued jobs whose samples (located in `root_id`) are no longer reachable as blocked.
Result<void> block_unreachable_jobs(sqlite::Connection& connection, std::int64_t root_id);

/// Returns blocked jobs whose samples (located in `root_id`) are reachable again to the queue.
Result<void> unblock_reachable_jobs(sqlite::Connection& connection, std::int64_t root_id);

}  // namespace mso::catalog::detail
