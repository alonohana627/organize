#include "mso/media/detail/text_decoding.hpp"

#include <utf8proc.h>

#include "mso/domain/core.hpp"

namespace mso::media::detail {

namespace {

constexpr char32_t kReplacementCharacter = 0xFFFD;

/// Appends one Unicode scalar value as UTF-8 (deps/utf8proc). Surrogates and out-of-range values
/// become U+FFFD.
void append_utf8(std::string& output, char32_t code_point) {
    auto scalar = static_cast<utf8proc_int32_t>(code_point);
    if (!utf8proc_codepoint_valid(scalar)) scalar = static_cast<utf8proc_int32_t>(kReplacementCharacter);
    utf8proc_uint8_t encoded[4];
    const auto length = utf8proc_encode_char(scalar, encoded);
    output.append(reinterpret_cast<const char*>(encoded), static_cast<std::size_t>(length));
}

/// Decodes ISO-8859-1 bytes (each byte is its own code point).
std::string decode_latin1(Bytes bytes) {
    std::string output;
    output.reserve(bytes.size());
    for (const std::uint8_t byte : bytes)
        append_utf8(output, byte);
    return output;
}

/// Decodes UTF-16 code units, pairing surrogates; a lone surrogate becomes U+FFFD.
std::string decode_utf16(Bytes bytes, bool big_endian) {
    const auto unit_at = [&](std::size_t index) -> char32_t {
        const char32_t first = bytes[2 * index];
        const char32_t second = bytes[2 * index + 1];
        return big_endian ? (first << 8) | second : (second << 8) | first;
    };
    std::string output;
    output.reserve(bytes.size());
    const std::size_t unit_count = bytes.size() / 2;
    for (std::size_t index = 0; index < unit_count; ++index) {
        const char32_t unit = unit_at(index);
        if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < unit_count) {
            const char32_t low = unit_at(index + 1);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                append_utf8(output, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                ++index;
                continue;
            }
        }
        append_utf8(output, unit);
    }
    return output;
}

}  // namespace

std::size_t code_unit_size(TextEncoding encoding) noexcept {
    switch (encoding) {
        case TextEncoding::Utf16WithBom:
        case TextEncoding::Utf16BigEndian:
        case TextEncoding::Utf16LittleEndian:
            return 2;
        case TextEncoding::Latin1:
        case TextEncoding::Utf8:
            return 1;
    }
    return 1;
}

std::size_t find_terminator(Bytes bytes, TextEncoding encoding) noexcept {
    const std::size_t unit = code_unit_size(encoding);
    for (std::size_t offset = 0; offset + unit <= bytes.size(); offset += unit) {
        if (bytes[offset] == 0 && (unit == 1 || bytes[offset + 1] == 0)) return offset;
    }
    return bytes.size();
}

std::string decode_legacy_text(Bytes bytes) {
    const Bytes text = bytes.first(find_terminator(bytes, TextEncoding::Utf8));
    if (is_valid_utf8(as_text(text))) return std::string(as_text(text));
    return decode_latin1(text);
}

std::string decode_text(Bytes bytes, TextEncoding encoding) {
    const Bytes text = bytes.first(find_terminator(bytes, encoding));
    switch (encoding) {
        case TextEncoding::Latin1:
        case TextEncoding::Utf8:
            return decode_legacy_text(text);
        case TextEncoding::Utf16BigEndian:
            return decode_utf16(text, true);
        case TextEncoding::Utf16LittleEndian:
            return decode_utf16(text, false);
        case TextEncoding::Utf16WithBom:
            if (text.size() >= 2 && text[0] == 0xFE && text[1] == 0xFF) return decode_utf16(text.subspan(2), true);
            if (text.size() >= 2 && text[0] == 0xFF && text[1] == 0xFE) return decode_utf16(text.subspan(2), false);
            return decode_utf16(text, false);
    }
    return {};
}

}  // namespace mso::media::detail
