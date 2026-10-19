#pragma once
/// @file
/// Vorbis comment blocks: the tag format of FLAC, Ogg Vorbis, Ogg Opus, Ogg FLAC and Speex.
///
/// Comment blocks can carry cover art (METADATA_BLOCK_PICTURE, base64) in the same block as the
/// text tags, so the reader pulls the block through a CommentByteSource and skips every entry too
/// large to be a text tag without reading it.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "mso/domain/error.hpp"
#include "mso/media/detail/bounded_reader.hpp"
#include "mso/media/detail/byte_cursor.hpp"
#include "mso/media/media.hpp"

namespace mso::media::detail {

/// Comment entries longer than this are skipped unread (cover art, embedded lyrics files).
inline constexpr std::size_t kMaxCommentEntryBytes = 16 * 1024;

/// Sequential access to the bytes of one Vorbis comment block, wherever they are stored.
class CommentByteSource {
public:
    virtual ~CommentByteSource() = default;
    /// Reads up to `count` bytes; fewer (possibly none) only when the block ends first.
    virtual Result<std::vector<std::uint8_t>> read(std::size_t count) = 0;
    /// Skips up to `count` bytes without reading them; false when the block ended first.
    virtual Result<bool> skip(std::uint64_t count) = 0;
};

/// A comment block that is already in memory.
class MemoryCommentSource final : public CommentByteSource {
public:
    explicit MemoryCommentSource(Bytes block) noexcept : cursor_(block) {}
    Result<std::vector<std::uint8_t>> read(std::size_t count) override;
    Result<bool> skip(std::uint64_t count) override;

private:
    ByteCursor cursor_;
};

/// A comment block stored contiguously in the file (a FLAC VORBIS_COMMENT metadata block).
class FileRangeCommentSource final : public CommentByteSource {
public:
    /// The block occupies `length` bytes at `offset`.
    FileRangeCommentSource(BoundedFileReader& reader, std::uint64_t offset, std::uint64_t length) noexcept
        : reader_(reader), position_(offset), end_(checked_add(offset, length).value_or(~0ull)) {}
    Result<std::vector<std::uint8_t>> read(std::size_t count) override;
    Result<bool> skip(std::uint64_t count) override;

private:
    BoundedFileReader& reader_;
    std::uint64_t position_;
    std::uint64_t end_;
};

/// Reads a Vorbis comment block (vendor string, count, "NAME=value" entries) without any
/// codec-specific packet prefix. Entries longer than kMaxCommentEntryBytes are skipped unread;
/// a truncated block is parsed as far as it goes. Errors: only the source's own (ProbeLimit).
Result<void> read_vorbis_comment(CommentByteSource& source, EmbeddedTags& tags);

/// read_vorbis_comment() over a block already in memory.
void parse_vorbis_comment(Bytes block, EmbeddedTags& tags);

}  // namespace mso::media::detail
