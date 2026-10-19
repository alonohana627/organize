// Recursive-descent parser for the search query language (see query.hpp).
#include <algorithm>
#include <format>
#include <initializer_list>
#include <limits>
#include <utility>

#include "mso/domain/analysis_flags.hpp"
#include "mso/search/query.hpp"

namespace mso::search {

std::string_view to_string(Field field) noexcept {
    switch (field) {
        case Field::Tag:
            return "tag";
        case Field::Category:
            return "category";
        case Field::Folder:
            return "folder";
        case Field::Collection:
            return "collection";
        case Field::Bpm:
            return "bpm";
        case Field::Key:
            return "key";
        case Field::Duration:
            return "duration";
        case Field::Rating:
            return "rating";
        case Field::Favorite:
            return "favorite";
        case Field::Format:
            return "format";
        case Field::Codec:
            return "codec";
        case Field::SampleRate:
            return "rate";
        case Field::Channels:
            return "channels";
        case Field::BitDepth:
            return "bits";
        case Field::Size:
            return "size";
        case Field::Available:
            return "available";
        case Field::Analyzed:
            return "analyzed";
        case Field::Name:
            return "name";
        case Field::Path:
            return "path";
        case Field::Notes:
            return "notes";
    }
    return "?";
}

namespace {

/// Whether '\' separates folders in a typed folder value, as in path_key: Windows only.
#ifdef _WIN32
constexpr bool kBackslashIsSeparator = true;
#else
constexpr bool kBackslashIsSeparator = false;
#endif

/// The field a query names ("tag", "tempo", "favourite"...), ASCII case-insensitive; nullopt for an
/// unknown name.
std::optional<Field> field_from_name(std::string_view name) {
    const std::string lowered = to_lower_ascii(name);
    struct Alias {
        std::string_view name;
        Field field;
    };
    static constexpr Alias kAliases[] = {
        {"tag", Field::Tag},
        {"tags", Field::Tag},
        {"category", Field::Category},
        {"folder", Field::Folder},
        {"collection", Field::Collection},
        {"bpm", Field::Bpm},
        {"tempo", Field::Bpm},
        {"key", Field::Key},
        {"duration", Field::Duration},
        {"length", Field::Duration},
        {"rating", Field::Rating},
        {"favorite", Field::Favorite},
        {"favourite", Field::Favorite},
        {"format", Field::Format},
        {"type", Field::Format},
        {"codec", Field::Codec},
        {"rate", Field::SampleRate},
        {"samplerate", Field::SampleRate},
        {"channels", Field::Channels},
        {"bits", Field::BitDepth},
        {"bitdepth", Field::BitDepth},
        {"size", Field::Size},
        {"available", Field::Available},
        {"analyzed", Field::Analyzed},
        {"analysed", Field::Analyzed},
        {"name", Field::Name},
        {"path", Field::Path},
        {"notes", Field::Notes},
    };
    for (const auto& alias : kAliases)
        if (alias.name == lowered) return alias.field;
    return std::nullopt;
}

/// A lexical token with its byte offset in the query.
struct Token {
    /// Token type.
    enum class Kind { Word, Quoted, FieldValue, OpenParen, CloseParen, End };
    Kind kind = Kind::End;      ///< token type
    std::string text;           ///< word / quoted content / field value
    std::string field_name;     ///< FieldValue only
    bool value_quoted = false;  ///< FieldValue only
    std::size_t offset = 0;     ///< byte offset of the token's first character
};

/// A unit suffix and how many storage units (milli-BPM, microseconds, bytes, Hz) one of it is.
struct Unit {
    std::string_view suffix;
    std::int64_t storage_units;
};

/// A non-negative decimal number as typed: `mantissa` × 10^-`decimals` ("120.25" → 12025, 2).
struct Decimal {
    std::int64_t mantissa = 0;
    int decimals = 0;
};

/// The largest supported number of decimals and significant digits of a typed number.
constexpr int kMaxDecimals = 9;
constexpr int kMaxDigits = 18;
/// Largest accepted value in storage units (well inside SQLite's 64-bit integers).
constexpr std::int64_t kMaxStorageValue = 9'000'000'000'000'000;

/// ⌈numerator / denominator⌉ for a positive denominator, for every int64 numerator (no
/// intermediate sum that could overflow; C++ division truncates toward zero).
constexpr std::int64_t ceiling_divide(std::int64_t numerator, std::int64_t denominator) {
    return numerator / denominator + (numerator % denominator > 0 ? 1 : 0);
}

/// ⌊numerator / denominator⌋ for a positive denominator, for every int64 numerator.
constexpr std::int64_t floor_divide(std::int64_t numerator, std::int64_t denominator) {
    return numerator / denominator - (numerator % denominator < 0 ? 1 : 0);
}
static_assert(ceiling_divide(7, 2) == 4 && ceiling_divide(-7, 2) == -3 && ceiling_divide(6, 2) == 3);
static_assert(floor_divide(7, 2) == 3 && floor_divide(-7, 2) == -4 && floor_divide(-6, 2) == -3);
static_assert(ceiling_divide(std::numeric_limits<std::int64_t>::max(), 2'000'000'000) == 4'611'686'019);

/// Recursive-descent parser over a token list; one instance parses one query.
class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    /// Tokenizes and parses the whole text.
    Result<Query> parse() {
        if (count_code_points(text_) > kMaxQueryCodePoints) return error_at(0, "query is longer than 4096 characters");
        if (!is_valid_utf8(text_)) return error_at(0, "query is not valid UTF-8");
        if (auto tokenized = tokenize(); !tokenized) return std::unexpected(tokenized.error());
        Query query;
        if (peek().kind == Token::Kind::End) return query;
        auto root = parse_or(0);
        if (!root) return std::unexpected(root.error());
        if (peek().kind != Token::Kind::End)
            return error_at(peek().offset,
                            peek().kind == Token::Kind::CloseParen ? "unmatched ')'" : "unexpected input");
        query.root = std::move(*root);
        return query;
    }

private:
    // ------------------------------------------------------------ errors
    /// PARSE_ERROR positioned at the code point that starts at `byte_offset`.
    std::unexpected<Error> error_at(std::size_t byte_offset, std::string message) const {
        Error error{ErrorCode::ParseError, false, std::move(message), std::nullopt,
                    static_cast<std::uint32_t>(count_code_points(text_.substr(0, byte_offset)))};
        return std::unexpected(std::move(error));
    }

    // ------------------------------------------------------------ tokenizer
    static bool is_space(char character) {
        return character == ' ' || character == '\t' || character == '\n' || character == '\r';
    }

    /// True when a token may end before `position`: at the end, at whitespace or a parenthesis.
    bool at_token_end(std::size_t position) const {
        return position >= text_.size() || is_space(text_[position]) || text_[position] == '(' ||
               text_[position] == ')';
    }

    /// Reads a quoted string starting at the opening quote; returns the unescaped content. The
    /// closing quote must end the token: `"kick"*` (a quoted prefix) and `tag:"x"y` are errors
    /// rather than silently becoming two terms.
    Result<std::string> read_quoted(std::size_t& position) {
        const std::size_t start = position++;
        std::string content;
        while (position < text_.size()) {
            const char character = text_[position++];
            if (character == '"') {
                if (at_token_end(position)) return content;
                if (text_[position] == '*')
                    return error_at(position,
                                    "a quoted phrase cannot be a prefix search; remove the quotes or the '*'");
                return error_at(position, "put a space after the closing quote");
            }
            if (character == '\\') {
                if (position >= text_.size()) break;
                const char escaped = text_[position++];
                if (escaped != '"' && escaped != '\\')
                    return error_at(position - 2, "only \\\" and \\\\ escapes are allowed");
                content.push_back(escaped);
                continue;
            }
            content.push_back(character);
        }
        return error_at(start, "unterminated quote");
    }

    /// Splits the text into words, quoted phrases, `field:value` pairs and parentheses.
    Result<void> tokenize() {
        std::size_t position = 0;
        while (true) {
            while (position < text_.size() && is_space(text_[position]))
                ++position;
            if (position >= text_.size()) break;
            const char character = text_[position];
            if (character == '(' || character == ')') {
                tokens_.push_back(
                    {character == '(' ? Token::Kind::OpenParen : Token::Kind::CloseParen, {}, {}, false, position});
                ++position;
                continue;
            }
            if (character == '"') {
                const std::size_t start = position;
                auto quoted = read_quoted(position);
                if (!quoted) return std::unexpected(quoted.error());
                tokens_.push_back({Token::Kind::Quoted, std::move(*quoted), {}, false, start});
                continue;
            }
            const std::size_t start = position;
            while (position < text_.size() && !is_space(text_[position]) && text_[position] != '(' &&
                   text_[position] != ')' && text_[position] != '"' && text_[position] != ':')
                ++position;
            std::string word(text_.substr(start, position - start));
            if (position < text_.size() && text_[position] == ':' && !word.empty()) {
                ++position;  // field:value
                Token token{Token::Kind::FieldValue, {}, std::move(word), false, start};
                if (position < text_.size() && text_[position] == '"') {
                    auto quoted = read_quoted(position);
                    if (!quoted) return std::unexpected(quoted.error());
                    token.text = std::move(*quoted);
                    token.value_quoted = true;
                } else {
                    const std::size_t value_start = position;
                    while (position < text_.size() && !is_space(text_[position]) && text_[position] != '(' &&
                           text_[position] != ')')
                        ++position;
                    token.text = std::string(text_.substr(value_start, position - value_start));
                }
                if (token.text.empty()) return error_at(start, std::format("'{}:' needs a value", token.field_name));
                tokens_.push_back(std::move(token));
                continue;
            }
            if (word.empty()) return error_at(position, "unexpected ':'");
            tokens_.push_back({Token::Kind::Word, std::move(word), {}, false, start});
        }
        tokens_.push_back({Token::Kind::End, {}, {}, false, text_.size()});
        return {};
    }

    /// The next token (the End token once everything is consumed).
    const Token& peek() const { return tokens_[index_]; }
    /// Consumes and returns the next token.
    const Token& advance() { return tokens_[index_++]; }
    /// True when the next token is the operator `keyword` (operators are upper-case only).
    bool peek_keyword(std::string_view keyword) const {
        return peek().kind == Token::Kind::Word && peek().text == keyword;
    }
    /// True when the next token cannot start a term.
    bool at_term_boundary() const {
        return peek().kind == Token::Kind::End || peek().kind == Token::Kind::CloseParen || peek_keyword("OR") ||
               peek_keyword("AND");
    }

    // ------------------------------------------------------------ grammar
    /// A new node of `kind`, or a PARSE_ERROR once the query exceeds kMaxQueryNodes.
    Result<QueryNodePointer> make_node(QueryNode::Kind kind, std::size_t offset) {
        if (++node_count_ > kMaxQueryNodes) return error_at(offset, "query has more than 256 terms");
        auto node = std::make_unique<QueryNode>();
        node->kind = kind;
        return node;
    }

    /// or := and ("OR" and)*
    Result<QueryNodePointer> parse_or(std::size_t depth) {
        if (depth > kMaxQueryDepth) return error_at(peek().offset, "query is nested more than 16 levels");
        auto first = parse_and(depth);
        if (!first) return first;
        if (!peek_keyword("OR")) return first;
        auto node = make_node(QueryNode::Kind::Or, peek().offset);
        if (!node) return node;
        (*node)->children.push_back(std::move(*first));
        while (peek_keyword("OR")) {
            const auto offset = advance().offset;
            if (at_term_boundary()) return error_at(offset, "OR needs a term on both sides");
            auto next = parse_and(depth);
            if (!next) return next;
            (*node)->children.push_back(std::move(*next));
        }
        return node;
    }

    /// and := unary (["AND"] unary)*
    Result<QueryNodePointer> parse_and(std::size_t depth) {
        std::vector<QueryNodePointer> terms;
        const std::size_t offset = peek().offset;
        while (peek().kind != Token::Kind::End && peek().kind != Token::Kind::CloseParen && !peek_keyword("OR")) {
            if (peek_keyword("AND")) {
                const auto and_offset = advance().offset;
                if (terms.empty() || at_term_boundary()) return error_at(and_offset, "AND needs a term on both sides");
                continue;
            }
            auto term = parse_unary(depth);
            if (!term) return term;
            terms.push_back(std::move(*term));
        }
        if (terms.empty()) {
            if (peek().kind == Token::Kind::CloseParen)
                return error_at(peek().offset, depth == 0 ? "unmatched ')'" : "empty parentheses");
            return error_at(offset, "expected a search term");
        }
        if (terms.size() == 1) return std::move(terms.front());
        auto node = make_node(QueryNode::Kind::And, offset);
        if (!node) return node;
        (*node)->children = std::move(terms);
        return node;
    }

    /// unary := "NOT" unary | "(" or ")" | word | phrase | field:value
    Result<QueryNodePointer> parse_unary(std::size_t depth) {
        if (peek_keyword("NOT")) {
            const auto offset = advance().offset;
            if (depth + 1 > kMaxQueryDepth) return error_at(offset, "query is nested more than 16 levels");
            if (at_term_boundary()) return error_at(offset, "NOT needs a term");
            auto operand = parse_unary(depth + 1);
            if (!operand) return operand;
            auto node = make_node(QueryNode::Kind::Not, offset);
            if (!node) return node;
            (*node)->children.push_back(std::move(*operand));
            return node;
        }
        if (peek().kind == Token::Kind::OpenParen) {
            const auto offset = advance().offset;
            auto inner = parse_or(depth + 1);
            if (!inner) return inner;
            if (peek().kind != Token::Kind::CloseParen) return error_at(offset, "unmatched '('");
            advance();
            return inner;
        }
        const Token& token = advance();
        switch (token.kind) {
            case Token::Kind::Word:
                return text_node(token, false);
            case Token::Kind::Quoted:
                return text_node(token, true);
            case Token::Kind::FieldValue:
                return predicate_node(token);
            case Token::Kind::OpenParen:
            case Token::Kind::CloseParen:
            case Token::Kind::End:
                break;
        }
        return error_at(token.offset, "expected a search term");
    }

    /// A free-text term; words may end in '*' (prefix), phrases keep '*' literally.
    Result<QueryNodePointer> text_node(const Token& token, bool phrase, std::optional<Field> column = std::nullopt) {
        auto node = make_node(QueryNode::Kind::Text, token.offset);
        if (!node) return node;
        TextTerm term{token.text, phrase, false, column};
        if (!phrase && term.text.ends_with('*')) {
            term.text.pop_back();
            if (count_code_points(term.text) < 2)
                return error_at(token.offset, "a prefix search needs at least two characters before '*'");
            term.prefix = true;
        }
        if (!phrase && term.text.find('*') != std::string::npos)
            return error_at(token.offset, "'*' is only allowed at the end of a word");
        if (trim(term.text).empty()) return error_at(token.offset, "empty search term");
        if (!has_letter_or_digit(term.text))
            return error_at(token.offset, "a search term needs at least one letter or digit");
        (*node)->text = std::move(term);
        return node;
    }

    // ------------------------------------------------------------ field values
    /// A `field:value` token as a predicate node (or a column-restricted text node for
    /// name:/path:/notes:).
    Result<QueryNodePointer> predicate_node(const Token& token) {
        auto field = field_from_name(token.field_name);
        if (!field) return error_at(token.offset, std::format("unknown field '{}'", token.field_name));
        if (*field == Field::Name || *field == Field::Path || *field == Field::Notes)
            return text_node(token, token.value_quoted, *field);
        auto node = make_node(QueryNode::Kind::Predicate, token.offset);
        if (!node) return node;
        FieldPredicate& predicate = (*node)->predicate;
        predicate.field = *field;
        const std::size_t value_offset = token.offset + token.field_name.size() + 1;
        if (auto parsed = parse_value(predicate, token.text, value_offset); !parsed)
            return std::unexpected(parsed.error());
        return node;
    }

    /// Parses the value of `predicate.field` into the predicate; `offset` is the value's byte
    /// offset for error positions.
    Result<void> parse_value(FieldPredicate& predicate, std::string_view value, std::size_t offset) {
        switch (predicate.field) {
            case Field::Folder:
                // Folders are addressed from a root down: "/Drums" means "Drums", and so does "\Drums"
                // on Windows, where path_key treats '\' as a separator (elsewhere it is a name byte).
                // Spaces around those separators go too ("/ Drums" is "Drums"), as around the value.
                predicate.text = trim(value);
                while (predicate.text.starts_with('/') || (kBackslashIsSeparator && predicate.text.starts_with('\\')))
                    predicate.text = trim(std::string_view(predicate.text).substr(1));
                if (predicate.text.empty()) return error_at(offset, "expected a folder name");
                return {};
            case Field::Tag:
            case Field::Category:
            case Field::Collection:
                predicate.text = trim(value);
                if (predicate.text.empty()) return error_at(offset, "expected a name");
                return {};
            case Field::Format:
            case Field::Codec:
                predicate.text = to_lower_ascii(trim(value));
                if (predicate.text.starts_with('.')) predicate.text.erase(0, 1);
                if (predicate.text.empty()) return error_at(offset, "expected a format or codec name");
                return {};
            case Field::Favorite:
            case Field::Available:
                return parse_flag(predicate, value, offset);
            case Field::Key:
                return parse_key_value(predicate, value, offset);
            case Field::Analyzed:
                return parse_analyzed(predicate, value, offset);
            case Field::Bpm:
                return parse_numeric(predicate, value, offset, {{"", 1000}, {"bpm", 1000}}, true);
            case Field::Duration:
                return parse_numeric(predicate, value, offset,
                                     {{"", 1'000'000}, {"s", 1'000'000}, {"ms", 1000}, {"min", 60'000'000}}, true);
            case Field::Size:
                return parse_numeric(predicate, value, offset,
                                     {{"", 1},
                                      {"b", 1},
                                      {"kb", 1'000},
                                      {"mb", 1'000'000},
                                      {"gb", 1'000'000'000},
                                      {"kib", 1024},
                                      {"mib", 1'048'576},
                                      {"gib", 1'073'741'824}},
                                     true);
            case Field::SampleRate:
                return parse_numeric(predicate, value, offset, {{"", 1}, {"hz", 1}, {"k", 1000}, {"khz", 1000}}, true);
            case Field::Rating:
            case Field::Channels:
            case Field::BitDepth: {
                auto parsed = parse_numeric(predicate, value, offset, {{"", 1}}, false);
                if (!parsed) return parsed;
                if (predicate.field == Field::Rating && !rating_in_range(predicate))
                    return error_at(offset, "rating is between 0 and 5");
                return {};
            }
            case Field::Name:
            case Field::Path:
            case Field::Notes:
                break;
        }
        return error_at(offset, "unsupported field");
    }

    /// The typed rating values (not the widened bounds) must lie in 0..5.
    static bool rating_in_range(const FieldPredicate& predicate) {
        switch (predicate.comparison) {
            case Comparison::Equal:
            case Comparison::Between:
                return predicate.low >= 0 && predicate.high <= 5;
            case Comparison::Less:
                return predicate.high >= -1 && predicate.high <= 4;
            case Comparison::LessOrEqual:
                return predicate.high >= 0 && predicate.high <= 5;
            case Comparison::Greater:
                return predicate.low >= 1 && predicate.low <= 6;
            case Comparison::GreaterOrEqual:
                return predicate.low >= 0 && predicate.low <= 5;
        }
        return false;
    }

    /// true/yes/1 or false/no/0 for favorite: and available:.
    Result<void> parse_flag(FieldPredicate& predicate, std::string_view value, std::size_t offset) {
        const std::string lowered = to_lower_ascii(value);
        if (lowered == "true" || lowered == "yes" || lowered == "1")
            predicate.flag = true;
        else if (lowered == "false" || lowered == "no" || lowered == "0")
            predicate.flag = false;
        else
            return error_at(offset, "expected true or false");
        return {};
    }

    /// A musical key ("F#minor", "Am", "Eb") or "unknown" (no key detected or set).
    Result<void> parse_key_value(FieldPredicate& predicate, std::string_view value, std::size_t offset) {
        if (to_lower_ascii(value) == "unknown") {
            predicate.text = "unknown";
            return {};
        }
        auto key = parse_key(value);
        if (!key) return error_at(offset, std::format("'{}' is not a key (try F#minor, Am, Eb)", value));
        predicate.key = *key;
        return {};
    }

    /// `analyzed:<stage>` (that stage recorded), `analyzed:any|true|yes` (any stage) or
    /// `analyzed:none|false|no` (no stage at all).
    Result<void> parse_analyzed(FieldPredicate& predicate, std::string_view value, std::size_t offset) {
        const std::string lowered = to_lower_ascii(value);
        predicate.flag = true;
        if (lowered == "tempo" || lowered == "bpm") {
            predicate.low = kAnalyzedTempo;
        } else if (lowered == "key") {
            predicate.low = kAnalyzedKey;
        } else if (lowered == "waveform") {
            predicate.low = kAnalyzedWaveform;
        } else if (lowered == "hash" || lowered == "duplicates") {
            predicate.low = kAnalyzedHash;
        } else if (lowered == "loudness") {
            predicate.low = kAnalyzedLoudness;
        } else if (lowered == "any" || lowered == "true" || lowered == "yes") {
            predicate.low = kAnalyzedAny;
        } else if (lowered == "none" || lowered == "false" || lowered == "no") {
            predicate.low = kAnalyzedAny;
            predicate.flag = false;
        } else {
            return error_at(offset, "analyzed: expects tempo, key, waveform, hash, loudness, any or none");
        }
        predicate.high = predicate.low;
        predicate.text = lowered;
        return {};
    }

    /// Parses "x", "<x", "<=x", ">x", ">=x", "=x" or "a..b" with optional unit suffixes into the
    /// inclusive integer bounds described at FieldPredicate. Equality and ranges are widened to
    /// the precision typed; `allow_fraction` = false rejects fractional values (count fields).
    Result<void> parse_numeric(FieldPredicate& predicate, std::string_view value, std::size_t offset,
                               std::initializer_list<Unit> units, bool allow_fraction) {
        std::string_view rest = value;
        std::size_t rest_offset = offset;
        predicate.comparison = Comparison::Equal;
        for (const auto& [prefix, comparison] :
             {std::pair{std::string_view(">="), Comparison::GreaterOrEqual},
              std::pair{std::string_view("<="), Comparison::LessOrEqual},
              std::pair{std::string_view(">"), Comparison::Greater}, std::pair{std::string_view("<"), Comparison::Less},
              std::pair{std::string_view("="), Comparison::Equal}}) {
            if (!rest.starts_with(prefix)) continue;
            predicate.comparison = comparison;
            rest.remove_prefix(prefix.size());
            rest_offset += prefix.size();
            break;
        }

        const auto range = rest.find("..");
        if (range != std::string_view::npos) {
            if (predicate.comparison != Comparison::Equal)
                return error_at(offset, "a range cannot also have a comparison");
            auto low = parse_quantity(rest.substr(0, range), rest_offset, units, allow_fraction);
            if (!low) return std::unexpected(low.error());
            auto high = parse_quantity(rest.substr(range + 2), rest_offset + range + 2, units, allow_fraction);
            if (!high) return std::unexpected(high.error());
            auto low_bound = precision_lower_bound(*low, offset);
            if (!low_bound) return std::unexpected(low_bound.error());
            auto high_bound = precision_upper_bound(*high, offset);
            if (!high_bound) return std::unexpected(high_bound.error());
            auto low_exact = exact_floor(*low, offset);
            if (!low_exact) return std::unexpected(low_exact.error());
            auto high_exact = exact_floor(*high, offset);
            if (!high_exact) return std::unexpected(high_exact.error());
            if (*low_exact > *high_exact) return error_at(offset, "range start is greater than its end");
            predicate.comparison = Comparison::Between;
            predicate.low = *low_bound;
            predicate.high = *high_bound;
            return {};
        }
        auto quantity = parse_quantity(rest, rest_offset, units, allow_fraction);
        if (!quantity) return std::unexpected(quantity.error());
        Result<std::int64_t> low = 0;
        Result<std::int64_t> high = 0;
        switch (predicate.comparison) {
            case Comparison::Equal:
            case Comparison::Between:
                low = precision_lower_bound(*quantity, offset);
                high = precision_upper_bound(*quantity, offset);
                break;
            case Comparison::Less:  // value < x  ⇔  value <= ⌈x⌉ - 1
                high = exact_ceiling(*quantity, offset).transform([](std::int64_t bound) { return bound - 1; });
                break;
            case Comparison::LessOrEqual:  // value <= x  ⇔  value <= ⌊x⌋
                high = exact_floor(*quantity, offset);
                break;
            case Comparison::Greater:  // value > x  ⇔  value >= ⌊x⌋ + 1
                low = exact_floor(*quantity, offset).transform([](std::int64_t bound) { return bound + 1; });
                break;
            case Comparison::GreaterOrEqual:  // value >= x  ⇔  value >= ⌈x⌉
                low = exact_ceiling(*quantity, offset);
                break;
        }
        if (!low) return std::unexpected(low.error());
        if (!high) return std::unexpected(high.error());
        predicate.low = *low;
        predicate.high = *high;
        return {};
    }

    /// A typed number with its unit: value = mantissa × 10^-decimals × storage_units.
    struct Quantity {
        Decimal number;
        std::int64_t storage_units = 1;
    };

    /// A non-negative decimal with an optional unit suffix from `units` (ASCII case-insensitive).
    Result<Quantity> parse_quantity(std::string_view text, std::size_t offset, std::initializer_list<Unit> units,
                                    bool allow_fraction) {
        Decimal number;
        std::size_t position = 0;
        int digits = 0;
        bool seen_point = false;
        for (; position < text.size(); ++position) {
            const char character = text[position];
            if (character == '.' && !seen_point && !(position + 1 < text.size() && text[position + 1] == '.')) {
                seen_point = true;
                continue;
            }
            if (character < '0' || character > '9') break;
            if (++digits > kMaxDigits) return error_at(offset, "number has too many digits");
            number.mantissa = number.mantissa * 10 + (character - '0');
            if (seen_point) ++number.decimals;
        }
        if (digits == 0) return error_at(offset, std::format("'{}' is not a number", text));
        if (number.decimals > kMaxDecimals) return error_at(offset, "number has too many decimals");
        if (!allow_fraction && number.decimals > 0) return error_at(offset, "expected a whole number");
        const std::string suffix = to_lower_ascii(text.substr(position));
        for (const auto& unit : units)
            if (unit.suffix == suffix) return Quantity{number, unit.storage_units};
        return error_at(offset + position, std::format("unknown unit '{}'", suffix));
    }

    /// (mantissa × `mantissa_multiplier` + `addend`) × storage_units / (`divisor` × 10^decimals),
    /// rounded to an integer with `round`; a parse error when it overflows or exceeds the limit.
    template <class Round>
    Result<std::int64_t> scaled(const Quantity& quantity, std::int64_t mantissa_multiplier, std::int64_t addend,
                                std::int64_t divisor, std::size_t offset, Round round) {
        std::int64_t power = 1;
        for (int index = 0; index < quantity.number.decimals; ++index)
            power *= 10;
        std::int64_t numerator = 0;
        std::int64_t denominator = 0;
        if (__builtin_mul_overflow(quantity.number.mantissa, mantissa_multiplier, &numerator) ||
            __builtin_add_overflow(numerator, addend, &numerator) ||
            __builtin_mul_overflow(numerator, quantity.storage_units, &numerator) ||
            __builtin_mul_overflow(divisor, power, &denominator))
            return error_at(offset, "number is too large");
        const std::int64_t result = round(numerator, denominator);
        if (result > kMaxStorageValue) return error_at(offset, "number is too large");
        return result;
    }

    /// ⌈x - h⌉ where h is half of the last typed digit: the smallest stored value that rounds to x.
    Result<std::int64_t> precision_lower_bound(const Quantity& quantity, std::size_t offset) {
        return scaled(quantity, 2, -1, 2, offset, ceiling_divide);
    }
    /// ⌈x + h⌉ - 1: the largest stored value that rounds to x (x + h itself rounds up).
    Result<std::int64_t> precision_upper_bound(const Quantity& quantity, std::size_t offset) {
        return scaled(quantity, 2, 1, 2, offset, ceiling_divide).transform([](std::int64_t bound) {
            return bound - 1;
        });
    }
    /// ⌊x⌋ in storage units.
    Result<std::int64_t> exact_floor(const Quantity& quantity, std::size_t offset) {
        return scaled(quantity, 1, 0, 1, offset, floor_divide);
    }
    /// ⌈x⌉ in storage units.
    Result<std::int64_t> exact_ceiling(const Quantity& quantity, std::size_t offset) {
        return scaled(quantity, 1, 0, 1, offset, ceiling_divide);
    }

    std::string_view text_;       ///< the whole query
    std::vector<Token> tokens_;   ///< the tokenized query, ending with an End token
    std::size_t index_ = 0;       ///< next token to consume
    std::size_t node_count_ = 0;  ///< nodes created so far (kMaxQueryNodes)
};

/// Appends the text terms of `node` that are not under an odd number of NOTs.
void collect_positive_terms(const QueryNode& node, bool negated, std::vector<TextTerm>& terms) {
    switch (node.kind) {
        case QueryNode::Kind::Text:
            if (!negated) terms.push_back(node.text);
            return;
        case QueryNode::Kind::Not:
            collect_positive_terms(*node.children.front(), !negated, terms);
            return;
        case QueryNode::Kind::And:
        case QueryNode::Kind::Or:
            for (const auto& child : node.children)
                collect_positive_terms(*child, negated, terms);
            return;
        case QueryNode::Kind::Predicate:
            return;
    }
}

/// True when every match of `node` matches one of its positive text terms.
bool node_requires_text_match(const QueryNode& node) {
    switch (node.kind) {
        case QueryNode::Kind::Text:
            return true;
        case QueryNode::Kind::And:
            return std::ranges::any_of(node.children,
                                       [](const auto& child) { return node_requires_text_match(*child); });
        case QueryNode::Kind::Or:
            return std::ranges::all_of(node.children,
                                       [](const auto& child) { return node_requires_text_match(*child); });
        case QueryNode::Kind::Not:
        case QueryNode::Kind::Predicate:
            return false;
    }
    return false;
}

}  // namespace

Result<Query> parse_query(std::string_view text) {
    return Parser(text).parse();
}

std::vector<TextTerm> positive_text_terms(const Query& query) {
    std::vector<TextTerm> terms;
    if (query.root) collect_positive_terms(*query.root, false, terms);
    return terms;
}

bool requires_text_match(const Query& query) {
    return query.root && node_requires_text_match(*query.root);
}

}  // namespace mso::search
