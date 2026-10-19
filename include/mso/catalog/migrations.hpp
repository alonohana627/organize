#pragma once
/// @file
/// Numbered catalog schema migrations. Released migrations are immutable (invariant 15): their
/// SHA-256 checksums are recorded in schema_migrations when applied, verified on every open, and
/// pinned by a test so an accidental edit fails the build's tests instead of users' libraries.
#include <string>
#include <string_view>
#include <vector>

namespace mso::catalog {

/// One schema migration.
struct Migration {
    int version = 0;        ///< 1, 2, 3, … without gaps
    std::string_view name;  ///< short description
    std::string_view sql;   ///< the script, applied in one transaction
    std::string checksum;   ///< lowercase hex SHA-256 of `sql`, computed once at startup
};

/// Every migration this build ships, in version order.
const std::vector<Migration>& migrations();
/// Version of the newest migration.
int latest_schema_version();

}  // namespace mso::catalog
