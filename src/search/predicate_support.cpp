#include "mso/search/detail/predicate_support.hpp"

#include <nlohmann/json.hpp>

#include "mso/domain/core.hpp"
#include "mso/domain/taxonomy.hpp"

namespace mso::search::detail {

namespace {

/// Removes trailing '/' separators (path keys already turned Windows '\' into '/').
std::string without_trailing_slashes(std::string key) {
    while (key.ends_with('/'))
        key.pop_back();
    return key;
}

/// Splits a path key on '/'.
std::vector<std::string_view> split_on_slash(std::string_view key) {
    std::vector<std::string_view> segments;
    std::size_t start = 0;
    while (true) {
        const auto end = key.find('/', start);
        segments.push_back(key.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) return segments;
        start = end + 1;
    }
}

}  // namespace

RootFolderMatch match_folder_in_root(std::string_view value, std::string_view root_name, bool root_case_sensitive) {
    RootFolderMatch match;
    const std::string folded_value = without_trailing_slashes(fold_path_key(value));
    if (folded_value.empty()) return match;
    const std::string exact_value = without_trailing_slashes(path_key(value, true));

    // A folder anywhere below the root, compared with the root's own case sensitivity.
    match.prefixes.push_back((root_case_sensitive ? exact_value : folded_value) + '/');

    // The root itself, or a folder addressed as "<root name>/<folder>".
    const std::string folded_root = without_trailing_slashes(fold_path_key(root_name));
    if (folded_root.empty()) return match;
    if (folded_value == folded_root) {
        match.whole_root = true;
    } else if (folded_value.starts_with(folded_root + '/')) {
        // Folding maps code points one to one and keeps '/', so the exact value has the same
        // segments; drop the ones that spelled the root name.
        const std::size_t root_segments = split_on_slash(folded_root).size();
        const auto exact_segments = split_on_slash(exact_value);
        std::string rest;
        for (std::size_t index = root_segments; index < exact_segments.size(); ++index) {
            if (!rest.empty()) rest += '/';
            rest += exact_segments[index];
        }
        const std::string rest_key = without_trailing_slashes(path_key(rest, root_case_sensitive));
        if (!rest_key.empty()) match.prefixes.push_back(rest_key + '/');
    }
    return match;
}

std::string prefix_upper_bound(std::string_view prefix) {
    std::string upper(prefix);
    if (!upper.empty()) upper.back() = static_cast<char>('/' + 1);
    return upper;
}

std::optional<std::vector<std::string>> folded_taxonomy_segments(std::string_view path) {
    auto segments = split_taxonomy_path(path);
    if (!segments) return std::nullopt;
    for (auto& segment : *segments)
        segment = fold_key(segment);
    return std::move(*segments);
}

std::vector<std::string> folded_display_segments(std::string_view path) {
    std::vector<std::string> segments;
    for (const auto segment : split_on_slash(path))
        segments.push_back(fold_key(segment));
    return segments;
}

std::optional<std::string> smart_collection_text(std::string_view query_json) {
    const auto document = nlohmann::json::parse(query_json, nullptr, false);
    if (!document.is_object()) return std::nullopt;
    const auto text = document.find("text");
    if (text == document.end() || !text->is_string()) return std::nullopt;
    return text->get<std::string>();
}

std::string id_array_json(const std::vector<std::int64_t>& ids) {
    std::string json = "[";
    for (const auto id : ids) {
        if (json.size() > 1) json += ',';
        json += std::to_string(id);
    }
    json += ']';
    return json;
}

}  // namespace mso::search::detail
