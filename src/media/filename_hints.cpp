// Filename heuristics for BPM and key. Conservative by design: a wrong hint is worse than none,
// so ambiguous or conflicting tokens produce no proposal at all.
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "mso/domain/core.hpp"
#include "mso/media/media.hpp"

namespace mso::media {

namespace {

constexpr double kMinMarkedBpm = 20.0;  // "NNbpm" tokens: the UI's manual range
constexpr double kMaxMarkedBpm = 400.0;
constexpr int kMinBareBpm = 60;  // unmarked numbers: only typical musical tempos
constexpr int kMaxBareBpm = 200;

/// Hardware model prefixes whose numbers are not tempos ("SH 101", "Juno 106", "TR 808").
constexpr std::array<std::string_view, 20> kModelPrefixes = {
    "tr",  "tb", "sh", "mc", "sp", "jx", "jd", "jp", "juno", "cr",
    "mpc", "dx", "ms", "rx", "rz", "cz", "sk", "op", "lm",   "dmx",
};

/// Words after a number that make it a measurement or count, not a tempo.
constexpr std::array<std::string_view, 18> kUnitWords = {
    "hz", "khz",   "ms", "s", "sec", "db",  "k",    "bit",  "bits",
    "st", "cents", "ct", "x", "v",   "bar", "bars", "beat", "beats",
};

/// Words before a number that make it an index ("Take 120", "Vol 100", "No 75").
constexpr std::array<std::string_view, 9> kIndexWords = {
    "take", "vol", "volume", "no", "part", "pt", "track", "version", "ver",
};

bool contains(auto& list, std::string_view word) {
    return std::ranges::find(list, word) != list.end();
}

bool is_word_character(char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte >= 0x80 || std::isalnum(byte) || character == '#';  // non-ASCII bytes belong to words
}

/// Replaces the Unicode sharp and flat signs with '#' and 'b' so "F♯m" reads like "F#m".
std::string normalize_accidentals(std::string_view stem) {
    std::string text(stem);
    for (const auto& [sign, ascii] : {std::pair{std::string_view("♯"), std::string_view("#")},
                                      std::pair{std::string_view("♭"), std::string_view("b")}}) {
        for (std::size_t found = text.find(sign); found != std::string::npos; found = text.find(sign, found)) {
            text.replace(found, sign.size(), ascii);
        }
    }
    return text;
}

/// Splits a filename stem into word tokens. A '.' or ',' between two digits stays inside the
/// token so "127.5bpm" survives.
std::vector<std::string> tokenize(std::string_view stem) {
    std::vector<std::string> tokens;
    std::string current;
    for (std::size_t index = 0; index < stem.size(); ++index) {
        const char character = stem[index];
        const bool decimal_point = (character == '.' || character == ',') && !current.empty() &&
                                   std::isdigit(static_cast<unsigned char>(current.back())) &&
                                   index + 1 < stem.size() && std::isdigit(static_cast<unsigned char>(stem[index + 1]));
        if (is_word_character(character) || decimal_point) {
            current.push_back(character == ',' ? '.' : character);
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

/// A tempo written as 2-3 digits with an optional decimal part ("128", "127.5").
std::optional<double> parse_tempo_number(std::string_view text) {
    const std::size_t integer_digits = std::min(text.find('.'), text.size());
    if (integer_digits < 2 || integer_digits > 3) return std::nullopt;
    if (!std::ranges::all_of(text.substr(0, integer_digits),
                             [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        return std::nullopt;
    }
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

std::optional<std::int64_t> marked_bpm(double value) {
    if (value < kMinMarkedBpm || value > kMaxMarkedBpm) return std::nullopt;
    return static_cast<std::int64_t>(std::llround(value * 1000.0));
}

/// Tempos marked with "bpm": "128bpm", "bpm128", "128 BPM", "BPM 128".
std::set<std::int64_t> marked_tempos(const std::vector<std::string>& lowered) {
    std::set<std::int64_t> tempos;
    for (std::size_t index = 0; index < lowered.size(); ++index) {
        const std::string_view token = lowered[index];
        std::optional<double> value;
        if (token.size() > 3 && token.ends_with("bpm"))
            value = parse_tempo_number(token.substr(0, token.size() - 3));
        else if (token.size() > 3 && token.starts_with("bpm"))
            value = parse_tempo_number(token.substr(3));
        else if (token == "bpm" && index > 0 && parse_tempo_number(lowered[index - 1]))
            value = parse_tempo_number(lowered[index - 1]);
        else if (token == "bpm" && index + 1 < lowered.size())
            value = parse_tempo_number(lowered[index + 1]);
        if (!value) continue;
        if (const auto bpm = marked_bpm(*value)) tempos.insert(*bpm);
    }
    return tempos;
}

/// Unmarked numbers that are plausibly tempos: 2-3 digits, no leading zero, 60-200, not the
/// first token (track numbers), not a model number, unit or index.
std::set<std::int64_t> bare_tempos(const std::vector<std::string>& lowered) {
    std::set<std::int64_t> tempos;
    for (std::size_t index = 1; index < lowered.size(); ++index) {
        const std::string& token = lowered[index];
        if (token.size() < 2 || token.size() > 3 || token.front() == '0') continue;
        if (!std::ranges::all_of(token, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) continue;
        const int value = std::stoi(token);
        if (value < kMinBareBpm || value > kMaxBareBpm) continue;
        const std::string_view previous = lowered[index - 1];
        if (contains(kModelPrefixes, previous) || contains(kIndexWords, previous)) continue;
        if (index + 1 < lowered.size() && contains(kUnitWords, lowered[index + 1])) continue;
        tempos.insert(static_cast<std::int64_t>(value) * 1000);
    }
    return tempos;
}

/// A key spelled in one token: upper-case letter, optional '#'/'b', and a mode suffix
/// ("m", "min", "minor", "maj", "major"); or just a root, whose mode is then nullopt.
struct KeyToken {
    int pitch_class = 0;
    std::optional<KeyMode> mode;
    bool has_accidental = false;
};

std::optional<KeyToken> parse_key_token(std::string_view token) {
    if (token.empty() || token[0] < 'A' || token[0] > 'G') return std::nullopt;
    std::size_t root_length = 1;
    if (token.size() > 1 && (token[1] == '#' || token[1] == 'b')) root_length = 2;
    const auto root = parse_key(token.substr(0, root_length));
    if (!root) return std::nullopt;
    KeyToken key{root->pitch_class, std::nullopt, root_length == 2};
    const std::string_view suffix = token.substr(root_length);
    const std::string folded = to_lower_ascii(suffix);
    if (suffix == "m" || folded == "min" || folded == "minor")
        key.mode = KeyMode::Minor;
    else if (folded == "maj" || folded == "major")
        key.mode = KeyMode::Major;
    else if (!suffix.empty())
        return std::nullopt;
    return key;
}

std::optional<KeyMode> parse_mode_word(std::string_view lowered) {
    if (lowered == "minor" || lowered == "min") return KeyMode::Minor;
    if (lowered == "major" || lowered == "maj") return KeyMode::Major;
    return std::nullopt;
}

/// Key candidates. A root with an accidental ("F#", "Bb") or after the word "key" is kept with
/// an unknown mode; a bare letter elsewhere is too ambiguous ("Snare A", "A Day").
std::vector<MusicalKey> key_candidates(const std::vector<std::string>& tokens,
                                       const std::vector<std::string>& lowered) {
    std::vector<MusicalKey> keys;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const auto key = parse_key_token(tokens[index]);
        if (!key) continue;
        const bool after_pronoun = index > 0 && lowered[index - 1] == "i";  // "I Am ..."
        if (key->mode) {
            if (!after_pronoun) keys.push_back({key->pitch_class, *key->mode});
            continue;
        }
        if (index + 1 < tokens.size()) {
            if (const auto mode = parse_mode_word(lowered[index + 1])) {
                keys.push_back({key->pitch_class, *mode});
                ++index;
                continue;
            }
        }
        const bool after_key_word = index > 0 && lowered[index - 1] == "key";
        if (key->has_accidental || after_key_word) keys.push_back({key->pitch_class, KeyMode::Unknown});
    }
    return keys;
}

/// One agreed key, preferring candidates that state a mode; nullopt when candidates conflict.
std::optional<MusicalKey> agreed_key(const std::vector<MusicalKey>& keys) {
    std::optional<MusicalKey> chosen;
    for (const MusicalKey& key : keys) {
        if (!chosen) {
            chosen = key;
        } else if (chosen->pitch_class != key.pitch_class) {
            return std::nullopt;
        } else if (chosen->mode == KeyMode::Unknown) {
            chosen = key;
        } else if (key.mode != KeyMode::Unknown && key.mode != chosen->mode) {
            return std::nullopt;
        }
    }
    return chosen;
}

}  // namespace

FilenameHints filename_hints(std::string_view stem_utf8) {
    const std::vector<std::string> tokens = tokenize(normalize_accidentals(stem_utf8));
    std::vector<std::string> lowered;
    lowered.reserve(tokens.size());
    for (const std::string& token : tokens)
        lowered.push_back(to_lower_ascii(token));

    FilenameHints hints;
    const auto marked = marked_tempos(lowered);
    if (marked.size() == 1) {
        hints.bpm_milli = *marked.begin();
    } else if (marked.empty()) {
        const auto bare = bare_tempos(lowered);
        if (bare.size() == 1) hints.bpm_milli = *bare.begin();
    }
    hints.key = agreed_key(key_candidates(tokens, lowered));
    return hints;
}

}  // namespace mso::media
