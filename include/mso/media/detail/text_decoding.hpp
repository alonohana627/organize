#pragma once
/// @file
/// Converts tag text from the encodings containers use into UTF-8.
#include <cstddef>
#include <string>

#include "mso/media/detail/byte_cursor.hpp"

namespace mso::media::detail {

/// Character encodings declared by tag formats (ID3v2 encoding byte, MP4 data type, ...).
enum class TextEncoding {
    Latin1,        ///< ISO-8859-1; UTF-8 is accepted when the bytes are well-formed UTF-8
    Utf16WithBom,  ///< UTF-16 with a byte-order mark (little endian when the mark is missing)
    Utf16BigEndian,
    Utf16LittleEndian,
    Utf8,
};

/// Decodes `bytes` to UTF-8, stopping at the first NUL character. The result is always valid
/// UTF-8: unpaired UTF-16 surrogates become U+FFFD, and Latin-1/UTF-8 input that is not
/// well-formed UTF-8 is read as Latin-1 (tag writers often mislabel the two).
std::string decode_text(Bytes bytes, TextEncoding encoding);

/// Decodes container text with no declared encoding (RIFF INFO, AIFF NAME, ID3v1, CAF info):
/// well-formed UTF-8 is kept, anything else is read as Latin-1.
std::string decode_legacy_text(Bytes bytes);

/// Width in bytes of one code unit, which is also the width of the NUL terminator.
std::size_t code_unit_size(TextEncoding encoding) noexcept;

/// Offset of the first NUL terminator aligned to the encoding's code unit, or bytes.size().
std::size_t find_terminator(Bytes bytes, TextEncoding encoding) noexcept;

}  // namespace mso::media::detail
