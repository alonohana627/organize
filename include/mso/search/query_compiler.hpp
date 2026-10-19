#pragma once
/// @file
/// Compiles a parsed Query into a parameterized SQL predicate over `samples s LEFT JOIN
/// effective_metadata e`. Values are always bound parameters; FTS terms are escaped phrases.
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
    std::string sql = "1";
    std::vector<SqlValue> parameters;
};

/// Binds the parameters of a predicate starting at parameter index `first` (1-based); returns
/// the next free index.
int bind_parameters(sqlite::PreparedStatement& statement, const std::vector<SqlValue>& parameters, int first);

/// Escapes one term as an FTS5 string (doubled quotes), with an optional column filter and prefix.
std::string fts_expression(const TextTerm& term);

/// Compiles a query for one library. Tag/category/collection names are resolved through the
/// given read connection; unknown names compile to a predicate that matches nothing.
Result<SqlPredicate> compile_query(const Query& query, sqlite::Connection& connection);

/// SQL expression that is true when a sample has a playable location (present, root online).
std::string_view availability_sql();

}  // namespace mso::search
