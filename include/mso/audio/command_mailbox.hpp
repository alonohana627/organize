#pragma once
/// @file
/// Bounded lock-free mailbox carrying transport commands from the control thread to the decoder thread.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <readerwriterqueue/readerwriterqueue.h>

#include "mso/audio/transport.hpp"

namespace mso::audio {

/// Single-producer/single-consumer command FIFO built on moodycamel::ReaderWriterQueue
/// (preallocated; only try_enqueue/peek/pop are used, so neither side ever allocates). On top:
///  - Stop never needs queue space: it records its sequence number in an atomic flag, and take()
///    hands it out exactly between the commands posted before and after it;
///  - a seek, volume, speed or pitch command posted right after one of the same kind replaces the
///    pending value instead of queuing again, so scrubbing a seek bar or dragging a slider can
///    never fill the queue and the newest value always wins.
/// The decoder thread receives volumes already converted to linear gain (SetVolumeLinear).
class CommandMailbox {
public:
    /// Queue entries available to commands other than Stop (moodycamel may round this up).
    static constexpr std::size_t kGeneralCapacity = 32;

    /// Preallocates everything. Throws std::bad_alloc only.
    CommandMailbox();
    CommandMailbox(const CommandMailbox&) = delete;
    CommandMailbox& operator=(const CommandMailbox&) = delete;

    /// Producer side. Returns false (and counts a drop) when the command had to be rejected:
    /// the queue is full, or the value is unusable (NaN speed).
    bool post(const TransportCommand& command) noexcept;

    /// Consumer side: oldest pending command with its newest coalesced value, or nullopt when empty.
    std::optional<TransportCommand> take() noexcept;

    /// Commands rejected so far.
    std::uint64_t dropped_count() const noexcept { return dropped_count_.load(std::memory_order_relaxed); }

private:
    enum class EntryKind : std::uint8_t {
        Play,
        Pause,
        TogglePlayPause,
        Stop,
        Seek,
        Volume,
        Speed,
        Pitch,
        PlaybackMode,
        Loop
    };

    /// Queued description of a command. Its value lives in payload_cells_[cell], the one part
    /// either side may still touch after publication (through atomic exchanges only).
    struct Entry {
        EntryKind kind = EntryKind::Play;
        std::uint64_t sequence = 0;
        std::size_t cell = 0;
        std::optional<LoopRegion> loop;
    };

    /// Marks a payload the consumer has already taken; never a valid encoded value.
    static constexpr std::uint64_t kTakenPayload = ~std::uint64_t{0};

    bool push(EntryKind kind, std::uint64_t payload, std::optional<LoopRegion> loop = std::nullopt) noexcept;
    bool post_stop() noexcept;
    bool try_replace_newest_value(EntryKind kind, std::uint64_t payload) noexcept;
    bool reject() noexcept;
    static bool is_coalescing(EntryKind kind) noexcept;
    static TransportCommand decode(const Entry& entry, std::uint64_t payload) noexcept;

    moodycamel::ReaderWriterQueue<Entry> queue_;
    /// Value cells, one more than the queue can hold. moodycamel offers no way to amend a queued
    /// element, which coalescing needs, so values sit beside the queue. A cell is reused only after
    /// its previous entry was popped (the consumer takes the value before popping), because more
    /// than max_capacity() entries can never be in flight at once.
    std::vector<std::atomic<std::uint64_t>> payload_cells_;
    /// Sequence number of the newest Stop (0 = none yet).
    std::atomic<std::uint64_t> stop_sequence_{0};
    std::atomic<std::uint64_t> dropped_count_{0};

    // Producer-only bookkeeping.
    std::uint64_t next_sequence_ = 1;
    std::uint64_t enqueued_count_ = 0;
    std::optional<EntryKind> newest_post_kind_;

    // Consumer-only bookkeeping.
    std::uint64_t delivered_stop_sequence_ = 0;
};

}  // namespace mso::audio
