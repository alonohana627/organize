#pragma once
/// @file
/// ID3 tag parsing (ID3v2.2/2.3/2.4 and ID3v1). ID3v2 tags lead MP3 files and some FLAC/AAC
/// streams, and also appear inside WAV "id3 " and AIFF "ID3 " chunks.
#include <cstddef>
#include <cstdint>
#include <optional>

#include "mso/media/byte_cursor.hpp"
#include "mso/media/media.hpp"

namespace mso::media::detail {

inline constexpr std::size_t kId3v2HeaderSize = 10;
inline constexpr std::size_t kId3v1TagSize = 128;

/// Total size of the ID3v2 tag whose header starts at header[0] (header, body and optional
/// footer); nullopt when the bytes are not a valid ID3v2 header.
std::optional<std::uint64_t> id3v2_tag_size(Bytes header) noexcept;

/// Reads title, artist, album, genre, comment, BPM and key frames from an ID3v2 tag (header
/// included). A tag cut short by the probe budget is parsed as far as it goes.
void parse_id3v2(Bytes tag, EmbeddedTags& tags);

/// Reads a 128-byte ID3v1 tag; false when `tail` is not one.
bool parse_id3v1(Bytes tail, EmbeddedTags& tags);

}  // namespace mso::media::detail
