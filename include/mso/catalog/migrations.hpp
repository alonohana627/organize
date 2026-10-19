#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace mso::catalog {

struct Migration {
    int version;
    std::string_view name;
    std::string_view sql;
    std::string checksum;  // SHA-256 of sql, computed at startup
};

const std::vector<Migration>& migrations();
int latest_schema_version();

}  // namespace mso::catalog
