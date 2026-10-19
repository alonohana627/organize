#pragma once
/// @file
/// Maps decoded tag values from every container into EmbeddedTags, so BPM, key and keyword rules
/// are identical whether a value came from ID3, a Vorbis comment, RIFF INFO, MP4 ilst or CAF info.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "mso/media/media.hpp"

namespace mso::media::detail {

/// The EmbeddedTags slot a container field feeds.
enum class TagField { Title, Artist, Album, Genre, Comment, Bpm, Key, Keywords };

/// Stores one UTF-8 tag value. The first non-empty value of a field wins (a file's primary tag
/// block is parsed first); keywords accumulate, split on ';' and ','. Values that do not parse
/// (a BPM of "fast", a key of "8X") are ignored rather than stored half-understood.
void store_tag(EmbeddedTags& tags, TagField field, std::string_view value);

/// Field for a free-form tag name such as a Vorbis comment key, an ID3 TXXX description, an MP4
/// "----" name or a CAF info key. Case-insensitive; nullopt for names not worth indexing.
std::optional<TagField> field_for_tag_name(std::string_view name);

/// Converts a tempo in BPM to milli-BPM; nullopt outside 1..999 BPM or when not finite.
[[nodiscard]] std::optional<std::int64_t> milli_bpm_from_value(double bpm) noexcept;

/// Parses a tag tempo such as "128", "127.5", "128,00" or "126 BPM" into milli-BPM (1..999 BPM).
[[nodiscard]] std::optional<std::int64_t> parse_tag_bpm(std::string_view text);

/// Parses a tag key ("F#m", "Bbmin", "C major", Camelot "8A", Open Key "1m"); nullopt when the
/// mode is unknown.
std::optional<MusicalKey> parse_tag_key(std::string_view text);

/// Genre name for an ID3v1 genre number (0..191, including the Winamp extensions); nullopt for
/// anything else.
std::optional<std::string_view> id3v1_genre_name(int genre_number) noexcept;

}  // namespace mso::media::detail
