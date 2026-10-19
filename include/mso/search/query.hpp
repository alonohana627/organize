#pragma once
/// @file
/// The typed search query language (docs/03_DATA_AND_SEARCH.md §6):
///
///     kick
///     "metallic impact" tag:Drums/Kick
///     bpm:140..155 key:F#minor duration:<2s available:true
///     format:flac rating:>=4 favorite:true
///     (tag:Drums OR tag:Percussion) AND NOT tag:Acoustic
///     folder:"Packs/Goa" category:Bass analyzed:tempo
///
/// Whitespace means AND; OR binds weaker than AND; NOT is unary; parentheses group. Quoted strings
/// accept \" and \\ escapes. A trailing '*' on a word of at least two characters is a prefix search;
/// inside a quoted phrase '*' is literal. Limits: 4,096 code points, depth 16 (parentheses and NOT
/// chains), 256 nodes. Errors carry the code-point position and nothing is executed for an invalid
/// query. User text is never forwarded as raw FTS or SQL syntax.
///
/// Numbers: `field:x` means "equal at the precision typed": `bpm:120` matches 119.5 ≤ BPM < 120.5,
/// `bpm:120.0` matches 119.95 ≤ BPM < 120.05, `duration:2s` matches 1.5 s ≤ d < 2.5 s; a range
/// `a..b` widens both ends the same way. `<`, `<=`, `>`, `>=` compare exactly. Count fields
/// (rating, channels, bits) accept whole numbers only.
///
/// Unknown values: a predicate on a value the catalog does not know (no BPM, no duration, no
/// category...) is false, and `NOT` of it is true, so `X` and `NOT X` always partition the library.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"

namespace mso::search {

/// Fields usable as `field:value`.
enum class Field {
    // Taxonomy and navigation.
    Tag,
    Category,
    Folder,
    Collection,
    // Musical and organizing facts.
    Bpm,
    Key,
    Duration,
    Rating,
    Favorite,
    // Technical facts.
    Format,
    Codec,
    SampleRate,
    Channels,
    BitDepth,
    Size,
    Available,
    Analyzed,
    // Lexical, restricted to one indexed full-text column.
    Name,
    Path,
    Notes,
};

/// Canonical name of a field as written in queries ("tag", "bpm", "rate", "bits"...).
std::string_view to_string(Field field) noexcept;

/// Comparison of a numeric field, as typed by the user.
enum class Comparison { Equal, Less, LessOrEqual, Greater, GreaterOrEqual, Between };

/// Free text: one word, a quoted phrase, or a prefix (`kic*`), optionally limited to one column.
struct TextTerm {
    std::string text;             ///< the word or phrase, unescaped
    bool phrase = false;          ///< typed in quotes
    bool prefix = false;          ///< typed with a trailing '*' (removed from `text`)
    std::optional<Field> column;  ///< Name, Path or Notes; nullopt = all indexed text
};

/// `field:value` with a parsed, typed value.
///
/// Numeric bounds are inclusive integers in the field's storage unit (milli-BPM, microseconds,
/// bytes, Hz, plain counts), already adjusted for the comparison and the typed precision:
/// Equal and Between match `low <= value <= high`; Less and LessOrEqual match `value <= high`;
/// Greater and GreaterOrEqual match `value >= low`.
struct FieldPredicate {
    Field field = Field::Tag;                   ///< which field
    Comparison comparison = Comparison::Equal;  ///< numeric comparison as typed
    std::string text;                           ///< tag/category/folder/collection/format/codec value
    std::int64_t low = 0;                       ///< inclusive lower bound (see above); Analyzed: flag mask
    std::int64_t high = 0;                      ///< inclusive upper bound (see above)
    bool flag = false;                          ///< favorite/available value; Analyzed: true = any bit set
    std::optional<MusicalKey> key;              ///< key:F#m; nullopt (text "unknown") = key unknown
};

struct QueryNode;
/// Owning pointer to a query node.
using QueryNodePointer = std::unique_ptr<QueryNode>;

/// One node of the query tree.
struct QueryNode {
    /// Node type: boolean operators, a full-text term or a field predicate.
    enum class Kind { And, Or, Not, Text, Predicate };
    Kind kind = Kind::And;                   ///< node type
    std::vector<QueryNodePointer> children;  ///< And/Or: ≥1, Not: exactly 1
    TextTerm text;                           ///< Text nodes only
    FieldPredicate predicate;                ///< Predicate nodes only
};

/// A parsed query. An empty query (no nodes) matches every live sample.
struct Query {
    QueryNodePointer root;  ///< null for the empty query
    /// True for the empty query.
    bool empty() const { return root == nullptr; }
};

/// Longest accepted query, in Unicode code points.
inline constexpr std::size_t kMaxQueryCodePoints = 4096;
/// Deepest accepted nesting of parentheses and NOT operators.
inline constexpr std::size_t kMaxQueryDepth = 16;
/// Most accepted nodes (terms and operators) in one query.
inline constexpr std::size_t kMaxQueryNodes = 256;

/// Parses query text. PARSE_ERROR carries Error::position (code-point offset) on failure.
Result<Query> parse_query(std::string_view text);

/// Collects the positive (non-negated) text terms, used for relevance ranking.
std::vector<TextTerm> positive_text_terms(const Query& query);

/// True when every sample the query matches necessarily matches one of its positive text terms
/// (so ranking only full-text hits loses nothing). False for `kick OR tag:Drums`, `NOT kick`, or a
/// query without text.
bool requires_text_match(const Query& query);

}  // namespace mso::search
