#pragma once
/// @file
/// Bounded lock-free mailbox carrying transport commands from the control thread to the decoder thread.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "mso/audio/transport.hpp"

namespace mso::audio::detail {

/// Single-producer/single-consumer command FIFO built on moodycamel::ReaderWriterQueue
/// (preallocated; only try_enqueue/peek/pop are used, so neither side ever allocates). On top:
///  - Stop never needs queue space: it records its sequence number in an atomic, and take() hands
///    it out exactly between the commands posted before and after it;
///  - a seek, volume, speed or pitch command posted right after one of the same kind replaces the
///    pending value instead of queuing again, so scrubbing a seek bar or dragging a slider can
///    never fill the queue and the newest value always wins.
/// The decoder thread receives volumes already converted to linear gain (SetVolumeLinear).
///
/// The Stop side channel and the coalescing are written here (a recorded exception to owner rule 1):
/// moodycamel's queues, like every vendored queue, cannot amend or reorder an element once it is
/// queued, and the transport needs both "the newest seek/volume/speed/pitch wins" and "Stop is never
/// refused" without the producer ever blocking or allocating.
class CommandMailbox {
public:
    /// Queue entries available to commands other than Stop (moodycamel may round this up).
    static constexpr std::size_t kGeneralCapacity = 32;

    /// Preallocates everything. Throws std::bad_alloc only.
    CommandMailbox();
    ~CommandMailbox();
    CommandMailbox(const CommandMailbox&) = delete;
    CommandMailbox& operator=(const CommandMailbox&) = delete;

    /// Producer side. Returns false (and counts a drop) when the command had to be rejected:
    /// the queue is full, or the value is unusable (NaN speed).
    [[nodiscard]] bool post(const TransportCommand& command) noexcept;

    /// Consumer side: oldest pending command with its newest coalesced value, or nullopt when empty.
    [[nodiscard]] std::optional<TransportCommand> take() noexcept;

    /// Commands rejected so far.
    std::uint64_t dropped_count() const noexcept { return dropped_count_.load(std::memory_order_relaxed); }

private:
    /// What a queue entry stands for (Stop never enters the queue but is tracked for coalescing).
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
        SeamFades,
        Loop
    };

    /// Queued description of a command. Its value lives in payload_cells_[cell], the one part
    /// either side may still touch after publication (through atomic exchanges only).
    struct Entry {
        EntryKind kind = EntryKind::Play;  ///< command kind
        std::uint64_t sequence = 0;        ///< post order, shared with Stop's sequence numbers
        std::size_t cell = 0;              ///< index into payload_cells_
        std::optional<LoopRegion> loop;    ///< SetLoop's region (immutable once queued)
    };

    struct EntryQueue;

    /// Marks a payload the consumer has already taken; never a valid encoded value.
    static constexpr std::uint64_t kTakenPayload = ~std::uint64_t{0};

    /// Queues (or coalesces) one non-Stop command.
    bool push(EntryKind kind, std::uint64_t payload, std::optional<LoopRegion> loop = std::nullopt) noexcept;
    /// Records a Stop; always succeeds.
    bool post_stop() noexcept;
    /// Swaps `payload` into the newest entry when it is of `kind` and not yet taken.
    bool try_replace_newest_value(EntryKind kind, std::uint64_t payload) noexcept;
    /// Counts a rejected command and returns false.
    bool reject() noexcept;
    /// Kinds whose newest value supersedes a pending one.
    static bool is_coalescing(EntryKind kind) noexcept;
    /// Rebuilds the command an entry and its payload stand for.
    static TransportCommand decode(const Entry& entry, std::uint64_t payload) noexcept;

    std::unique_ptr<EntryQueue> queue_;  ///< moodycamel queue of entries (behind a pointer: PRIVATE dependency)
    /// Value cells, one more than the queue can hold. moodycamel offers no way to amend a queued
    /// element, which coalescing needs, so values sit beside the queue. A cell is reused only after
    /// its previous entry was popped (the consumer takes the value before popping), because more
    /// than the queue's capacity can never be in flight at once.
    std::vector<std::atomic<std::uint64_t>> payload_cells_;
    std::atomic<std::uint64_t> stop_sequence_{0};  ///< sequence number of the newest Stop (0 = none yet)
    std::atomic<std::uint64_t> dropped_count_{0};  ///< commands rejected so far

    // Producer-only bookkeeping.
    std::uint64_t next_sequence_ = 1;            ///< sequence number of the next post
    std::uint64_t enqueued_count_ = 0;           ///< entries ever queued (selects the next cell)
    std::optional<EntryKind> newest_post_kind_;  ///< kind of the most recent post, Stop included

    // Consumer-only bookkeeping.
    std::uint64_t delivered_stop_sequence_ = 0;  ///< newest Stop already handed out
};

}  // namespace mso::audio::detail
