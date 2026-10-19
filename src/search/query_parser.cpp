// Recursive-descent parser for the search query language (see query.hpp).
#include <charconv>
#include <cmath>
#include <format>
#include <utility>

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

/// Bit flags stored in effective_metadata.analysis_flags (see Library::record_analysis).
constexpr std::int64_t kAnalyzedTempo = 1;
constexpr std::int64_t kAnalyzedKey = 2;
constexpr std::int64_t kAnalyzedWaveform = 4;
constexpr std::int64_t kAnalyzedHash = 8;
constexpr std::int64_t kAnalyzedLoudness = 16;

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
    enum class Kind { Word, Quoted, FieldValue, OpenParen, CloseParen, End };
    Kind kind = Kind::End;
    std::string text;           ///< word / quoted content / field value
    std::string field_name;     ///< FieldValue only
    bool value_quoted = false;  ///< FieldValue only
    std::size_t offset = 0;
};

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

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
    std::unexpected<Error> error_at(std::size_t byte_offset, std::string message) const {
        Error error{ErrorCode::ParseError, false, std::move(message), std::nullopt,
                    static_cast<std::uint32_t>(count_code_points(text_.substr(0, byte_offset)))};
        return std::unexpected(std::move(error));
    }

    // ------------------------------------------------------------ tokenizer
    static bool is_space(char character) {
        return character == ' ' || character == '\t' || character == '\n' || character == '\r';
    }

    /// Reads a quoted string starting at the opening quote; returns the unescaped content.
    Result<std::string> read_quoted(std::size_t& position) {
        const std::size_t start = position++;
        std::string content;
        while (position < text_.size()) {
            const char character = text_[position++];
            if (character == '"') return content;
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

    const Token& peek() const { return tokens_[index_]; }
    const Token& advance() { return tokens_[index_++]; }
    bool peek_keyword(std::string_view keyword) const {
        return peek().kind == Token::Kind::Word && peek().text == keyword;  // operators are upper-case only
    }

    // ------------------------------------------------------------ grammar
    Result<QueryNodePointer> make_node(QueryNode::Kind kind, std::size_t offset) {
        if (++node_count_ > kMaxQueryNodes) return error_at(offset, "query has more than 256 terms");
        auto node = std::make_unique<QueryNode>();
        node->kind = kind;
        return node;
    }

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
            if (peek().kind == Token::Kind::End || peek().kind == Token::Kind::CloseParen)
                return error_at(offset, "OR needs a term on both sides");
            auto next = parse_and(depth);
            if (!next) return next;
            (*node)->children.push_back(std::move(*next));
        }
        return node;
    }

    Result<QueryNodePointer> parse_and(std::size_t depth) {
        std::vector<QueryNodePointer> terms;
        const std::size_t offset = peek().offset;
        while (peek().kind != Token::Kind::End && peek().kind != Token::Kind::CloseParen && !peek_keyword("OR")) {
            if (peek_keyword("AND")) {
                const auto and_offset = advance().offset;
                if (terms.empty() || peek().kind == Token::Kind::End || peek().kind == Token::Kind::CloseParen ||
                    peek_keyword("OR"))
                    return error_at(and_offset, "AND needs a term on both sides");
                continue;
            }
            auto term = parse_unary(depth);
            if (!term) return term;
            terms.push_back(std::move(*term));
        }
        if (terms.empty()) return error_at(offset, "expected a search term");
        if (terms.size() == 1) return std::move(terms.front());
        auto node = make_node(QueryNode::Kind::And, offset);
        if (!node) return node;
        (*node)->children = std::move(terms);
        return node;
    }

    Result<QueryNodePointer> parse_unary(std::size_t depth) {
        if (peek_keyword("NOT")) {
            const auto offset = advance().offset;
            if (peek().kind == Token::Kind::End || peek().kind == Token::Kind::CloseParen || peek_keyword("OR") ||
                peek_keyword("AND"))
                return error_at(offset, "NOT needs a term");
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
            default:
                return error_at(token.offset, "expected a search term");
        }
    }

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
        if (term.text.find('*') != std::string::npos)
            return error_at(token.offset, "'*' is only allowed at the end of a word");
        if (trim(term.text).empty()) return error_at(token.offset, "empty search term");
        (*node)->text = std::move(term);
        return node;
    }

    // ------------------------------------------------------------ field values
    Result<QueryNodePointer> predicate_node(const Token& token) {
        auto field = field_from_name(token.field_name);
        if (!field) return error_at(token.offset, std::format("unknown field '{}'", token.field_name));
        if (*field == Field::Name || *field == Field::Path || *field == Field::Notes) {
            Token text_token = token;
            return text_node(text_token, token.value_quoted, *field);
        }
        auto node = make_node(QueryNode::Kind::Predicate, token.offset);
        if (!node) return node;
        FieldPredicate& predicate = (*node)->predicate;
        predicate.field = *field;
        const std::size_t value_offset = token.offset + token.field_name.size() + 1;
        if (auto parsed = parse_value(predicate, token.text, value_offset); !parsed)
            return std::unexpected(parsed.error());
        return node;
    }

    Result<void> parse_value(FieldPredicate& predicate, std::string_view value, std::size_t offset) {
        switch (predicate.field) {
            case Field::Tag:
            case Field::Category:
            case Field::Folder:
            case Field::Collection:
                predicate.text = trim(value);
                return {};
            case Field::Format:
            case Field::Codec:
                predicate.text = to_lower_ascii(trim(value));
                if (predicate.text.starts_with('.')) predicate.text.erase(0, 1);
                return {};
            case Field::Favorite:
            case Field::Available:
                return parse_flag(predicate, value, offset);
            case Field::Key:
                return parse_key_value(predicate, value, offset);
            case Field::Analyzed:
                return parse_analyzed(predicate, value, offset);
            case Field::Bpm:
                return parse_numeric(predicate, value, offset, 1000, {{"", 1}, {"bpm", 1}});
            case Field::Duration:
                return parse_numeric(predicate, value, offset, 1'000'000,
                                     {{"", 1}, {"s", 1}, {"ms", 0.001}, {"min", 60}});
            case Field::Size:
                return parse_numeric(predicate, value, offset, 1,
                                     {{"", 1},
                                      {"b", 1},
                                      {"kb", 1e3},
                                      {"mb", 1e6},
                                      {"gb", 1e9},
                                      {"kib", 1024},
                                      {"mib", 1048576},
                                      {"gib", 1073741824}});
            case Field::SampleRate:
                return parse_numeric(predicate, value, offset, 1, {{"", 1}, {"hz", 1}, {"k", 1000}, {"khz", 1000}});
            case Field::Rating:
            case Field::Channels:
            case Field::BitDepth: {
                auto parsed = parse_numeric(predicate, value, offset, 1, {{"", 1}});
                if (!parsed) return parsed;
                if (predicate.field == Field::Rating && (predicate.low < 0 || predicate.high > 5 || predicate.low > 5))
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

    Result<void> parse_analyzed(FieldPredicate& predicate, std::string_view value, std::size_t offset) {
        const std::string lowered = to_lower_ascii(value);
        if (lowered == "tempo" || lowered == "bpm")
            predicate.low = kAnalyzedTempo;
        else if (lowered == "key")
            predicate.low = kAnalyzedKey;
        else if (lowered == "waveform")
            predicate.low = kAnalyzedWaveform;
        else if (lowered == "hash" || lowered == "duplicates")
            predicate.low = kAnalyzedHash;
        else if (lowered == "loudness")
            predicate.low = kAnalyzedLoudness;
        else if (lowered == "any" || lowered == "true")
            predicate.low = 0xFF;
        else
            return error_at(offset, "analyzed: expects tempo, key, waveform, hash, loudness or any");
        predicate.text = lowered;
        return {};
    }

    /// A unit suffix and its multiplier relative to the field's base unit.
    struct Unit {
        std::string_view suffix;
        double multiplier;
    };

    /// Parses "x", "<x", "<=x", ">x", ">=x", "=x" or "a..b" with optional unit suffixes, scaled by
    /// `scale` into integers (e.g. BPM → milli-BPM).
    Result<void> parse_numeric(FieldPredicate& predicate, std::string_view value, std::size_t offset, double scale,
                               std::initializer_list<Unit> units) {
        std::string_view rest = value;
        predicate.comparison = Comparison::Equal;
        if (rest.starts_with(">=")) {
            predicate.comparison = Comparison::GreaterOrEqual;
            rest.remove_prefix(2);
        } else if (rest.starts_with("<=")) {
            predicate.comparison = Comparison::LessOrEqual;
            rest.remove_prefix(2);
        } else if (rest.starts_with('>')) {
            predicate.comparison = Comparison::Greater;
            rest.remove_prefix(1);
        } else if (rest.starts_with('<')) {
            predicate.comparison = Comparison::Less;
            rest.remove_prefix(1);
        } else if (rest.starts_with('=')) {
            rest.remove_prefix(1);
        }

        const auto range = rest.find("..");
        if (range != std::string_view::npos) {
            if (predicate.comparison != Comparison::Equal)
                return error_at(offset, "a range cannot also have a comparison");
            auto low = parse_quantity(rest.substr(0, range), offset, scale, units);
            auto high = parse_quantity(rest.substr(range + 2), offset, scale, units);
            if (!low) return std::unexpected(low.error());
            if (!high) return std::unexpected(high.error());
            if (*low > *high) return error_at(offset, "range start is greater than its end");
            predicate.comparison = Comparison::Between;
            predicate.low = *low;
            predicate.high = *high;
            return {};
        }
        auto quantity = parse_quantity(rest, offset, scale, units);
        if (!quantity) return std::unexpected(quantity.error());
        predicate.low = predicate.high = *quantity;
        return {};
    }

    Result<std::int64_t> parse_quantity(std::string_view text, std::size_t offset, double scale,
                                        std::initializer_list<Unit> units) {
        std::size_t number_end = 0;
        while (number_end < text.size() &&
               ((text[number_end] >= '0' && text[number_end] <= '9') || text[number_end] == '.'))
            ++number_end;
        if (number_end == 0) return error_at(offset, std::format("'{}' is not a number", text));
        double number = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + number_end, number);
        if (parsed.ec != std::errc() || parsed.ptr != text.data() + number_end)
            return error_at(offset, std::format("'{}' is not a number", text.substr(0, number_end)));
        const std::string suffix = to_lower_ascii(text.substr(number_end));
        for (const auto& unit : units) {
            if (unit.suffix != suffix) continue;
            const double scaled = std::round(number * unit.multiplier * scale);
            if (!std::isfinite(scaled) || scaled > 9.0e15) return error_at(offset, "number is too large");
            return static_cast<std::int64_t>(scaled);
        }
        return error_at(offset + number_end, std::format("unknown unit '{}'", suffix));
    }

    std::string_view text_;
    std::vector<Token> tokens_;
    std::size_t index_ = 0;
    std::size_t node_count_ = 0;
};

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

}  // namespace

Result<Query> parse_query(std::string_view text) {
    return Parser(text).parse();
}

std::vector<TextTerm> positive_text_terms(const Query& query) {
    std::vector<TextTerm> terms;
    if (query.root) collect_positive_terms(*query.root, false, terms);
    return terms;
}

}  // namespace mso::search
