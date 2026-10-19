#pragma once
/// @file
/// Paged, sorted, federated search across open libraries (docs/03_DATA_AND_SEARCH.md §7–8).
/// Metadata sorts use keyset pagination with a two-part cursor (non-null values, then nulls);
/// relevance uses a bounded top-K window per library merged with reciprocal rank fusion.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mso/catalog/library.hpp"
#include "mso/search/query.hpp"

namespace mso::search {

/// Sortable columns (the whitelist from the spec).
enum class SortField { Name, Added, Duration, Size, SampleRate, Bpm, Key, Rating, Format, Relevance };

std::string_view to_string(SortField field) noexcept;
std::optional<SortField> parse_sort_field(std::string_view text) noexcept;

/// Playability of a sample as displayed in the table.
enum class RowAvailability { Available, SourceOffline, Missing, Unknown };
std::string_view to_string(RowAvailability availability) noexcept;

/// One hydrated result row (bounded: at most page_size per request).
struct SampleRow {
    uuids::uuid library;
    std::int64_t sample_id = 0;
    uuids::uuid sample_uuid;
    std::string display_name;
    std::string file_name;
    std::string root_name;
    std::string relative_path;  ///< first live location, root-relative
    uuids::uuid root_uuid;
    catalog::RootMode root_mode = catalog::RootMode::Referenced;
    std::string format, codec;
    std::optional<std::int64_t> duration_us;
    bool duration_exact = false;
    std::optional<std::int64_t> sample_rate, channels, bit_depth, byte_size;
    std::optional<std::int64_t> bpm_milli;
    std::string bpm_source;
    std::optional<double> bpm_confidence;
    std::optional<MusicalKey> key;
    std::string key_source;
    std::optional<double> key_confidence;
    std::string category;
    std::string tags;                    ///< display paths joined with ", "
    std::vector<std::string> tag_paths;  ///< the same paths as a list (export, inspector)
    int rating = 0;
    bool favorite = false;
    RowAvailability availability = RowAvailability::Unknown;
    std::int64_t analysis_flags = 0;
    std::int64_t created_at = 0;
    double relevance = 0;  ///< relevance sort only
};

/// Opaque continuation token (JSON); invalid for a different query or sort (STALE_CURSOR).
struct Cursor {
    std::string token;
};

/// Navigation scope applied in addition to the query text.
struct Scope {
    std::optional<std::int64_t> root_id;  ///< single-library scopes only
    std::optional<std::string> folder;    ///< root-relative folder path
    std::optional<std::int64_t> collection_id;
    std::optional<std::int64_t> tag_id;       ///< subtree
    std::optional<std::int64_t> category_id;  ///< subtree
    bool uncategorized = false;
};

struct SearchRequest {
    std::string text;
    Scope scope;
    SortField sort = SortField::Name;
    bool descending = false;
    std::size_t page_size = 200;  ///< clamped to 1..500
    std::optional<Cursor> cursor;
};

/// Per-library outcome so partial results are never silently reported as complete.
struct LibraryCoverage {
    uuids::uuid library;
    std::string name;
    bool complete = true;
    std::optional<Error> error;
};

struct SearchPage {
    std::vector<SampleRow> rows;
    std::optional<Cursor> next;
    std::vector<LibraryCoverage> coverage;
    bool relevance_window_limited = false;  ///< relevance ranks only the top 1,000 per library
};

/// Exact count with per-library coverage (run asynchronously; never on every keystroke).
struct CountResult {
    std::int64_t total = 0;
    std::vector<LibraryCoverage> coverage;
};

/// Stateless search over the given libraries. Thread-safe: each call leases its own readers.
class SearchService {
public:
    static constexpr std::size_t kMaxPageSize = 500;
    static constexpr std::size_t kRelevanceDepth = 1000;
    static constexpr double kReciprocalRankConstant = 60.0;

    Result<SearchPage> search(std::span<catalog::Library* const> libraries, const SearchRequest& request) const;
    Result<CountResult> count(std::span<catalog::Library* const> libraries, const SearchRequest& request) const;
    /// Validates query text without running it (for smart collections and the search box).
    Result<void> validate(std::string_view text) const;
};

}  // namespace mso::search
