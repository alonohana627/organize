#pragma once
/// @file
/// Compiles a parsed Query into a parameterized SQL predicate over `samples s LEFT JOIN
/// effective_metadata e`. Values are always bound parameters; FTS terms are escaped phrases.
/// The in-memory evaluator (`row_matcher.hpp`) implements the same semantics for SampleRows.
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "mso/catalog/sqlite.hpp"
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
[[nodiscard]] Result<SqlPredicate> compile_query(const Query& query, sqlite::Connection& connection);

/// Compiles the stored query of a collection (manual membership, or a smart collection's query)
/// as a predicate; a missing or deleted collection matches nothing. Errors as for compile_query
/// (every query problem here is a stored one).
[[nodiscard]] Result<SqlPredicate> compile_collection(std::int64_t collection_id, sqlite::Connection& connection);

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
[[nodiscard]] Result<SqlPredicate> primary_location_filter(sqlite::Connection& connection,
                                                           const SqlPredicate& location_condition);

/// SQL expression that is true when a sample has a playable location (present, root online).
[[nodiscard]] std::string_view availability_sql();

}  // namespace mso::search
