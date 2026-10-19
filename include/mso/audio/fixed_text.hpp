#pragma once
/// @file
/// Fixed-capacity UTF-8 text that copies without allocating, so it can travel inside noexcept snapshots.
#include <array>
#include <cstddef>
#include <string_view>

namespace mso::audio {

/// Text buffer of at most `Capacity` UTF-8 bytes. Longer input is truncated at a code-point
/// boundary, never in the middle of a multi-byte sequence. Trivially copyable, so it can be
/// published between threads by detail::Seqlock.
template <std::size_t Capacity>
class FixedText {
public:
    FixedText() = default;

    /// Copies `text`, keeping the longest prefix of whole code points that fits in `Capacity` bytes.
    static FixedText truncated_from(std::string_view text) noexcept {
        std::size_t length = text.size();
        if (length > Capacity) {
            length = Capacity;
            // text[length] is the first byte left out; if it continues a sequence, drop that sequence.
            while (length > 0 && is_continuation_byte(text[length])) {
                --length;
            }
        }
        FixedText result;
        for (std::size_t index = 0; index < length; ++index) {
            result.bytes_[index] = text[index];
        }
        result.size_ = length;
        return result;
    }

    /// The stored text.
    std::string_view view() const noexcept { return {bytes_.data(), size_}; }
    /// True when no text is stored.
    bool empty() const noexcept { return size_ == 0; }

private:
    /// True for the 10xxxxxx bytes that continue a multi-byte UTF-8 sequence.
    static bool is_continuation_byte(char byte) noexcept { return (static_cast<unsigned char>(byte) & 0xC0u) == 0x80u; }

    std::array<char, Capacity> bytes_{};  ///< UTF-8 bytes; only the first size_ are meaningful
    std::size_t size_ = 0;                ///< bytes in use
};

}  // namespace mso::audio
