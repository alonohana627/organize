#include "mso/media/id3.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>
#include <vector>

#include "mso/domain/core.hpp"
#include "mso/media/tag_mapping.hpp"
#include "mso/media/text_decoding.hpp"

namespace mso::media::detail {

namespace {

// Tag-level header flags.
constexpr std::uint8_t kTagUnsynchronised = 0x80;
constexpr std::uint8_t kTagExtendedHeader = 0x40;
constexpr std::uint8_t kTagFooter = 0x10;
// ID3v2.3 frame format flags.
constexpr std::uint16_t kV23Compressed = 0x0080;
constexpr std::uint16_t kV23Encrypted = 0x0040;
constexpr std::uint16_t kV23Grouped = 0x0020;
// ID3v2.4 frame format flags.
constexpr std::uint16_t kV24Grouped = 0x0040;
constexpr std::uint16_t kV24Compressed = 0x0008;
constexpr std::uint16_t kV24Encrypted = 0x0004;
constexpr std::uint16_t kV24Unsynchronised = 0x0002;
constexpr std::uint16_t kV24DataLengthIndicator = 0x0001;

/// Reverses ID3 unsynchronisation: every 0xFF 0x00 pair becomes 0xFF.
std::vector<std::uint8_t> remove_unsynchronisation(Bytes bytes) {
    std::vector<std::uint8_t> restored;
    restored.reserve(bytes.size());
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        restored.push_back(bytes[index]);
        if (bytes[index] == 0xFF && index + 1 < bytes.size() && bytes[index + 1] == 0x00) ++index;
    }
    return restored;
}

std::optional<TextEncoding> id3_text_encoding(std::uint8_t marker) noexcept {
    switch (marker) {
        case 0:
            return TextEncoding::Latin1;
        case 1:
            return TextEncoding::Utf16WithBom;
        case 2:
            return TextEncoding::Utf16BigEndian;
        case 3:
            return TextEncoding::Utf8;
        default:
            return std::nullopt;
    }
}

/// Translates ID3v2.2 three-character frame identifiers to their ID3v2.3 names.
std::string_view canonical_frame_id(std::string_view id) noexcept {
    static constexpr std::pair<std::string_view, std::string_view> kV22Names[] = {
        {"TT2", "TIT2"}, {"TP1", "TPE1"}, {"TAL", "TALB"}, {"TCO", "TCON"},
        {"TBP", "TBPM"}, {"TKE", "TKEY"}, {"COM", "COMM"}, {"TXX", "TXXX"},
    };
    for (const auto& [old_name, new_name] : kV22Names) {
        if (id == old_name) return new_name;
    }
    return id;
}

std::optional<TagField> text_frame_field(std::string_view id) noexcept {
    if (id == "TIT2") return TagField::Title;
    if (id == "TPE1") return TagField::Artist;
    if (id == "TALB") return TagField::Album;
    if (id == "TBPM") return TagField::Bpm;
    if (id == "TKEY") return TagField::Key;
    return std::nullopt;
}

std::optional<int> parse_genre_number(std::string_view text) noexcept {
    int number = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return number;
}

/// Resolves ID3 genre references: "(17)", "17", "(17)Indie Rock" (the refinement wins), "(RX)".
std::string resolve_genre(std::string_view text) {
    std::string_view reference = text;
    if (text.starts_with('(') && !text.starts_with("((")) {
        const std::size_t close = text.find(')');
        if (close == std::string_view::npos) return std::string(text);
        const std::string_view refinement = text.substr(close + 1);
        if (!refinement.empty()) return std::string(refinement);
        reference = text.substr(1, close - 1);
    }
    if (reference == "RX") return "Remix";
    if (reference == "CR") return "Cover";
    if (const auto number = parse_genre_number(reference)) {
        if (const auto name = id3v1_genre_name(*number)) return std::string(*name);
        return {};
    }
    return std::string(text);
}

/// Walks the frames of one ID3v2 tag body and stores the ones EmbeddedTags cares about.
class Id3v2FrameReader {
public:
    Id3v2FrameReader(int major_version, bool tag_unsynchronised, EmbeddedTags& tags)
        : major_version_(major_version), tag_unsynchronised_(tag_unsynchronised), tags_(tags) {}

    /// Reads frames until padding, a malformed header or the end of the (possibly cut) body.
    void read_frames(Bytes body) {
        ByteCursor cursor(body);
        const std::size_t header_size = major_version_ == 2 ? 6 : 10;
        while (cursor.has(header_size)) {
            const std::string id(as_text(cursor.take(major_version_ == 2 ? 3 : 4)));
            if (!is_valid_frame_id(id)) return;  // padding or garbage: no more frames
            std::uint32_t size = 0;
            std::uint16_t flags = 0;
            if (major_version_ == 2) {
                size = cursor.u24_be();
            } else {
                size = major_version_ == 4 ? decode_syncsafe(cursor.u32_be()) : cursor.u32_be();
                flags = cursor.u16_be();
            }
            if (!cursor.has(size)) return;
            handle_frame(canonical_frame_id(id), flags, cursor.take(size));
        }
    }

private:
    static bool is_valid_frame_id(std::string_view id) noexcept {
        return std::ranges::all_of(id, [](char character) {
            return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9');
        });
    }

    /// Strips frame-level grouping, length and unsynchronisation; nullopt for compressed or
    /// encrypted frames, which import does not unpack.
    std::optional<Bytes> frame_payload(std::uint16_t flags, Bytes data) {
        ByteCursor cursor(data);
        bool unsynchronised = false;
        if (major_version_ == 3) {
            if (flags & (kV23Compressed | kV23Encrypted)) return std::nullopt;
            if (flags & kV23Grouped) cursor.skip(1);
        } else if (major_version_ == 4) {
            if (flags & (kV24Compressed | kV24Encrypted)) return std::nullopt;
            if (flags & kV24Grouped) cursor.skip(1);
            if (flags & kV24DataLengthIndicator) cursor.skip(4);
            unsynchronised = tag_unsynchronised_ || (flags & kV24Unsynchronised);
        }
        const Bytes payload = cursor.take_rest();
        if (!cursor.ok()) return std::nullopt;
        if (!unsynchronised) return payload;
        resynchronised_ = remove_unsynchronisation(payload);
        return Bytes(resynchronised_);
    }

    void handle_frame(std::string_view id, std::uint16_t flags, Bytes data) {
        const auto payload = frame_payload(flags, data);
        if (!payload || payload->empty()) return;
        const auto encoding = id3_text_encoding((*payload)[0]);
        if (!encoding) return;
        const Bytes content = payload->subspan(1);
        if (const auto field = text_frame_field(id)) {
            store_tag(tags_, *field, decode_text(content, *encoding));
        } else if (id == "TCON") {
            store_tag(tags_, TagField::Genre, resolve_genre(trim(decode_text(content, *encoding))));
        } else if (id == "COMM") {
            handle_comment(content, *encoding);
        } else if (id == "TXXX") {
            handle_user_text(content, *encoding);
        }
    }

    /// COMM: 3-byte language, short description, text. iTunes stores machine data in comments
    /// whose description starts with "iTun"; those are not user comments.
    void handle_comment(Bytes content, TextEncoding encoding) {
        if (content.size() < 3) return;
        const Bytes described = content.subspan(3);
        const std::size_t description_end = find_terminator(described, encoding);
        const std::string description = decode_text(described.first(description_end), encoding);
        if (description.starts_with("iTun")) return;
        const std::size_t text_start = std::min(described.size(), description_end + code_unit_size(encoding));
        store_tag(tags_, TagField::Comment, decode_text(described.subspan(text_start), encoding));
    }

    /// TXXX: user-defined text such as "INITIALKEY" or "BPM" written by DJ tools.
    void handle_user_text(Bytes content, TextEncoding encoding) {
        const std::size_t description_end = find_terminator(content, encoding);
        const auto field = field_for_tag_name(decode_text(content.first(description_end), encoding));
        if (!field) return;
        const std::size_t value_start = std::min(content.size(), description_end + code_unit_size(encoding));
        store_tag(tags_, *field, decode_text(content.subspan(value_start), encoding));
    }

    int major_version_;
    bool tag_unsynchronised_;
    EmbeddedTags& tags_;
    std::vector<std::uint8_t> resynchronised_;
};

}  // namespace

std::optional<std::uint64_t> id3v2_tag_size(Bytes header) noexcept {
    if (header.size() < kId3v2HeaderSize || !has_magic(header, 0, "ID3")) return std::nullopt;
    const std::uint8_t major_version = header[3];
    if (major_version < 2 || major_version > 4 || header[4] == 0xFF) return std::nullopt;
    for (std::size_t index = 6; index < kId3v2HeaderSize; ++index) {
        if (header[index] & 0x80) return std::nullopt;  // size bytes are syncsafe
    }
    ByteCursor cursor(header.subspan(6, 4));
    const std::uint64_t body_size = decode_syncsafe(cursor.u32_be());
    const bool has_footer = major_version == 4 && (header[5] & kTagFooter);
    return kId3v2HeaderSize + body_size + (has_footer ? kId3v2HeaderSize : 0);
}

void parse_id3v2(Bytes tag, EmbeddedTags& tags) {
    if (!id3v2_tag_size(tag)) return;
    const int major_version = tag[3];
    const std::uint8_t flags = tag[5];
    ByteCursor size_cursor(tag.subspan(6, 4));
    const std::uint64_t declared_body = decode_syncsafe(size_cursor.u32_be());
    Bytes body = tag.subspan(kId3v2HeaderSize);
    body = body.first(static_cast<std::size_t>(std::min<std::uint64_t>(declared_body, body.size())));

    // Before v2.4 unsynchronisation applies to the whole tag body; v2.4 flags it per frame.
    std::vector<std::uint8_t> restored;
    if ((flags & kTagUnsynchronised) && major_version < 4) {
        restored = remove_unsynchronisation(body);
        body = restored;
    }

    ByteCursor cursor(body);
    if (flags & kTagExtendedHeader) {
        if (major_version == 2) return;  // v2.2 used this bit for whole-tag compression
        const std::uint32_t declared = cursor.u32_be();
        if (major_version == 3) {
            cursor.skip(declared);  // v2.3 size excludes its own four bytes
        } else {
            const std::uint32_t extended_size = decode_syncsafe(declared);
            if (extended_size < 4) return;
            cursor.skip(extended_size - 4);  // v2.4 size includes them
        }
        if (!cursor.ok()) return;
    }
    Id3v2FrameReader reader(major_version, (flags & kTagUnsynchronised) != 0, tags);
    reader.read_frames(cursor.take_rest());
}

bool parse_id3v1(Bytes tail, EmbeddedTags& tags) {
    if (tail.size() != kId3v1TagSize || !has_magic(tail, 0, "TAG")) return false;
    store_tag(tags, TagField::Title, decode_legacy_text(tail.subspan(3, 30)));
    store_tag(tags, TagField::Artist, decode_legacy_text(tail.subspan(33, 30)));
    store_tag(tags, TagField::Album, decode_legacy_text(tail.subspan(63, 30)));
    store_tag(tags, TagField::Comment, decode_legacy_text(tail.subspan(97, 28)));
    if (const auto genre = id3v1_genre_name(tail[127])) store_tag(tags, TagField::Genre, *genre);
    return true;
}

}  // namespace mso::media::detail
