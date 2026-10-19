#pragma once
/// @file
/// Internal pieces shared by the SQL compiler (query_compiler.cpp), the scope compiler and row
/// loader (search_service.cpp, sample_rows.cpp) and the in-memory matcher (row_matcher.cpp), so
/// that SQL and memory evaluate every predicate identically. Not part of the public search API.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mso/search/query.hpp"

namespace mso::search::detail {

/// Ranks a sample's live locations (`l` joined with its root `r`): present on a reachable,
/// non-deleted root first, then present, then oldest. The first is the sample's *primary*
/// location: the one shown in the table (SampleRow::relative_path) and the one `folder:` and the
/// root/folder scopes test.
inline constexpr std::string_view kPrimaryLocationOrder =
    "(l.state='present' AND r.availability<>'offline' AND r.deleted=0) DESC, l.state='present' DESC, l.id";

/// Scalar subquery: the id of the primary location (kPrimaryLocationOrder) of the sample whose id
/// is the SQL expression `sample_id_expression` (for example "s.id" or "fl.sample_id"), NULL when
/// it has none. Uses the table aliases `l` and `r` internally.
[[nodiscard]] std::string primary_location_id_sql(std::string_view sample_id_expression);

/// Whether one field predicate can be evaluated from SampleRow fields alone: the single rule that
/// both `is_row_evaluable` and RowMatcher follow. False for `collection:` (membership is not part
/// of a row), a multi-segment `tag:` path (each segment may name a merged tag that the catalog
/// resolves through its alias) and a `category:` path the catalog would reject.
[[nodiscard]] bool is_predicate_row_evaluable(const FieldPredicate& predicate);

/// How a `folder:` value (or a folder scope) applies to the locations of one root.
struct RootFolderMatch {
    bool whole_root = false;            ///< the value names the root itself: every location matches
    std::vector<std::string> prefixes;  ///< location path keys starting with one of these match (each ends in '/')
};

/// Resolves `folder:<value>` against one root. A location matches when its path key starts with
/// the value's path key plus '/' (a folder anywhere below the root, compared with the root's case
/// sensitivity), or when the value is the root's name (whole root) or the root's name followed by
/// a folder below it ("Samples/Drums"). Root names always compare case-insensitively.
[[nodiscard]] RootFolderMatch match_folder_in_root(std::string_view value, std::string_view root_name,
                                                   bool root_case_sensitive);

/// The exclusive upper bound of the path keys that start with `prefix` (which ends in '/'):
/// the prefix with its final '/' replaced by '0', the next byte. `key >= prefix AND key < upper`
/// is an index-friendly, byte-exact prefix test.
[[nodiscard]] std::string prefix_upper_bound(std::string_view prefix);

/// fold_key of each segment of a multi-segment tag/category path ("Drums/Kick" → two keys),
/// validated like the catalog's taxonomy paths (no empty segment, at most 16 segments of at most
/// 128 code points). nullopt for a path the catalog would reject.
[[nodiscard]] std::optional<std::vector<std::string>> folded_taxonomy_segments(std::string_view path);

/// fold_key of each segment of a stored display path (tag or category path of a row); never fails.
[[nodiscard]] std::vector<std::string> folded_display_segments(std::string_view path);

/// True when `text` starts with `lowered_prefix`, comparing ASCII letters case-insensitively and
/// every other byte exactly: SQLite's default `LIKE 'prefix%'` on UTF-8 text. `lowered_prefix` must
/// already be ASCII-lowered (the parser lowers `codec:` values). Allocation-free.
[[nodiscard]] bool starts_with_ascii_case_insensitive(std::string_view text, std::string_view lowered_prefix) noexcept;

/// The `text` of a smart collection's stored `query_json` ({"text": "..."}), or nullopt when the
/// document is not valid JSON, not an object, or its text is missing or not a string.
[[nodiscard]] std::optional<std::string> smart_collection_text(std::string_view query_json);

/// A JSON array of ids ("[1,2,3]") for `json_each(?)` set filters, written by nlohmann::json.
[[nodiscard]] std::string id_array_json(std::span<const std::int64_t> ids);

}  // namespace mso::search::detail
