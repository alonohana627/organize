#pragma once
/// @file
/// Lock-free publication of a small trivially copyable value from one writer to many readers.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>

namespace mso::audio::detail {

/// Sequence lock over a trivially copyable `Value`. The single writer never waits (two counter
/// stores around the copy), so the real-time callback may publish through it; readers retry until
/// they see the same even version before and after copying, yielding while a write is in progress.
/// The value is stored as atomic 64-bit words, so concurrent access is free of data races
/// (Boehm, "Can seqlocks get along with programming language memory models?", 2012).
/// No vendored library provides a seqlock (a deliberate exception to owner rule 1, recorded in
/// IMPLEMENTATION_STATUS.md); it is kept to the minimum the engine needs.
template <class Value>
class Seqlock {
    static_assert(std::is_trivially_copyable_v<Value>, "Seqlock copies values bytewise");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "the version counter must never lock");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "the value words must never lock");

public:
    Seqlock() noexcept { store(Value{}); }
    explicit Seqlock(const Value& initial) noexcept { store(initial); }
    Seqlock(const Seqlock&) = delete;
    Seqlock& operator=(const Seqlock&) = delete;

    /// Publishes `value`. Wait-free; callers must not write concurrently (one writer at a time).
    void store(const Value& value) noexcept {
        std::array<std::uint64_t, kWords> raw{};
        std::memcpy(raw.data(), &value, sizeof(Value));
        const std::uint32_t version = version_.load(std::memory_order_relaxed);
        version_.store(version + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t word = 0; word < kWords; ++word) {
            words_[word].store(raw[word], std::memory_order_relaxed);
        }
        version_.store(version + 2, std::memory_order_release);
    }

    /// A consistent copy of the most recently published value.
    [[nodiscard]] Value load() const noexcept {
        std::array<std::uint64_t, kWords> raw{};
        for (;;) {
            const std::uint32_t version_before = version_.load(std::memory_order_acquire);
            if ((version_before & 1u) == 0) {
                for (std::size_t word = 0; word < kWords; ++word) {
                    raw[word] = words_[word].load(std::memory_order_relaxed);
                }
                std::atomic_thread_fence(std::memory_order_acquire);
                if (version_.load(std::memory_order_relaxed) == version_before) {
                    break;
                }
            }
            std::this_thread::yield();  // a write is in progress; it finishes in a few nanoseconds
        }
        Value value;
        std::memcpy(&value, raw.data(), sizeof(Value));
        return value;
    }

private:
    static constexpr std::size_t kWords = (sizeof(Value) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);

    std::atomic<std::uint32_t> version_{0};                   ///< odd while a store is in progress
    std::array<std::atomic<std::uint64_t>, kWords> words_{};  ///< the value's bytes
};

}  // namespace mso::audio::detail
