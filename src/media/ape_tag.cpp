// APEv1/APEv2 tag reader (footer at the end of the file, items walked with small reads).
#include "mso/media/detail/ape_tag.hpp"

#include <algorithm>
#include <string_view>

#include "mso/media/detail/byte_cursor.hpp"
#include "mso/media/detail/probe_formats.hpp"
#include "mso/media/detail/tag_mapping.hpp"
#include "mso/media/detail/text_decoding.hpp"

namespace mso::media::detail {

namespace {

constexpr std::uint32_t kApeVersion2 = 2000;
constexpr std::uint32_t kHasHeaderFlag = 0x80000000u;
constexpr std::uint32_t kItemTypeShift = 1;
constexpr std::uint32_t kItemTypeMask = 0x3;
constexpr std::uint32_t kItemTypeText = 0;
constexpr std::size_t kItemHeaderSize = 8;  // value size, item flags
constexpr std::size_t kMaxKeyBytes = 255;

}  // namespace

Result<std::optional<std::uint64_t>> read_ape_tag(BoundedFileReader& reader, std::uint64_t end_offset,
                                                  EmbeddedTags& tags) {
    if (end_offset < kApeTagFooterSize || end_offset > reader.file_size()) return std::optional<std::uint64_t>{};
    auto footer = reader.read_within_budget(end_offset - kApeTagFooterSize, kApeTagFooterSize);
    if (!footer) return std::unexpected(footer.error());
    if (footer->size() < kApeTagFooterSize || !has_magic(*footer, 0, "APETAGEX")) {
        return std::optional<std::uint64_t>{};
    }
    ByteCursor cursor(Bytes(*footer).subspan(8));
    const std::uint32_t version = cursor.u32_le();
    const std::uint32_t tag_size = cursor.u32_le();  // items plus footer, header excluded
    const std::uint32_t item_count = cursor.u32_le();
    const std::uint32_t flags = cursor.u32_le();
    if (tag_size < kApeTagFooterSize || tag_size > end_offset) return std::optional<std::uint64_t>{};
    const std::uint64_t items_start = end_offset - tag_size;
    const std::uint64_t items_end = end_offset - kApeTagFooterSize;
    const bool has_header = version >= kApeVersion2 && (flags & kHasHeaderFlag) != 0;
    const std::uint64_t tag_start =
        has_header && items_start >= kApeTagFooterSize ? items_start - kApeTagFooterSize : items_start;

    std::uint64_t position = items_start;
    const std::uint32_t items = std::min<std::uint32_t>(item_count, kMaxChunkCount);
    for (std::uint32_t index = 0; index < items && items_end - position > kItemHeaderSize; ++index) {
        const auto window =
            static_cast<std::size_t>(std::min<std::uint64_t>(kItemHeaderSize + kMaxKeyBytes + 1, items_end - position));
        auto head = reader.read_exact(position, window);
        if (!head) return std::unexpected(head.error());
        ByteCursor item(*head);
        const std::uint32_t value_size = item.u32_le();
        const std::uint32_t item_flags = item.u32_le();
        const std::string_view rest = as_text(Bytes(*head).subspan(kItemHeaderSize));
        const std::size_t key_end = rest.find('\0');
        if (key_end == std::string_view::npos || key_end == 0) break;  // malformed item: stop
        const std::uint64_t value_offset = position + kItemHeaderSize + key_end + 1;
        if (value_size > items_end - value_offset) break;
        const bool is_text =
            version < kApeVersion2 || ((item_flags >> kItemTypeShift) & kItemTypeMask) == kItemTypeText;
        const auto field = field_for_tag_name(rest.substr(0, key_end));
        if (is_text && field && value_size <= kMaxTextItemBytes) {
            auto value = reader.read_exact(value_offset, value_size);
            if (!value) return std::unexpected(value.error());
            store_tag(tags, *field, decode_text(*value, TextEncoding::Utf8));  // multiple values: the first
        }
        position = value_offset + value_size;
    }
    return std::optional<std::uint64_t>{tag_start};
}

}  // namespace mso::media::detail
