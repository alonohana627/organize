#include "mso/domain/taxonomy.hpp"

#include "mso/domain/core.hpp"

namespace mso {

Result<std::vector<std::string>> split_taxonomy_path(std::string_view path) {
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (start <= path.size()) {
        auto end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        std::string segment = trim(path.substr(start, end - start));
        if (segment.empty()) return fail(ErrorCode::InvalidArgument, "empty tag or category name in path");
        if (!is_valid_utf8(segment)) return fail(ErrorCode::InvalidArgument, "tag or category name is not valid UTF-8");
        if (count_code_points(segment) > kMaxTaxonomyNameCodePoints)
            return fail(ErrorCode::InvalidArgument, "tag or category name longer than 128 characters");
        segments.push_back(std::move(segment));
        if (segments.size() > kMaxTaxonomyDepth)
            return fail(ErrorCode::InvalidArgument, "hierarchy deeper than 16 levels");
        start = end + 1;
    }
    return segments;
}

}  // namespace mso
