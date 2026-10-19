#include "mso/search/detail/predicate_support.hpp"

#include <format>

#include <nlohmann/json.hpp>

#include "mso/domain/core.hpp"
#include "mso/domain/json_fields.hpp"
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

/// ASCII lower case of one byte; every other byte (including UTF-8 continuation bytes) unchanged.
constexpr char lower_ascii(char character) noexcept {
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
}

}  // namespace

std::string primary_location_id_sql(std::string_view sample_id_expression) {
    return std::format(
        "(SELECT l.id FROM locations l JOIN roots r ON r.id=l.root_id WHERE l.sample_id={} AND l.state<>'retired' "
        "ORDER BY {} LIMIT 1)",
        sample_id_expression, kPrimaryLocationOrder);
}

bool is_predicate_row_evaluable(const FieldPredicate& predicate) {
    switch (predicate.field) {
        case Field::Collection:
            return false;
        case Field::Tag:
            return predicate.text.find('/') == std::string::npos;
        case Field::Category:
            return predicate.text.find('/') == std::string::npos ||
                   folded_taxonomy_segments(predicate.text).has_value();
        case Field::Name:  // column-restricted text terms are Text nodes, never predicates
        case Field::Path:
        case Field::Notes:
            return false;
        case Field::Folder:
        case Field::Bpm:
        case Field::Key:
        case Field::Duration:
        case Field::Rating:
        case Field::Favorite:
        case Field::Format:
        case Field::Codec:
        case Field::SampleRate:
        case Field::Channels:
        case Field::BitDepth:
        case Field::Size:
        case Field::Available:
        case Field::Analyzed:
            return true;
    }
    return false;
}

RootFolderMatch match_folder_in_root(std::string_view value, std::string_view root_name, bool root_case_sensitive) {
    RootFolderMatch match;
    const std::string insensitive_value = without_trailing_slashes(fold_path_key(value));
    if (insensitive_value.empty()) return match;
    const std::string exact_value = without_trailing_slashes(path_key(value, true));

    // A folder anywhere below the root, compared with the root's own case sensitivity.
    match.prefixes.push_back((root_case_sensitive ? exact_value : insensitive_value) + '/');

    // The root itself, or a folder addressed as "<root name>/<folder>".
    const std::string insensitive_root = without_trailing_slashes(fold_path_key(root_name));
    if (insensitive_root.empty()) return match;
    if (insensitive_value == insensitive_root) {
        match.whole_root = true;
    } else if (insensitive_value.starts_with(insensitive_root + '/')) {
        // The case-insensitive key maps code points one to one and keeps '/', so the exact value
        // has the same segments; drop the ones that spelled the root name.
        const std::size_t root_segments = split_on_slash(insensitive_root).size();
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

bool starts_with_ascii_case_insensitive(std::string_view text, std::string_view lowered_prefix) noexcept {
    if (text.size() < lowered_prefix.size()) return false;
    for (std::size_t index = 0; index < lowered_prefix.size(); ++index)
        if (lower_ascii(text[index]) != lowered_prefix[index]) return false;
    return true;
}

std::optional<std::string> smart_collection_text(std::string_view query_json) {
    return json_fields::string_field(json_fields::parse_json(query_json), "text");
}

std::string id_array_json(std::span<const std::int64_t> ids) {
    return json_fields::dump_json(nlohmann::json(std::vector<std::int64_t>(ids.begin(), ids.end())));
}

}  // namespace mso::search::detail
