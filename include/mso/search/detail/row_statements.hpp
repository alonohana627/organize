#pragma once
/// @file
/// The SQL that hydrates sample rows (sample_rows.cpp), exposed so tests can explain its query
/// plans. Not part of the public search API.
#include <string>

namespace mso::search::detail {

/// The three statements merged into hydrated rows, each ordered by sample id (column 0).
struct RowStatements {
    std::string samples;    ///< base columns of samples joined to effective_metadata
    std::string locations;  ///< live locations, ranked per sample by kPrimaryLocationOrder
    std::string tags;       ///< tag links
};

/// The statements for the whole library, or (`filtered_by_ids`) for the ids of a JSON array bound
/// as parameter 1 of each statement.
[[nodiscard]] RowStatements row_statements(bool filtered_by_ids);

}  // namespace mso::search::detail
