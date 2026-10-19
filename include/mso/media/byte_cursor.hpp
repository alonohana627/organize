#pragma once
/// @file
/// Bounds-checked integer decoding over in-memory byte ranges, shared by every container parser.
/// Nothing here can read outside the span it was given: a short read yields zero and marks the
/// cursor as failed, so a parser checks ok() once after a group of reads instead of after each one.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mso::media::detail {

/// Read-only view of bytes loaded from a file.
using Bytes = std::span<const std::uint8_t>;

/// Adds two unsigned 64-bit values; nullopt when the sum would wrap.
constexpr std::optional<std::uint64_t> checked_add(std::uint64_t left, std::uint64_t right) noexcept {
    std::uint64_t sum = 0;
    if (__builtin_add_overflow(left, right, &sum)) return std::nullopt;
    return sum;
}

/// Multiplies two unsigned 64-bit values; nullopt when the product would wrap.
constexpr std::optional<std::uint64_t> checked_multiply(std::uint64_t left, std::uint64_t right) noexcept {
    std::uint64_t product = 0;
    if (__builtin_mul_overflow(left, right, &product)) return std::nullopt;
    return product;
}

/// Views bytes as characters, for comparing magic numbers and identifiers.
inline std::string_view as_text(Bytes bytes) noexcept {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// True when `bytes` holds `magic` at `offset` (false when the range is too short).
inline bool has_magic(Bytes bytes, std::size_t offset, std::string_view magic) noexcept {
    return offset <= bytes.size() && bytes.size() - offset >= magic.size() &&
           as_text(bytes.subspan(offset, magic.size())) == magic;
}

/// Decodes a 28-bit ID3v2 "syncsafe" integer (7 significant bits per byte).
constexpr std::uint32_t decode_syncsafe(std::uint32_t encoded) noexcept {
    return ((encoded >> 3) & 0x0FE00000u) | ((encoded >> 2) & 0x001FC000u) | ((encoded >> 1) & 0x00003F80u) |
           (encoded & 0x0000007Fu);
}

/// Sequential reader over a byte span with a sticky failure flag.
class ByteCursor {
public:
    explicit ByteCursor(Bytes bytes) noexcept : bytes_(bytes) {}

    std::size_t position() const noexcept { return position_; }
    std::size_t remaining() const noexcept { return bytes_.size() - position_; }
    /// False once any read or skip ran past the end of the span.
    bool ok() const noexcept { return ok_; }
    /// True when `count` more bytes can be read without failing.
    bool has(std::uint64_t count) const noexcept { return ok_ && remaining() >= count; }

    /// Advances by `count` bytes; fails (without moving) when fewer remain.
    void skip(std::uint64_t count) noexcept {
        if (has(count)) {
            position_ += static_cast<std::size_t>(count);
        } else {
            ok_ = false;
        }
    }

    /// Returns the next `count` bytes and advances; an empty span and failure when fewer remain.
    Bytes take(std::uint64_t count) noexcept {
        if (!has(count)) {
            ok_ = false;
            return {};
        }
        const Bytes taken = bytes_.subspan(position_, static_cast<std::size_t>(count));
        position_ += static_cast<std::size_t>(count);
        return taken;
    }

    /// Everything not yet consumed; the cursor ends up at the end.
    Bytes take_rest() noexcept { return take(remaining()); }

    /// The next four bytes as text (chunk and box identifiers).
    std::string fourcc() { return std::string(as_text(take(4))); }

    std::uint8_t u8() noexcept { return static_cast<std::uint8_t>(unsigned_value<1, false>()); }
    std::uint16_t u16_le() noexcept { return static_cast<std::uint16_t>(unsigned_value<2, false>()); }
    std::uint16_t u16_be() noexcept { return static_cast<std::uint16_t>(unsigned_value<2, true>()); }
    std::uint32_t u24_be() noexcept { return static_cast<std::uint32_t>(unsigned_value<3, true>()); }
    std::uint32_t u32_le() noexcept { return static_cast<std::uint32_t>(unsigned_value<4, false>()); }
    std::uint32_t u32_be() noexcept { return static_cast<std::uint32_t>(unsigned_value<4, true>()); }
    std::uint64_t u64_le() noexcept { return unsigned_value<8, false>(); }
    std::uint64_t u64_be() noexcept { return unsigned_value<8, true>(); }

private:
    template <std::size_t Width, bool BigEndian>
    std::uint64_t unsigned_value() noexcept {
        if (!has(Width)) {
            ok_ = false;
            return 0;
        }
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < Width; ++index) {
            const std::uint64_t byte = bytes_[position_ + index];
            value |= BigEndian ? byte << (8 * (Width - 1 - index)) : byte << (8 * index);
        }
        position_ += Width;
        return value;
    }

    Bytes bytes_;
    std::size_t position_ = 0;
    bool ok_ = true;
};

}  // namespace mso::media::detail
