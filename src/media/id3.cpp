#include "mso/media/detail/id3.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>
#include <vector>

#include "mso/domain/core.hpp"
#include "mso/media/detail/probe_formats.hpp"
#include "mso/media/detail/tag_mapping.hpp"
#include "mso/media/detail/text_decoding.hpp"

namespace mso::media::detail {

namespace {

// Tag-level header flags.
constexpr std::uint8_t kTagUnsynchronised = 0x80;
constexpr std::uint8_t kTagExtendedHeader = 0x40;  // ID3v2.2: whole-tag compression instead
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
constexpr std::size_t kId3v1CommentOffset = 97;

/// Bytes of a frame header: 6 in ID3v2.2 (3-character id, 24-bit size), 10 later.
constexpr std::size_t frame_header_size(int major_version) noexcept {
    return major_version == 2 ? 6 : 10;
}

/// Characters in a frame identifier: 3 in ID3v2.2, 4 later.
constexpr std::size_t frame_id_size(int major_version) noexcept {
    return major_version == 2 ? 3 : 4;
}

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

/// Text encoding announced by the first byte of a text frame.
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

/// EmbeddedTags field of a plain text frame.
std::optional<TagField> text_frame_field(std::string_view id) noexcept {
    if (id == "TIT2") return TagField::Title;
    if (id == "TPE1") return TagField::Artist;
    if (id == "TALB") return TagField::Album;
    if (id == "TBPM") return TagField::Bpm;
    if (id == "TKEY") return TagField::Key;
    return std::nullopt;
}

/// True for the (canonical) frames the probe stores; every other frame is skipped unread.
bool is_wanted_frame(std::string_view id) noexcept {
    return text_frame_field(id).has_value() || id == "TCON" || id == "COMM" || id == "TXXX";
}

/// Frame identifiers are upper-case letters and digits.
bool is_valid_frame_id(std::string_view id) noexcept {
    return !id.empty() && std::ranges::all_of(id, [](char character) {
        return (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9');
    });
}

/// True when `bytes` (what follows a frame) plausibly starts the next frame or ends the frame list:
/// nothing left, padding, or a valid frame identifier.
bool looks_like_frame_boundary(Bytes bytes, int major_version) noexcept {
    if (bytes.empty() || bytes[0] == 0) return true;
    const std::size_t id_size = frame_id_size(major_version);
    return bytes.size() >= id_size && is_valid_frame_id(as_text(bytes.first(id_size)));
}

/// ID3v2.4 frame sizes are syncsafe, but old iTunes versions wrote plain 32-bit sizes. Like TagLib
/// and mutagen: a size word with a high bit set cannot be syncsafe; otherwise the syncsafe reading
/// wins unless only the plain reading lands on a frame boundary. `boundary_after(size)` reports
/// whether the frame data followed by `size` bytes ends at a plausible boundary.
template <class BoundaryCheck>
std::uint32_t v24_frame_size(std::uint32_t raw_size, BoundaryCheck&& boundary_after) {
    if ((raw_size & 0x80808080u) != 0) return raw_size;
    const std::uint32_t syncsafe_size = decode_syncsafe(raw_size);
    if (syncsafe_size == raw_size || boundary_after(syncsafe_size)) return syncsafe_size;
    return boundary_after(raw_size) ? raw_size : syncsafe_size;
}

/// Resolves ID3 genre references: "(17)", "17", "(17)Indie Rock" (the refinement wins), "(RX)",
/// and the "((" escape for a genre name that really starts with a parenthesis.
std::string resolve_genre(std::string_view text) {
    if (text.starts_with("((")) return std::string(text.substr(1));
    std::string_view reference = text;
    if (text.starts_with('(')) {
        const std::size_t close = text.find(')');
        if (close == std::string_view::npos) return std::string(text);
        const std::string_view refinement = text.substr(close + 1);
        if (!refinement.empty()) return std::string(refinement);
        reference = text.substr(1, close - 1);
    }
    if (reference == "RX") return "Remix";
    if (reference == "CR") return "Cover";
    int number = 0;
    const auto [end, error] = std::from_chars(reference.data(), reference.data() + reference.size(), number);
    if (error == std::errc{} && end == reference.data() + reference.size()) {
        if (const auto name = id3v1_genre_name(number)) return std::string(*name);
        return {};
    }
    return std::string(text);
}

/// Decodes the frames EmbeddedTags cares about (one tag at a time).
class Id3v2FrameHandler {
public:
    Id3v2FrameHandler(int major_version, bool tag_unsynchronised, EmbeddedTags& tags)
        : major_version_(major_version), tag_unsynchronised_(tag_unsynchronised), tags_(tags) {}

    /// Stores one frame given its canonical id, format flags and (possibly cut) data.
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

    /// Walks the frames of a tag body held in memory until padding, a malformed header or the end.
    void read_frames(Bytes body) {
        ByteCursor cursor(body);
        const std::size_t header_size = frame_header_size(major_version_);
        while (cursor.has(header_size)) {
            const std::string id(as_text(cursor.take(frame_id_size(major_version_))));
            if (!is_valid_frame_id(id)) return;  // padding or garbage: no more frames
            std::uint32_t size = 0;
            std::uint16_t flags = 0;
            if (major_version_ == 2) {
                size = cursor.u24_be();
            } else {
                const std::uint32_t raw_size = cursor.u32_be();
                flags = cursor.u16_be();
                size = major_version_ == 4
                           ? v24_frame_size(raw_size,
                                            [&](std::uint32_t candidate) {
                                                if (candidate > cursor.remaining()) return false;
                                                return looks_like_frame_boundary(
                                                    body.subspan(cursor.position() + candidate), major_version_);
                                            })
                           : raw_size;
            }
            if (!cursor.has(size)) return;
            handle_frame(canonical_frame_id(id), flags, cursor.take(size));
        }
    }

private:
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

/// Bytes the extended header occupies, read from its first four bytes; nullopt when invalid.
std::optional<std::uint64_t> extended_header_size(int major_version, std::uint32_t declared) noexcept {
    if (major_version == 3) return std::uint64_t{declared} + 4;  // v2.3 size excludes its own four bytes
    const std::uint32_t size = decode_syncsafe(declared);        // v2.4 size includes them
    if (size < 4) return std::nullopt;
    return size;
}

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
        const auto extended = extended_header_size(major_version, cursor.u32_be());
        if (!extended || *extended < 4) return;
        cursor.skip(*extended - 4);
        if (!cursor.ok()) return;
    }
    Id3v2FrameHandler handler(major_version, (flags & kTagUnsynchronised) != 0, tags);
    handler.read_frames(cursor.take_rest());
}

Result<void> read_id3v2(BoundedFileReader& reader, std::uint64_t offset, std::uint64_t length, EmbeddedTags& tags) {
    if (length < kId3v2HeaderSize) return {};
    auto header = reader.read_within_budget(offset, kId3v2HeaderSize);
    if (!header) return std::unexpected(header.error());
    const auto tag_size = id3v2_tag_size(*header);
    if (!tag_size) return {};
    const int major_version = (*header)[3];
    const std::uint8_t flags = (*header)[5];
    if (major_version == 2 && (flags & kTagExtendedHeader)) return {};  // compressed v2.2 tag
    const std::uint64_t tag_end = std::min({checked_add(offset, *tag_size).value_or(~0ull),
                                            checked_add(offset, length).value_or(~0ull), reader.file_size()});

    if ((flags & kTagUnsynchronised) && major_version < 4) {
        // Frame boundaries only exist after resynchronisation: load the tag in one piece.
        const auto loadable = static_cast<std::size_t>(std::min<std::uint64_t>(tag_end - offset, kMaxTagBytes));
        auto tag = reader.read_within_budget(offset, loadable);
        if (!tag) return std::unexpected(tag.error());
        parse_id3v2(*tag, tags);
        return {};
    }

    std::uint64_t position = offset + kId3v2HeaderSize;
    const std::uint64_t frames_end =
        std::min(tag_end, position + decode_syncsafe(ByteCursor(Bytes(*header).subspan(6, 4)).u32_be()));
    if (flags & kTagExtendedHeader) {
        if (frames_end < position + 4) return {};
        auto declared = reader.read_exact(position, 4);
        if (!declared) return std::unexpected(declared.error());
        const auto extended = extended_header_size(major_version, ByteCursor(*declared).u32_be());
        if (!extended) return {};
        position += *extended;
    }

    Id3v2FrameHandler handler(major_version, (flags & kTagUnsynchronised) != 0, tags);
    const std::size_t header_size = frame_header_size(major_version);
    // True when `size` bytes of frame data starting at `data_offset` end at a plausible boundary.
    Result<void> lookahead_outcome;
    const auto boundary_after = [&](std::uint64_t data_offset, std::uint32_t size) {
        const std::uint64_t next = data_offset + size;
        if (next == frames_end) return true;
        if (next > frames_end) return false;
        auto bytes =
            reader.read_available(next, static_cast<std::size_t>(std::min<std::uint64_t>(4, frames_end - next)));
        if (!bytes) {
            lookahead_outcome = std::unexpected(bytes.error());
            return false;
        }
        return looks_like_frame_boundary(*bytes, major_version);
    };
    for (int count = 0; count < kMaxChunkCount && position < frames_end && frames_end - position >= header_size;
         ++count) {
        auto frame_header = reader.read_exact(position, header_size);
        if (!frame_header) return std::unexpected(frame_header.error());
        ByteCursor cursor(*frame_header);
        const std::string id(as_text(cursor.take(frame_id_size(major_version))));
        if (!is_valid_frame_id(id)) break;  // padding or garbage: no more frames
        const std::uint64_t data_offset = position + header_size;
        std::uint32_t size = 0;
        std::uint16_t frame_flags = 0;
        if (major_version == 2) {
            size = cursor.u24_be();
        } else {
            const std::uint32_t raw_size = cursor.u32_be();
            frame_flags = cursor.u16_be();
            size = major_version == 4
                       ? v24_frame_size(raw_size,
                                        [&](std::uint32_t candidate) { return boundary_after(data_offset, candidate); })
                       : raw_size;
            if (!lookahead_outcome) return lookahead_outcome;
        }
        if (size > frames_end - data_offset) break;  // frame runs past the tag
        const std::string_view canonical = canonical_frame_id(id);
        if (is_wanted_frame(canonical)) {
            auto data = reader.read_exact(data_offset,
                                          static_cast<std::size_t>(std::min<std::uint64_t>(size, kMaxTextItemBytes)));
            if (!data) return std::unexpected(data.error());
            handler.handle_frame(canonical, frame_flags, *data);
        }
        position = data_offset + size;
    }
    return {};
}

bool parse_id3v1(Bytes tail, EmbeddedTags& tags) {
    if (tail.size() != kId3v1TagSize || !has_magic(tail, 0, "TAG")) return false;
    store_tag(tags, TagField::Title, decode_legacy_text(tail.subspan(3, 30)));
    store_tag(tags, TagField::Artist, decode_legacy_text(tail.subspan(33, 30)));
    store_tag(tags, TagField::Album, decode_legacy_text(tail.subspan(63, 30)));
    // ID3v1.1 gives the last two comment bytes to a zero byte and a track number.
    const bool has_track_number = tail[125] == 0 && tail[126] != 0;
    store_tag(tags, TagField::Comment,
              decode_legacy_text(tail.subspan(kId3v1CommentOffset, has_track_number ? 28 : 30)));
    if (const auto genre = id3v1_genre_name(tail[127])) store_tag(tags, TagField::Genre, *genre);
    return true;
}

}  // namespace mso::media::detail
