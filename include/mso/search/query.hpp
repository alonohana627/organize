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
/// accept \" and \\ escapes. A trailing '*' on a word of at least two characters is a prefix search.
/// Limits: 4,096 code points, depth 16, 256 nodes. Errors carry the code-point position and nothing
/// is executed for an invalid query. User text is never forwarded as raw FTS or SQL syntax.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "mso/domain/core.hpp"
#include "mso/domain/error.hpp"

namespace mso::search {

/// Fields usable as `field:value`.
enum class Field {
    Tag,
    Category,
    Folder,
    Collection,  // taxonomy and navigation
    Bpm,
    Key,
    Duration,
    Rating,
    Favorite,  // musical and organizing facts
    Format,
    Codec,
    SampleRate,
    Channels,
    BitDepth,
    Size,
    Available,
    Analyzed,
    Name,
    Path,
    Notes,  // lexical, restricted to one indexed column
};

std::string_view to_string(Field field) noexcept;

/// Comparison of a numeric field.
enum class Comparison { Equal, Less, LessOrEqual, Greater, GreaterOrEqual, Between };

/// Free text: one word, a quoted phrase, or a prefix (`kic*`), optionally limited to one column.
struct TextTerm {
    std::string text;
    bool phrase = false;
    bool prefix = false;
    std::optional<Field> column;  ///< Name, Path or Notes; nullopt = all indexed text
};

/// `field:value` with a parsed, typed value.
struct FieldPredicate {
    Field field = Field::Tag;
    Comparison comparison = Comparison::Equal;
    std::string text;               ///< tag/category/folder/collection/format/codec value
    std::int64_t low = 0;           ///< numeric value (milli-BPM, microseconds, bytes, Hz...)
    std::int64_t high = 0;          ///< upper bound for Between (inclusive)
    bool flag = false;              ///< favorite/available
    std::optional<MusicalKey> key;  ///< key:F#m; nullopt with text "unknown" = key unknown
};

struct QueryNode;
using QueryNodePointer = std::unique_ptr<QueryNode>;

/// One node of the query tree.
struct QueryNode {
    enum class Kind { And, Or, Not, Text, Predicate };
    Kind kind = Kind::And;
    std::vector<QueryNodePointer> children;  ///< And/Or: ≥1, Not: exactly 1
    TextTerm text;
    FieldPredicate predicate;
};

/// A parsed query. An empty query (no nodes) matches every live sample.
struct Query {
    QueryNodePointer root;  ///< null for the empty query
    bool empty() const { return root == nullptr; }
};

inline constexpr std::size_t kMaxQueryCodePoints = 4096;
inline constexpr std::size_t kMaxQueryDepth = 16;
inline constexpr std::size_t kMaxQueryNodes = 256;

/// Parses query text. PARSE_ERROR carries Error::position (code-point offset) on failure.
Result<Query> parse_query(std::string_view text);

/// Collects the positive (non-negated) text terms, used for relevance ranking.
std::vector<TextTerm> positive_text_terms(const Query& query);

}  // namespace mso::search
