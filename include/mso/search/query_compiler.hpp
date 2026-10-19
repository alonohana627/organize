#pragma once
/// @file
/// Compiles a parsed Query into a parameterized SQL predicate over `samples s LEFT JOIN
/// effective_metadata e`. Values are always bound parameters; FTS terms are escaped phrases.
/// The in-memory evaluator (`row_matcher.hpp`) implements the same semantics for SampleRows.
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "mso/catalog/sqlite.hpp"
#include "mso/domain/core.hpp"
#include "mso/search/query.hpp"

namespace mso::search {

/// A bound SQL parameter value.
using SqlValue = std::variant<std::int64_t, std::string>;

/// A SQL boolean expression and its positional parameters, in order of appearance.
struct SqlPredicate {
    std::string sql = "1";             ///< expression over `s` (samples) and `e` (effective_metadata)
    std::vector<SqlValue> parameters;  ///< values for its '?' placeholders, in order
};

/// Most nodes a query may expand to once smart collections (`collection:` and the collection
/// scope) are inlined; guards against exponential growth through nested smart collections.
inline constexpr std::size_t kMaxCompiledNodes = 1024;

/// Which library, at which catalog revision, a compilation reads (Library::identity()). With it the
/// compiler reuses choices that depend only on the library's data across calls: the plan of a
/// `folder:` value or a root/folder scope (primary_location_filter) is chosen once per library,
/// revision, library size and condition instead of once per page.
struct LibraryRevision {
    uuids::uuid library;                ///< the library the connection reads
    std::int64_t catalog_revision = 0;  ///< its catalog revision as this process last saw it
};

/// Binds the parameters of a predicate starting at parameter index `first` (1-based); returns
/// the next free index (ignoring it is fine after the last parameters of a statement).
int bind_parameters(sqlite::PreparedStatement& statement, const std::vector<SqlValue>& parameters, int first);

/// Escapes one term as an FTS5 string (doubled quotes), with an optional column filter and prefix.
[[nodiscard]] std::string fts_expression(const TextTerm& term);

/// Compiles a query for one library. Tag/category/collection names and root names are resolved
/// through the given read connection; unknown names compile to a predicate that matches nothing.
/// A merged tag still matches by its old name (`tag:Percusion` after merging it into "Drums").
///
/// Errors: a problem with the query text itself (an invalid taxonomy path) fails with
/// INVALID_ARGUMENT. A problem with a smart collection's *stored* query is a problem of this
/// library, not of the request, so it never uses a request error code: an unparseable or invalid
/// stored query fails with DB_CORRUPT, and an expansion that nests too deeply or grows past
/// kMaxCompiledNodes with RESOURCE_LIMIT. Database failures keep their code.
///
/// `revision` (optional) lets folder plans be reused across calls (see LibraryRevision); without
/// it every `folder:` value counts its candidates again.
[[nodiscard]] Result<SqlPredicate> compile_query(const Query& query, sqlite::Connection& connection,
                                                 const std::optional<LibraryRevision>& revision = std::nullopt);

/// Compiles the stored query of a collection (manual membership, or a smart collection's query)
/// as a predicate; a missing or deleted collection matches nothing. Errors as for compile_query
/// (every query problem here is a stored one); `revision` as for compile_query.
[[nodiscard]] Result<SqlPredicate> compile_collection(std::int64_t collection_id, sqlite::Connection& connection,
                                                      const std::optional<LibraryRevision>& revision = std::nullopt);

/// A sample is matched by a location filter when this share of the library (1 / 16) or more of
/// live locations satisfy it: see primary_location_filter.
inline constexpr std::int64_t kIndexDrivenLocationShare = 16;

/// SQL that is true when the *primary* location (detail::kPrimaryLocationOrder) of sample `s.id`
/// satisfies `location_condition`, an expression over the location alias `fl` (`folder:`, the root
/// and folder scopes). Both shapes it can produce select exactly the same samples; the cheaper one
/// is chosen from the number of candidate locations, counted on the live (root_id, path_key) index
/// and capped at the threshold, against the library size (its largest sample id):
/// - selective (fewer than 1/kIndexDrivenLocationShare): `s.id IN (candidates that are primary)`,
///   evaluated once from index range scans, so a small folder never scans the library;
/// - broad: a per-sample `EXISTS` on the primary location, which streams in the requested order, so
///   the first page of a large folder stops early instead of materializing every candidate.
///
/// The count reads up to 1/16 of the library's locations. With `revision`, the choice is
/// remembered in a small process-wide cache keyed by library, revision, library size and
/// condition, so paging through one folder counts once; a stale choice only costs time, never
/// changes the result.
[[nodiscard]] Result<SqlPredicate> primary_location_filter(
    sqlite::Connection& connection, const SqlPredicate& location_condition,
    const std::optional<LibraryRevision>& revision = std::nullopt);

/// SQL expression that is true when a sample has a playable location (present, root online).
[[nodiscard]] std::string_view availability_sql();

}  // namespace mso::search
