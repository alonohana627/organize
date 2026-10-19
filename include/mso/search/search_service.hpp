#pragma once
/// @file
/// Paged, sorted, federated search across open libraries (docs/03_DATA_AND_SEARCH.md §7–8).
/// Metadata sorts use keyset pagination with a two-part cursor (non-null values, then nulls);
/// relevance uses a bounded top-K window per library merged with reciprocal rank fusion. Only the
/// final page is hydrated (load_rows), with a handful of set-based queries per library.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mso/catalog/library.hpp"
#include "mso/search/query.hpp"
#include "mso/search/row_matcher.hpp"
#include "mso/search/sample_rows.hpp"

namespace mso::search {

/// Sortable columns (the whitelist from the spec).
enum class SortField { Name, Added, Duration, Size, SampleRate, Bpm, Key, Rating, Format, Relevance };

/// Wire name of a sort field ("name", "added", "duration", "size", "rate", "bpm", "key", "rating",
/// "format", "relevance").
std::string_view to_string(SortField field) noexcept;
/// Inverse of `to_string(SortField)`; nullopt for an unknown name.
std::optional<SortField> parse_sort_field(std::string_view text) noexcept;

/// Opaque continuation token (JSON); STALE_CURSOR for a different query, sort, scope or library set.
struct Cursor {
    std::string token;  ///< opaque; pass back unchanged
};

/// Navigation scope applied in addition to the query text. Root and folder test the sample's
/// primary location (see SampleRow).
struct Scope {
    std::optional<std::int64_t> root_id;  ///< single-library scopes only
    std::optional<std::string> folder;    ///< root-relative folder path; requires root_id (INVALID_ARGUMENT otherwise)
    std::optional<std::int64_t> collection_id;  ///< manual or smart collection
    std::optional<std::int64_t> tag_id;         ///< tag subtree
    std::optional<std::int64_t> category_id;    ///< category subtree
    bool uncategorized = false;                 ///< only samples without a category
};

/// One search: query text, scope, order and page.
struct SearchRequest {
    std::string text;                  ///< query language text (query.hpp); "" matches everything
    Scope scope;                       ///< navigation scope
    SortField sort = SortField::Name;  ///< order; Relevance without positive text terms sorts by name
    bool descending = false;           ///< reverse order (metadata sorts)
    std::size_t page_size = 200;       ///< clamped to 1..500
    std::optional<Cursor> cursor;      ///< continuation of a previous page of the same request
};

/// Per-library outcome so partial results are never silently reported as complete.
struct LibraryCoverage {
    uuids::uuid library;         ///< which library
    std::string name;            ///< its display name
    bool complete = true;        ///< false when the library could not be searched (see `error`)
    std::optional<Error> error;  ///< why the library is missing from the results
};

/// One page of results.
struct SearchPage {
    std::vector<SampleRow> rows;            ///< at most page_size rows, in global order
    std::optional<Cursor> next;             ///< continuation, absent on the last page
    std::vector<LibraryCoverage> coverage;  ///< one entry per searched library
    bool relevance_window_limited = false;  ///< relevance ranked only the top 1,000 of some library
};

/// Exact count with per-library coverage (run asynchronously; never on every keystroke).
struct CountResult {
    std::int64_t total = 0;                 ///< matching live samples across complete libraries
    std::vector<LibraryCoverage> coverage;  ///< one entry per counted library
};

/// Ids of the live samples of one library matching `request.text` and `request.scope`, in
/// ascending id order (no sort, no hydration, no paging). The application combines it with its
/// in-memory index for queries that need full-text search.
Result<std::vector<std::int64_t>> matching_ids(catalog::Library& library, const SearchRequest& request);

/// Stateless search over the given libraries. Thread-safe: each call leases its own readers.
///
/// Error policy (search, count): a problem with the request itself (PARSE_ERROR, INVALID_ARGUMENT,
/// STALE_CURSOR) fails the whole call; a failure of one library (busy, offline database, I/O)
/// marks that library's coverage incomplete and the other libraries still answer.
class SearchService {
public:
    /// Largest page.
    static constexpr std::size_t kMaxPageSize = 500;
    /// Relevance ranks at most this many full-text hits per library.
    static constexpr std::size_t kRelevanceDepth = 1000;
    /// The k of reciprocal rank fusion: score = 1 / (k + rank).
    static constexpr double kReciprocalRankConstant = 60.0;

    /// One page of the request across `libraries`, globally ordered (ties: library UUID, then id).
    Result<SearchPage> search(std::span<catalog::Library* const> libraries, const SearchRequest& request) const;
    /// Exact number of matches across `libraries` (cursor, sort and page size are ignored).
    Result<CountResult> count(std::span<catalog::Library* const> libraries, const SearchRequest& request) const;
    /// Validates query text without running it (for smart collections and the search box).
    Result<void> validate(std::string_view text) const;
};

}  // namespace mso::search
