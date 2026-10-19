#pragma once
/// @file
/// APEv1/APEv2 tags: the usual tag format of WavPack and Monkey's Audio, and common at the end of
/// MP3 files written by foobar2000 and MP3Gain.
#include <cstddef>
#include <cstdint>
#include <optional>

#include "mso/domain/error.hpp"
#include "mso/media/detail/bounded_reader.hpp"
#include "mso/media/media.hpp"

namespace mso::media::detail {

/// Size of an APE tag header or footer.
inline constexpr std::size_t kApeTagFooterSize = 32;

/// Reads the APE tag whose footer ends at `end_offset` (the end of the file, or the start of a
/// trailing ID3v1 tag). Text items with a known name are stored; binary items (cover art) and
/// oversized values are skipped unread. Returns the offset where the tag starts (its header
/// included), or nullopt when no APE tag ends at `end_offset`.
/// Errors: only the reader's (ProbeLimit).
[[nodiscard]] Result<std::optional<std::uint64_t>> read_ape_tag(BoundedFileReader& reader, std::uint64_t end_offset,
                                                                EmbeddedTags& tags);

}  // namespace mso::media::detail
