#pragma once
/// @file
/// Vorbis comment blocks: the tag format of FLAC, Ogg Vorbis and Ogg Opus.
#include "mso/media/byte_cursor.hpp"
#include "mso/media/media.hpp"

namespace mso::media::detail {

/// Parses a Vorbis comment block (vendor string, count, "NAME=value" entries) without any
/// codec-specific packet prefix. Truncated blocks are parsed as far as they go.
void parse_vorbis_comment(Bytes block, EmbeddedTags& tags);

}  // namespace mso::media::detail
