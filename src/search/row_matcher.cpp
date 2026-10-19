#include "mso/search/row_matcher.hpp"

#include <algorithm>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include "mso/search/detail/predicate_support.hpp"

namespace mso::search {

namespace {

bool node_is_row_evaluable(const QueryNode& node) {
    switch (node.kind) {
        case QueryNode::Kind::And:
        case QueryNode::Kind::Or:
        case QueryNode::Kind::Not:
            return std::ranges::all_of(node.children, [](const auto& child) { return node_is_row_evaluable(*child); });
        case QueryNode::Kind::Text:
            return false;
        case QueryNode::Kind::Predicate:
            switch (node.predicate.field) {
                case Field::Collection:
                    return false;
                case Field::Tag:
                    return node.predicate.text.find('/') == std::string::npos;
                case Field::Category:
                    return node.predicate.text.find('/') == std::string::npos ||
                           detail::folded_taxonomy_segments(node.predicate.text).has_value();
                default:
                    return true;
            }
    }
    return false;
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

}  // namespace

/// One prepared node; mirrors QueryNode with its user values normalized once.
struct RowMatcher::Node {
    QueryNode::Kind kind = QueryNode::Kind::And;
    std::vector<std::unique_ptr<Node>> children;
    FieldPredicate predicate;
    bool evaluable = true;                     ///< false for parts only SQL can evaluate
    std::string folded_name;                   ///< tag/category single segment: fold_key(value)
    std::vector<std::string> folded_segments;  ///< category path: folded segments from the top
};

/// Normalizations that repeat across rows, computed on first use.
struct RowMatcher::Caches {
    /// Folded segments by display path (keyed by path, not id: one matcher may see rows of several
    /// libraries, whose ids overlap).
    std::unordered_map<std::string, std::vector<std::string>> path_segments;
    std::unordered_map<std::string, detail::RootFolderMatch> folder_rules;  ///< by root name + case flag + value

    const std::vector<std::string>& segments_of_path(const std::string& path) {
        auto found = path_segments.find(path);
        if (found == path_segments.end())
            found = path_segments.emplace(path, detail::folded_display_segments(path)).first;
        return found->second;
    }

    const detail::RootFolderMatch& folder_rule(const std::string& value, const SampleRow& row) {
        std::string key = row.root_name;
        key += '\0';
        key += row.root_case_sensitive ? '1' : '0';
        key += value;
        auto found = folder_rules.find(key);
        if (found == folder_rules.end())
            found = folder_rules
                        .emplace(std::move(key),
                                 detail::match_folder_in_root(value, row.root_name, row.root_case_sensitive))
                        .first;
        return found->second;
    }
};

std::unique_ptr<RowMatcher::Node> RowMatcher::prepare(const QueryNode& source) {
    // Recursion depth is bounded by the parser (depth 16, 256 nodes).
    auto node = std::make_unique<Node>();
    node->kind = source.kind;
    node->predicate = source.predicate;
    for (const auto& child : source.children)
        node->children.push_back(prepare(*child));
    if (source.kind == QueryNode::Kind::Text) node->evaluable = false;
    if (source.kind != QueryNode::Kind::Predicate) return node;
    const std::string& value = source.predicate.text;
    switch (source.predicate.field) {
        case Field::Collection:
            node->evaluable = false;
            break;
        case Field::Tag:
            node->evaluable = value.find('/') == std::string::npos;
            node->folded_name = fold_key(value);
            break;
        case Field::Category:
            if (value.find('/') == std::string::npos) {
                node->folded_name = fold_key(value);
            } else if (auto segments = detail::folded_taxonomy_segments(value)) {
                node->folded_segments = std::move(*segments);
            } else {
                node->evaluable = false;
            }
            break;
        default:
            break;
    }
    return node;
}

RowMatcher::RowMatcher(const Query& query) : caches_(std::make_unique<Caches>()) {
    if (query.root) root_ = prepare(*query.root);
}

RowMatcher::~RowMatcher() = default;
RowMatcher::RowMatcher(RowMatcher&&) noexcept = default;
RowMatcher& RowMatcher::operator=(RowMatcher&&) noexcept = default;

bool RowMatcher::evaluate(const Node& node, const SampleRow& row) const {
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

bool RowMatcher::evaluate_predicate(const Node& node, const SampleRow& row) const {
    Caches& caches = *caches_;
    const FieldPredicate& predicate = node.predicate;
    switch (predicate.field) {
        case Field::Tag:
            return std::ranges::any_of(row.tag_paths, [&](const std::string& path) {
                return std::ranges::contains(caches.segments_of_path(path), node.folded_name);
            });
        case Field::Category: {
            if (row.category.empty()) return false;
            const auto& segments = caches.segments_of_path(row.category);
            if (node.folded_segments.empty()) return std::ranges::contains(segments, node.folded_name);
            return segments.size() >= node.folded_segments.size() &&
                   std::ranges::equal(node.folded_segments, segments | std::views::take(node.folded_segments.size()));
        }
        case Field::Folder: {
            if (row.root_id == 0 || row.relative_path.empty()) return false;
            const auto& rule = caches.folder_rule(predicate.text, row);
            if (rule.whole_root) return true;
            if (rule.prefixes.empty()) return false;
            const std::string key = path_key(row.relative_path, row.root_case_sensitive);
            return std::ranges::any_of(rule.prefixes,
                                       [&](const std::string& prefix) { return key.starts_with(prefix); });
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
            return !row.codec.empty() && to_lower_ascii(row.codec).starts_with(predicate.text);
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

bool RowMatcher::operator()(const SampleRow& row) const {
    return !root_ || evaluate(*root_, row);
}

bool is_row_evaluable(const Query& query) {
    return !query.root || node_is_row_evaluable(*query.root);
}

bool matches(const Query& query, const SampleRow& row) {
    return RowMatcher(query)(row);
}

}  // namespace mso::search
