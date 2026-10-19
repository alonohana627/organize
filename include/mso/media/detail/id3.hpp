#pragma once
/// @file
/// ID3 tag parsing (ID3v2.2/2.3/2.4 and ID3v1). ID3v2 tags lead MP3 files and some FLAC/AAC
/// streams, and also appear inside WAV "id3 " and AIFF "ID3 " chunks.
#include <cstddef>
#include <cstdint>
#include <optional>

#include "mso/domain/error.hpp"
#include "mso/media/detail/bounded_reader.hpp"
#include "mso/media/detail/byte_cursor.hpp"
#include "mso/media/media.hpp"

namespace mso::media::detail {

/// Size of the ID3v2 header (and of the optional v2.4 footer).
inline constexpr std::size_t kId3v2HeaderSize = 10;
/// Size of an ID3v1 tag at the end of a file.
inline constexpr std::size_t kId3v1TagSize = 128;

/// Total size of the ID3v2 tag whose header starts at header[0] (header, body and optional
/// footer); nullopt when the bytes are not a valid ID3v2 header.
std::optional<std::uint64_t> id3v2_tag_size(Bytes header) noexcept;

/// Reads title, artist, album, genre, comment, BPM and key frames from an ID3v2 tag held in
/// memory (header included). A tag cut short is parsed as far as it goes.
void parse_id3v2(Bytes tag, EmbeddedTags& tags);

/// Reads the ID3v2 tag that starts at `offset` in the file and occupies at most `length` bytes,
/// walking frame headers with small reads and loading only the text frames the probe stores
/// (pictures, GEOB, PRIV and other binary frames are skipped unread). A tag unsynchronised as a
/// whole (before v2.4) is loaded in one piece, up to kMaxTagBytes. Nothing is stored when the
/// bytes at `offset` are not an ID3v2 header. Errors: only the reader's (ProbeLimit).
Result<void> read_id3v2(BoundedFileReader& reader, std::uint64_t offset, std::uint64_t length, EmbeddedTags& tags);

/// Reads a 128-byte ID3v1 or ID3v1.1 tag; false when `tail` is not one.
bool parse_id3v1(Bytes tail, EmbeddedTags& tags);

}  // namespace mso::media::detail
