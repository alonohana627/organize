#include "mso/media/detail/vorbis_comment.hpp"

#include <algorithm>
#include <optional>
#include <string_view>

#include "mso/media/detail/tag_mapping.hpp"
#include "mso/media/detail/text_decoding.hpp"

namespace mso::media::detail {

namespace {

/// Entries examined per block, so a hostile count cannot spin the probe.
constexpr std::uint32_t kMaxCommentEntries = 4096;

/// Reads a little-endian 32-bit length; nullopt when the block ends first.
Result<std::optional<std::uint32_t>> read_length(CommentByteSource& source) {
    auto bytes = source.read(4);
    if (!bytes) return std::unexpected(bytes.error());
    if (bytes->size() < 4) return std::optional<std::uint32_t>{};
    ByteCursor cursor(*bytes);
    return std::optional<std::uint32_t>{cursor.u32_le()};
}

/// Stores one "NAME=value" entry when its name maps to an EmbeddedTags field.
void store_entry(Bytes entry, EmbeddedTags& tags) {
    const std::string_view text = as_text(entry);
    const std::size_t separator = text.find('=');
    if (separator == std::string_view::npos) return;
    if (const auto field = field_for_tag_name(text.substr(0, separator))) {
        store_tag(tags, *field, decode_text(entry.subspan(separator + 1), TextEncoding::Utf8));
    }
}

}  // namespace

Result<std::vector<std::uint8_t>> MemoryCommentSource::read(std::size_t count) {
    const Bytes taken = cursor_.take(std::min<std::uint64_t>(count, cursor_.remaining()));
    return std::vector<std::uint8_t>(taken.begin(), taken.end());
}

Result<bool> MemoryCommentSource::skip(std::uint64_t count) {
    if (count > cursor_.remaining()) {
        cursor_.skip(cursor_.remaining());
        return false;
    }
    cursor_.skip(count);
    return true;
}

Result<std::vector<std::uint8_t>> FileRangeCommentSource::read(std::size_t count) {
    const std::uint64_t available = end_ > position_ ? end_ - position_ : 0;
    const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(count, available));
    auto bytes = reader_.read_available(position_, wanted);
    if (!bytes) return std::unexpected(bytes.error());
    position_ += bytes->size();
    return bytes;
}

Result<bool> FileRangeCommentSource::skip(std::uint64_t count) {
    const std::uint64_t available = end_ > position_ ? end_ - position_ : 0;
    const bool complete = count <= available;
    position_ += std::min(count, available);
    return complete;
}

Result<void> read_vorbis_comment(CommentByteSource& source, EmbeddedTags& tags) {
    auto vendor_length = read_length(source);
    if (!vendor_length) return std::unexpected(vendor_length.error());
    if (!*vendor_length) return {};
    auto vendor_skipped = source.skip(**vendor_length);
    if (!vendor_skipped) return std::unexpected(vendor_skipped.error());
    if (!*vendor_skipped) return {};
    auto comment_count = read_length(source);
    if (!comment_count) return std::unexpected(comment_count.error());
    if (!*comment_count) return {};
    const std::uint32_t entries = std::min(**comment_count, kMaxCommentEntries);
    for (std::uint32_t index = 0; index < entries; ++index) {
        auto length = read_length(source);
        if (!length) return std::unexpected(length.error());
        if (!*length) return {};
        if (**length > kMaxCommentEntryBytes) {
            auto skipped = source.skip(**length);  // cover art and other binary payloads
            if (!skipped) return std::unexpected(skipped.error());
            if (!*skipped) return {};
            continue;
        }
        auto entry = source.read(**length);
        if (!entry) return std::unexpected(entry.error());
        if (entry->size() < **length) return {};  // truncated block: the partial entry is dropped
        store_entry(*entry, tags);
    }
    return {};
}

void parse_vorbis_comment(Bytes block, EmbeddedTags& tags) {
    MemoryCommentSource source(block);
    // A memory source cannot fail; the Result only carries errors of file-backed sources.
    const Result<void> parsed = read_vorbis_comment(source, tags);
    static_cast<void>(parsed);
}

}  // namespace mso::media::detail
