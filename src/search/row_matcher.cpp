#include "mso/search/row_matcher.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include "mso/search/detail/predicate_support.hpp"

namespace mso::search {

namespace {

/// fold_key of each segment of one taxonomy path, from the top ("Drums/Kick" → two keys).
using FoldedPath = std::vector<std::string>;

/// The merged-tag redirects of one library, with the canonical paths folded once.
struct FoldedAliases {
    uuids::uuid library;                                       ///< whose redirects
    std::map<std::string, std::vector<FoldedPath>> canonical;  ///< fold_key(old name) → canonical tag paths
};

/// True when `path` lies in the subtree of `ancestor` (both folded, compared segment by segment).
bool is_in_subtree(const FoldedPath& path, const FoldedPath& ancestor) {
    return path.size() >= ancestor.size() && std::ranges::equal(ancestor, path | std::views::take(ancestor.size()));
}

/// True when `value` satisfies the inclusive bounds of a numeric predicate (see FieldPredicate).
bool satisfies(const FieldPredicate& predicate, std::int64_t value) {
    switch (predicate.comparison) {
        case Comparison::Equal:
        case Comparison::Between:
            return value >= predicate.low && value <= predicate.high;
        case Comparison::Less:
        case Comparison::LessOrEqual:
            return value <= predicate.high;
        case Comparison::Greater:
        case Comparison::GreaterOrEqual:
            return value >= predicate.low;
    }
    return false;
}

/// An optional value: unknown never satisfies a predicate (SQL NULL, made two-valued).
bool satisfies(const FieldPredicate& predicate, const std::optional<std::int64_t>& value) {
    return value && satisfies(predicate, *value);
}

/// Whether every node of a parsed query is row-evaluable (detail::is_predicate_row_evaluable).
bool node_is_row_evaluable(const QueryNode& node) {
    switch (node.kind) {
        case QueryNode::Kind::And:
        case QueryNode::Kind::Or:
        case QueryNode::Kind::Not:
            return std::ranges::all_of(node.children, [](const auto& child) { return node_is_row_evaluable(*child); });
        case QueryNode::Kind::Text:
            return false;
        case QueryNode::Kind::Predicate:
            return detail::is_predicate_row_evaluable(node.predicate);
    }
    return false;
}

/// Transparent hash so text-keyed memos are probed with a string_view without allocating.
struct TextHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>{}(text); }
};

/// What one predicate node remembers about one library's rows: decisions that depend only on a
/// taxonomy id or a root, never on the row itself.
struct LibraryMemo {
    uuids::uuid library;  ///< the library these decisions belong to
    /// tag:/category: nodes: tag or category id → whether that node's path matches.
    std::unordered_map<std::int64_t, bool> taxonomy_decisions;
    /// tag:/category: nodes: the same by display path, for tags and categories without an id yet
    /// (added optimistically, not stored), so their paths are folded once rather than per row.
    std::unordered_map<std::string, bool, TextHash, std::equal_to<>> path_decisions;
    /// folder: nodes: root id → the value resolved against that root.
    std::unordered_map<std::int64_t, detail::RootFolderMatch> folder_rules;
    /// tag: nodes: canonical paths the tag name redirects to in this library (merged tags), or null.
    const std::vector<FoldedPath>* alias_targets = nullptr;
};

}  // namespace

/// One prepared node; mirrors QueryNode with its user values normalized once and its per-library
/// decisions memoized.
struct RowMatcher::Node {
    QueryNode::Kind kind = QueryNode::Kind::And;  ///< node type
    std::vector<std::unique_ptr<Node>> children;  ///< And/Or: ≥1, Not: exactly 1
    FieldPredicate predicate;                     ///< Predicate nodes only
    bool evaluable = true;                        ///< false for parts only SQL can evaluate
    std::string folded_name;                      ///< tag/category single segment: fold_key(value)
    FoldedPath folded_segments;                   ///< category path: folded segments from the top
    mutable std::vector<LibraryMemo> memos;       ///< one per library seen (few)
    mutable std::size_t last_memo = 0;            ///< index of the most recently used memo
};

/// The prepared query with the folded tag aliases; evaluates rows.
struct RowMatcher::Prepared {
    std::vector<FoldedAliases> aliases;  ///< per library
    std::unique_ptr<Node> root;          ///< null for the empty query (matches everything)

    /// Copies a parsed node and normalizes its user values. Recursion depth is bounded by the
    /// parser (depth 16, 256 nodes).
    static std::unique_ptr<Node> prepare(const QueryNode& source) {
        auto node = std::make_unique<Node>();
        node->kind = source.kind;
        node->predicate = source.predicate;
        for (const auto& child : source.children)
            node->children.push_back(prepare(*child));
        if (source.kind == QueryNode::Kind::Text) node->evaluable = false;
        if (source.kind != QueryNode::Kind::Predicate) return node;
        node->evaluable = detail::is_predicate_row_evaluable(source.predicate);
        const std::string& value = source.predicate.text;
        if (source.predicate.field == Field::Tag || source.predicate.field == Field::Category) {
            if (value.find('/') == std::string::npos)
                node->folded_name = fold_key(value);
            else if (auto segments = detail::folded_taxonomy_segments(value))
                node->folded_segments = std::move(*segments);
        }
        return node;
    }

    /// The memo of `node` for `library`, created on first use.
    LibraryMemo& memo_for(const Node& node, const uuids::uuid& library) const {
        if (node.last_memo < node.memos.size() && node.memos[node.last_memo].library == library)
            return node.memos[node.last_memo];
        for (std::size_t index = 0; index < node.memos.size(); ++index) {
            if (node.memos[index].library != library) continue;
            node.last_memo = index;
            return node.memos[index];
        }
        LibraryMemo memo;
        memo.library = library;
        if (node.predicate.field == Field::Tag) {
            const auto library_aliases =
                std::ranges::find(aliases, library, [](const FoldedAliases& entry) { return entry.library; });
            if (library_aliases != aliases.end()) {
                const auto targets = library_aliases->canonical.find(node.folded_name);
                if (targets != library_aliases->canonical.end()) memo.alias_targets = &targets->second;
            }
        }
        node.memos.push_back(std::move(memo));
        node.last_memo = node.memos.size() - 1;
        return node.memos.back();
    }

    /// tag:<name> (SQL: the subtrees of every live tag with that name and of every tag a merged tag
    /// of that name redirects to): a tag path matches when one of its segments is the name, or when
    /// it lies under one of the redirect targets.
    static bool tag_path_matches(const Node& node, const LibraryMemo& memo, std::string_view path) {
        const FoldedPath segments = detail::folded_display_segments(path);
        if (std::ranges::contains(segments, node.folded_name)) return true;
        return memo.alias_targets != nullptr && std::ranges::any_of(*memo.alias_targets, [&](const FoldedPath& target) {
                   return is_in_subtree(segments, target);
               });
    }

    /// category:<name> matches a category path with that segment anywhere; category:<path> matches
    /// that path's subtree, resolved from the top.
    static bool category_path_matches(const Node& node, std::string_view path) {
        const FoldedPath segments = detail::folded_display_segments(path);
        if (node.folded_segments.empty()) return std::ranges::contains(segments, node.folded_name);
        return is_in_subtree(segments, node.folded_segments);
    }

    /// The decision of a tag/category node for one taxonomy node, computed once per library and id;
    /// a node without an id (0: added optimistically, not yet stored) is decided once per path.
    template <class Decide>
    static bool decide_once(LibraryMemo& memo, std::int64_t id, std::string_view path, Decide&& decide) {
        if (id <= 0) {
            if (const auto known = memo.path_decisions.find(path); known != memo.path_decisions.end())
                return known->second;
            const bool decision = decide();
            memo.path_decisions.emplace(std::string(path), decision);
            return decision;
        }
        const auto known = memo.taxonomy_decisions.find(id);
        if (known != memo.taxonomy_decisions.end()) return known->second;
        const bool decision = decide();
        memo.taxonomy_decisions.emplace(id, decision);
        return decision;
    }

    /// Evaluates a prepared node (and its children) against a row.
    bool evaluate(const Node& node, const SampleRow& row) const {
        if (!node.evaluable) return false;
        switch (node.kind) {
            case QueryNode::Kind::And:
                return std::ranges::all_of(node.children, [&](const auto& child) { return evaluate(*child, row); });
            case QueryNode::Kind::Or:
                return std::ranges::any_of(node.children, [&](const auto& child) { return evaluate(*child, row); });
            case QueryNode::Kind::Not:
                return !evaluate(*node.children.front(), row);
            case QueryNode::Kind::Text:
                return false;
            case QueryNode::Kind::Predicate:
                return evaluate_predicate(node, row);
        }
        return false;
    }

    /// Evaluates one field predicate against a row (the comments give the SQL it mirrors).
    bool evaluate_predicate(const Node& node, const SampleRow& row) const {
        const FieldPredicate& predicate = node.predicate;
        switch (predicate.field) {
            case Field::Tag: {
                if (row.tag_paths.empty()) return false;
                LibraryMemo& memo = memo_for(node, row.library);
                for (std::size_t index = 0; index < row.tag_paths.size(); ++index) {
                    const std::int64_t id = index < row.tag_ids.size() ? row.tag_ids[index] : 0;
                    const std::string& path = row.tag_paths[index];
                    if (decide_once(memo, id, path, [&] { return tag_path_matches(node, memo, path); })) return true;
                }
                return false;
            }
            case Field::Category: {
                if (row.category.empty()) return false;
                LibraryMemo& memo = memo_for(node, row.library);
                return decide_once(memo, row.category_id.value_or(0), row.category,
                                   [&] { return category_path_matches(node, row.category); });
            }
            case Field::Folder: {  // primary location: path_key BETWEEN prefix AND upper bound
                if (row.root_id == 0 || row.path_key.empty()) return false;
                LibraryMemo& memo = memo_for(node, row.library);
                auto rule = memo.folder_rules.find(row.root_id);
                if (rule == memo.folder_rules.end())
                    rule = memo.folder_rules
                               .emplace(row.root_id, detail::match_folder_in_root(predicate.text, row.root_name,
                                                                                  row.root_case_sensitive))
                               .first;
                if (rule->second.whole_root) return true;
                return std::ranges::any_of(rule->second.prefixes,
                                           [&](const std::string& prefix) { return row.path_key.starts_with(prefix); });
            }
            case Field::Collection:
                return false;
            case Field::Bpm:
                return satisfies(predicate, row.bpm_milli);
            case Field::Duration:
                return satisfies(predicate, row.duration_us);
            case Field::Rating:
                return satisfies(predicate, std::int64_t{row.rating});
            case Field::SampleRate:
                return satisfies(predicate, row.sample_rate);
            case Field::Channels:
                return satisfies(predicate, row.channels);
            case Field::BitDepth:
                return satisfies(predicate, row.bit_depth);
            case Field::Size:
                return satisfies(predicate, row.byte_size);
            case Field::Favorite:
                return row.favorite == predicate.flag;
            case Field::Available:
                return (row.availability == RowAvailability::Available) == predicate.flag;
            case Field::Analyzed:
                return ((row.analysis_flags & predicate.low) != 0) == predicate.flag;
            case Field::Format:  // SQL `=`: exact; an unknown format is "" and never equals a typed value
                return !row.format.empty() && row.format == predicate.text;
            case Field::Codec:  // SQL LIKE 'text%': prefix, ASCII case-insensitive
                return !row.codec.empty() && detail::starts_with_ascii_case_insensitive(row.codec, predicate.text);
            case Field::Key:
                if (!predicate.key) return !row.key.has_value();
                return row.key && row.key->pitch_class == predicate.key->pitch_class &&
                       row.key->mode == predicate.key->mode;
            case Field::Name:
            case Field::Path:
            case Field::Notes:
                return false;
        }
        return false;
    }
};

RowMatcher::RowMatcher(const Query& query, std::span<const TagAliases> tag_aliases)
    : prepared_(std::make_unique<Prepared>()) {
    for (const auto& library_aliases : tag_aliases) {
        FoldedAliases folded{library_aliases.library, {}};
        for (const auto& [name_key, paths] : library_aliases.canonical_paths)
            for (const auto& path : paths)
                folded.canonical[name_key].push_back(detail::folded_display_segments(path));
        prepared_->aliases.push_back(std::move(folded));
    }
    if (query.root) prepared_->root = Prepared::prepare(*query.root);
}

RowMatcher::~RowMatcher() = default;
RowMatcher::RowMatcher(RowMatcher&&) noexcept = default;
RowMatcher& RowMatcher::operator=(RowMatcher&&) noexcept = default;

bool RowMatcher::operator()(const SampleRow& row) const {
    if (!prepared_) return false;  // moved from
    return !prepared_->root || prepared_->evaluate(*prepared_->root, row);
}

bool is_row_evaluable(const Query& query) {
    return !query.root || node_is_row_evaluable(*query.root);
}

bool matches(const Query& query, const SampleRow& row, std::span<const TagAliases> tag_aliases) {
    return RowMatcher(query, tag_aliases)(row);
}

}  // namespace mso::search
