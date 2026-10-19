#include "mso/media/vorbis_comment.hpp"

#include <string_view>

#include "mso/media/tag_mapping.hpp"
#include "mso/media/text_decoding.hpp"

namespace mso::media::detail {

void parse_vorbis_comment(Bytes block, EmbeddedTags& tags) {
    ByteCursor cursor(block);
    cursor.skip(cursor.u32_le());  // vendor string
    const std::uint32_t comment_count = cursor.u32_le();
    for (std::uint32_t index = 0; index < comment_count && cursor.has(4); ++index) {
        const std::uint32_t length = cursor.u32_le();
        const Bytes entry = cursor.take(length);
        if (!cursor.ok()) return;
        const std::string_view text = as_text(entry);
        const std::size_t separator = text.find('=');
        if (separator == std::string_view::npos) continue;
        if (const auto field = field_for_tag_name(text.substr(0, separator))) {
            store_tag(tags, *field, decode_text(entry.subspan(separator + 1), TextEncoding::Utf8));
        }
    }
}

}  // namespace mso::media::detail
