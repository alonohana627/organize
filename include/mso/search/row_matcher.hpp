#pragma once
/// @file
/// In-memory evaluation of search queries against hydrated SampleRows, with exactly the semantics
/// of the SQL compiler (query_compiler.hpp): the application's in-memory sample index filters with
/// it, and search tests check both evaluators against each other on the same fixture.
#include <memory>

#include "mso/search/query.hpp"
#include "mso/search/sample_rows.hpp"

namespace mso::search {

/// Whether a query can be evaluated purely from SampleRow fields. False when it contains a
/// full-text term (needs the FTS index), a `collection:` predicate (membership is not part of a
/// row), a multi-segment `tag:` path (it may name a merged tag that the catalog resolves through
/// its alias), or a `category:` path the catalog would reject. The empty query is row-evaluable.
bool is_row_evaluable(const Query& query);

/// Evaluates a row-evaluable query against one row (same semantics as the SQL compiler: a
/// predicate on an unknown value is false and its negation true). Parts that are not
/// row-evaluable evaluate to false. Builds a RowMatcher per call: prefer RowMatcher for many rows.
bool matches(const Query& query, const SampleRow& row);

/// A query prepared for evaluating many rows: user values are normalized once, and folded tag and
/// category paths and per-root folder rules are cached across rows. Not thread-safe (the caches
/// are mutable): use one matcher per thread.
class RowMatcher {
public:
    /// Prepares `query`; the matcher keeps its own copy of everything it needs.
    explicit RowMatcher(const Query& query);
    ~RowMatcher();
    RowMatcher(RowMatcher&&) noexcept;
    RowMatcher& operator=(RowMatcher&&) noexcept;
    RowMatcher(const RowMatcher&) = delete;
    RowMatcher& operator=(const RowMatcher&) = delete;

    /// True when the row matches (see `matches`).
    bool operator()(const SampleRow& row) const;

private:
    struct Node;
    struct Caches;
    /// Copies a parsed node and normalizes its user values.
    static std::unique_ptr<Node> prepare(const QueryNode& source);
    /// Evaluates a prepared node (and its children) against a row.
    bool evaluate(const Node& node, const SampleRow& row) const;
    /// Evaluates one field predicate against a row.
    bool evaluate_predicate(const Node& node, const SampleRow& row) const;

    std::unique_ptr<Node> root_;      ///< null for the empty query (matches everything)
    std::unique_ptr<Caches> caches_;  ///< per-tag, per-category and per-root normalizations
};

}  // namespace mso::search
