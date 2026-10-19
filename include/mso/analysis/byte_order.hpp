#pragma once
/// @file
/// The one little-endian integer serializer of the analysis and worker modules, used by the PCM
/// hash's canonical stream, the waveform segment format and the worker frame header. Byte order
/// is fixed by those formats, so it is spelled out byte by byte instead of relying on the host's
/// endianness.
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mso::analysis {

/// The `sizeof(T)` little-endian bytes of `value`.
template <std::unsigned_integral T>
constexpr std::array<std::uint8_t, sizeof(T)> little_endian_bytes(T value) noexcept {
    std::array<std::uint8_t, sizeof(T)> bytes{};
    for (std::size_t index = 0; index < sizeof(T); ++index)
        bytes[index] = static_cast<std::uint8_t>(value >> (8 * index));
    return bytes;
}

/// Writes `value` as little-endian bytes into exactly `sizeof(T)` bytes of `destination`.
template <std::unsigned_integral T>
constexpr void store_little_endian(std::span<std::uint8_t, sizeof(T)> destination, T value) noexcept {
    const auto bytes = little_endian_bytes(value);
    for (std::size_t index = 0; index < sizeof(T); ++index)
        destination[index] = bytes[index];
}

/// Appends `value` as little-endian bytes to `bytes`.
template <std::unsigned_integral T>
void append_little_endian(std::vector<std::uint8_t>& bytes, T value) {
    const auto encoded = little_endian_bytes(value);
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
}

/// Reads a little-endian value from exactly `sizeof(T)` bytes.
template <std::unsigned_integral T>
constexpr T load_little_endian(std::span<const std::uint8_t, sizeof(T)> source) noexcept {
    T value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index)
        value = static_cast<T>(value | static_cast<T>(static_cast<T>(source[index]) << (8 * index)));
    return value;
}

}  // namespace mso::analysis
