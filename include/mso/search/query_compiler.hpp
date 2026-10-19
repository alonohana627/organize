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
/// the next free index.
int bind_parameters(sqlite::PreparedStatement& statement, const std::vector<SqlValue>& parameters, int first);

/// Escapes one term as an FTS5 string (doubled quotes), with an optional column filter and prefix.
std::string fts_expression(const TextTerm& term);

/// Compiles a query for one library. Tag/category/collection names and root names are resolved
/// through the given read connection; unknown names compile to a predicate that matches nothing.
/// Query problems (an invalid smart collection, too deep or too large an expansion, an invalid
/// taxonomy path) fail with INVALID_ARGUMENT or PARSE_ERROR; database failures keep their code.
Result<SqlPredicate> compile_query(const Query& query, sqlite::Connection& connection);

/// Compiles the stored query of a collection (manual membership, or a smart collection's query)
/// as a predicate; a missing or deleted collection matches nothing.
Result<SqlPredicate> compile_collection(std::int64_t collection_id, sqlite::Connection& connection);

/// SQL expression that is true when a sample has a playable location (present, root online).
std::string_view availability_sql();

}  // namespace mso::search
