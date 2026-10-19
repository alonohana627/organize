#pragma once
/// @file
/// In-memory evaluation of search queries against hydrated SampleRows, with exactly the semantics
/// of the SQL compiler (query_compiler.hpp): the application's in-memory sample index filters with
/// it, and search tests check both evaluators against each other on the same fixture.
#include <memory>
#include <span>

#include "mso/search/query.hpp"
#include "mso/search/sample_rows.hpp"

namespace mso::search {

/// Whether a query can be evaluated purely from SampleRow fields (plus the libraries' TagAliases).
/// False when it contains a full-text term (needs the FTS index) or a predicate that
/// `detail::is_predicate_row_evaluable` rejects: `collection:` (membership is not part of a row), a
/// multi-segment `tag:` path (each segment may name a merged tag), or a `category:` path the
/// catalog would reject. The empty query is row-evaluable.
[[nodiscard]] bool is_row_evaluable(const Query& query);

/// Evaluates a row-evaluable query against one row, with the SQL semantics (a predicate on an
/// unknown value is false and its negation true). The result is meaningful only for a row-evaluable
/// query: a non-evaluable leaf evaluates to false, so it turns true under NOT. Builds a RowMatcher
/// per call: prefer RowMatcher for many rows.
[[nodiscard]] bool matches(const Query& query, const SampleRow& row, std::span<const TagAliases> tag_aliases = {});

/// A query prepared for evaluating many rows of one or more libraries. User values are normalized
/// once; per library, `folder:` rules are resolved once per root and `tag:`/`category:` decisions
/// once per taxonomy id (once per path for a tag or category without an id yet), so a row costs a few
/// integer lookups and `starts_with` on its stored `SampleRow::path_key`.
///
/// Thread safety: `operator()` is logically const but fills those caches, so one matcher must not
/// be used by two threads at once; use one matcher per thread (they are cheap to build). A
/// moved-from matcher matches no row.
class RowMatcher {
public:
    /// Prepares `query`; the matcher keeps its own copy of everything it needs. `tag_aliases` holds
    /// the merged-tag redirects of the libraries whose rows will be evaluated (load_tag_aliases);
    /// without them `tag:<old name of a merged tag>` matches no row, unlike SQL.
    explicit RowMatcher(const Query& query, std::span<const TagAliases> tag_aliases = {});
    ~RowMatcher();
    RowMatcher(RowMatcher&&) noexcept;
    RowMatcher& operator=(RowMatcher&&) noexcept;
    RowMatcher(const RowMatcher&) = delete;
    RowMatcher& operator=(const RowMatcher&) = delete;

    /// True when the row matches (see `matches`).
    [[nodiscard]] bool operator()(const SampleRow& row) const;

private:
    struct Node;
    struct Prepared;
    std::unique_ptr<Prepared> prepared_;  ///< the prepared tree, aliases and caches; null only when moved from
};

}  // namespace mso::search
