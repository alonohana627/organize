#include "mso/media/detail/tag_mapping.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

#include "mso/domain/core.hpp"

namespace mso::media::detail {

namespace {

constexpr std::size_t kMaxTextBytes = 1024;   // longer values are cut at a code point boundary
constexpr std::size_t kMaxKeywordBytes = 64;  // keywords are short labels, not prose
constexpr std::size_t kMaxKeywords = 32;
constexpr double kMinTagBpm = 1.0;
constexpr double kMaxTagBpm = 999.0;

/// Cuts UTF-8 text to at most `limit` bytes without splitting a multi-byte sequence.
std::string truncate_utf8(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) return std::string(text);
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
        --end;
    return std::string(text.substr(0, end));
}

/// Stores `value` (cut to kMaxTextBytes) unless the slot already holds a value.
void store_text_once(std::optional<std::string>& slot, std::string_view value) {
    if (!slot) slot = truncate_utf8(value, kMaxTextBytes);
}

/// Splits `value` on ';' and ',' and appends new, short keywords (at most kMaxKeywords in total).
void add_keywords(std::vector<std::string>& keywords, std::string_view value) {
    std::size_t start = 0;
    while (start <= value.size() && keywords.size() < kMaxKeywords) {
        const std::size_t end = std::min(value.find_first_of(";,", start), value.size());
        const std::string keyword = trim(value.substr(start, end - start));
        if (!keyword.empty() && keyword.size() <= kMaxKeywordBytes &&
            std::ranges::find(keywords, keyword) == keywords.end()) {
            keywords.push_back(keyword);
        }
        start = end + 1;
    }
}

/// Camelot wheel notation used by DJ software: 1A..12A minor, 1B..12B major.
std::optional<MusicalKey> parse_camelot_key(std::string_view text) {
    if (text.size() < 2 || text.size() > 3) return std::nullopt;
    const char letter = static_cast<char>(text.back() & ~0x20);  // ASCII upper case
    if (letter != 'A' && letter != 'B') return std::nullopt;
    int number = 0;
    const auto digits = text.substr(0, text.size() - 1);
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (error != std::errc{} || end != digits.data() + digits.size() || number < 1 || number > 12) return std::nullopt;
    // Each step clockwise is a fifth (7 semitones); 1A is A-flat minor and 1B is B major.
    const int minor_root = 8;
    const int major_root = 11;
    const int steps = 7 * (number - 1);
    if (letter == 'A') return MusicalKey{(minor_root + steps) % 12, KeyMode::Minor};
    return MusicalKey{(major_root + steps) % 12, KeyMode::Major};
}

/// Open Key notation (Traktor): 1d..12d major, 1m..12m minor. 1d is C major and 1m is A minor;
/// each step is a fifth (7 semitones), like Camelot shifted by seven positions.
std::optional<MusicalKey> parse_open_key(std::string_view text) {
    if (text.size() < 2 || text.size() > 3) return std::nullopt;
    const char letter = static_cast<char>(text.back() | 0x20);  // ASCII lower case
    if (letter != 'd' && letter != 'm') return std::nullopt;
    int number = 0;
    const auto digits = text.substr(0, text.size() - 1);
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (error != std::errc{} || end != digits.data() + digits.size() || number < 1 || number > 12) return std::nullopt;
    const int major_root = 0;
    const int minor_root = 9;
    const int steps = 7 * (number - 1);
    if (letter == 'm') return MusicalKey{(minor_root + steps) % 12, KeyMode::Minor};
    return MusicalKey{(major_root + steps) % 12, KeyMode::Major};
}

/// ID3v1 genres indexed by genre number: the 80 of the original specification followed by the
/// Winamp extensions 80-191, as TagLib and mutagen list them (133 uses its modern name).
constexpr std::array<std::string_view, 192> kId3v1Genres = {
    "Blues",
    "Classic Rock",
    "Country",
    "Dance",
    "Disco",
    "Funk",
    "Grunge",
    "Hip-Hop",
    "Jazz",
    "Metal",
    "New Age",
    "Oldies",
    "Other",
    "Pop",
    "R&B",
    "Rap",
    "Reggae",
    "Rock",
    "Techno",
    "Industrial",
    "Alternative",
    "Ska",
    "Death Metal",
    "Pranks",
    "Soundtrack",
    "Euro-Techno",
    "Ambient",
    "Trip-Hop",
    "Vocal",
    "Jazz+Funk",
    "Fusion",
    "Trance",
    "Classical",
    "Instrumental",
    "Acid",
    "House",
    "Game",
    "Sound Clip",
    "Gospel",
    "Noise",
    "AlternRock",
    "Bass",
    "Soul",
    "Punk",
    "Space",
    "Meditative",
    "Instrumental Pop",
    "Instrumental Rock",
    "Ethnic",
    "Gothic",
    "Darkwave",
    "Techno-Industrial",
    "Electronic",
    "Pop-Folk",
    "Eurodance",
    "Dream",
    "Southern Rock",
    "Comedy",
    "Cult",
    "Gangsta",
    "Top 40",
    "Christian Rap",
    "Pop/Funk",
    "Jungle",
    "Native American",
    "Cabaret",
    "New Wave",
    "Psychedelic",
    "Rave",
    "Showtunes",
    "Trailer",
    "Lo-Fi",
    "Tribal",
    "Acid Punk",
    "Acid Jazz",
    "Polka",
    "Retro",
    "Musical",
    "Rock & Roll",
    "Hard Rock",
    "Folk",
    "Folk-Rock",
    "National Folk",
    "Swing",
    "Fast Fusion",
    "Bebop",
    "Latin",
    "Revival",
    "Celtic",
    "Bluegrass",
    "Avantgarde",
    "Gothic Rock",
    "Progressive Rock",
    "Psychedelic Rock",
    "Symphonic Rock",
    "Slow Rock",
    "Big Band",
    "Chorus",
    "Easy Listening",
    "Acoustic",
    "Humour",
    "Speech",
    "Chanson",
    "Opera",
    "Chamber Music",
    "Sonata",
    "Symphony",
    "Booty Bass",
    "Primus",
    "Porn Groove",
    "Satire",
    "Slow Jam",
    "Club",
    "Tango",
    "Samba",
    "Folklore",
    "Ballad",
    "Power Ballad",
    "Rhythmic Soul",
    "Freestyle",
    "Duet",
    "Punk Rock",
    "Drum Solo",
    "A Cappella",
    "Euro-House",
    "Dance Hall",
    "Goa",
    "Drum & Bass",
    "Club-House",
    "Hardcore Techno",
    "Terror",
    "Indie",
    "BritPop",
    "Afro-Punk",
    "Polsk Punk",
    "Beat",
    "Christian Gangsta Rap",
    "Heavy Metal",
    "Black Metal",
    "Crossover",
    "Contemporary Christian",
    "Christian Rock",
    "Merengue",
    "Salsa",
    "Thrash Metal",
    "Anime",
    "JPop",
    "Synthpop",
    "Abstract",
    "Art Rock",
    "Baroque",
    "Bhangra",
    "Big Beat",
    "Breakbeat",
    "Chillout",
    "Downtempo",
    "Dub",
    "EBM",
    "Eclectic",
    "Electro",
    "Electroclash",
    "Emo",
    "Experimental",
    "Garage",
    "Global",
    "IDM",
    "Illbient",
    "Industro-Goth",
    "Jam Band",
    "Krautrock",
    "Leftfield",
    "Lounge",
    "Math Rock",
    "New Romantic",
    "Nu-Breakz",
    "Post-Punk",
    "Post-Rock",
    "Psytrance",
    "Shoegaze",
    "Space Rock",
    "Trop Rock",
    "World Music",
    "Neoclassical",
    "Audiobook",
    "Audio Theatre",
    "Neue Deutsche Welle",
    "Podcast",
    "Indie Rock",
    "G-Funk",
    "Dubstep",
    "Garage Rock",
    "Psybient",
};

}  // namespace

std::optional<std::string_view> id3v1_genre_name(int genre_number) noexcept {
    if (genre_number < 0 || genre_number >= static_cast<int>(kId3v1Genres.size())) return std::nullopt;
    return kId3v1Genres[static_cast<std::size_t>(genre_number)];
}

std::optional<std::int64_t> milli_bpm_from_value(double bpm) noexcept {
    if (!std::isfinite(bpm) || bpm < kMinTagBpm || bpm > kMaxTagBpm) return std::nullopt;
    return static_cast<std::int64_t>(std::llround(bpm * 1000.0));
}

std::optional<std::int64_t> parse_tag_bpm(std::string_view text) {
    std::string number = to_lower_ascii(trim(text));
    if (number.ends_with("bpm")) number = trim(number.substr(0, number.size() - 3));
    std::ranges::replace(number, ',', '.');
    double value = 0.0;
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), value);
    if (error != std::errc{} || end != number.data() + number.size()) return std::nullopt;
    return milli_bpm_from_value(value);
}

std::optional<MusicalKey> parse_tag_key(std::string_view text) {
    const std::string value = trim(text);
    if (auto camelot = parse_camelot_key(value)) return camelot;
    if (auto open_key = parse_open_key(value)) return open_key;
    const auto key = parse_key(value);
    if (!key || key->mode == KeyMode::Unknown) return std::nullopt;
    return key;
}

std::optional<TagField> field_for_tag_name(std::string_view name) {
    const std::string folded = to_lower_ascii(trim(name));
    if (folded == "title") return TagField::Title;
    if (folded == "artist") return TagField::Artist;
    if (folded == "album") return TagField::Album;
    if (folded == "genre") return TagField::Genre;
    if (folded == "comment" || folded == "comments" || folded == "description") return TagField::Comment;
    if (folded == "bpm" || folded == "tbpm" || folded == "tempo") return TagField::Bpm;
    if (folded == "key" || folded == "tkey" || folded == "initialkey" || folded == "initial key" ||
        folded == "key signature") {
        return TagField::Key;
    }
    if (folded == "keywords") return TagField::Keywords;
    return std::nullopt;
}

void store_tag(EmbeddedTags& tags, TagField field, std::string_view value) {
    const std::string text = trim(value);
    if (text.empty()) return;
    switch (field) {
        case TagField::Title:
            store_text_once(tags.title, text);
            break;
        case TagField::Artist:
            store_text_once(tags.artist, text);
            break;
        case TagField::Album:
            store_text_once(tags.album, text);
            break;
        case TagField::Genre:
            store_text_once(tags.genre, text);
            break;
        case TagField::Comment:
            store_text_once(tags.comment, text);
            break;
        case TagField::Bpm:
            if (!tags.bpm_milli) tags.bpm_milli = parse_tag_bpm(text);
            break;
        case TagField::Key:
            if (!tags.key) tags.key = parse_tag_key(text);
            break;
        case TagField::Keywords:
            add_keywords(tags.keywords, text);
            break;
    }
}

}  // namespace mso::media::detail
