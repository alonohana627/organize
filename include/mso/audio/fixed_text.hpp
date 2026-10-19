#pragma once
/// @file
/// Fixed-capacity UTF-8 text that copies without allocating, so it can travel inside noexcept
/// snapshots, plus a seqlock cell that publishes such text between threads without locks.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mso::audio {

/// Immutable-size text buffer of at most `Capacity` UTF-8 bytes. Longer input is truncated at a
/// code-point boundary, never in the middle of a multi-byte sequence.
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

    std::string_view view() const noexcept { return {bytes_.data(), size_}; }
    bool empty() const noexcept { return size_ == 0; }

private:
    static bool is_continuation_byte(char byte) noexcept { return (static_cast<unsigned char>(byte) & 0xC0u) == 0x80u; }

    std::array<char, Capacity> bytes_{};
    std::size_t size_ = 0;
};

/// Lock-free cell holding a FixedText that one writer updates rarely and any thread may read.
/// Classic seqlock: the version is odd while a write is in progress and readers retry until they
/// observe the same even version before and after copying. Every byte is an atomic, so concurrent
/// access is free of data races; there is never more than one writer at a time.
template <std::size_t Capacity>
class AtomicText {
public:
    /// Publishes new text. Callers must serialize writes (single writer).
    void store(std::string_view text) noexcept {
        const FixedText<Capacity> value = FixedText<Capacity>::truncated_from(text);
        const std::string_view bytes = value.view();
        const std::uint32_t version = version_.load(std::memory_order_relaxed);
        version_.store(version + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        size_.store(bytes.size(), std::memory_order_relaxed);
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes_[index].store(bytes[index], std::memory_order_relaxed);
        }
        version_.store(version + 2, std::memory_order_release);
    }

    /// Returns a consistent copy of the most recently published text.
    FixedText<Capacity> load() const noexcept {
        std::array<char, Capacity> copy{};
        for (;;) {
            const std::uint32_t version_before = version_.load(std::memory_order_acquire);
            if ((version_before & 1u) != 0) {
                continue;  // a write is in progress
            }
            const std::size_t size = size_.load(std::memory_order_relaxed);
            for (std::size_t index = 0; index < size && index < Capacity; ++index) {
                copy[index] = bytes_[index].load(std::memory_order_relaxed);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (version_.load(std::memory_order_relaxed) == version_before) {
                return FixedText<Capacity>::truncated_from(std::string_view(copy.data(), size));
            }
        }
    }

private:
    std::atomic<std::uint32_t> version_{0};
    std::atomic<std::size_t> size_{0};
    std::array<std::atomic<char>, Capacity> bytes_{};
};

}  // namespace mso::audio
