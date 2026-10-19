#pragma once
/// @file
/// Preallocated pool of stereo float blocks tagged with PCM epochs, passed between the decoder
/// thread (only producer) and the audio callback (only consumer).
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "mso/audio/detail/source_position.hpp"

namespace mso::audio::detail {

/// The ring always carries interleaved stereo; every source is folded to two channels first.
inline constexpr std::size_t kOutputChannels = 2;
/// Frames per ring block: ~5 ms at 48 kHz, the granularity of epoch switches and position updates.
inline constexpr std::uint32_t kPcmBlockFrames = 256;
/// Hard memory cap of the ring, headers and queues included (docs/02_ARCHITECTURE.md §8).
inline constexpr std::size_t kPcmRingMaximumBytes = 8u * 1024u * 1024u;

/// Describes one block: which epoch and device rate produced it and which source frames it covers.
struct PcmBlockHeader {
    /// PCM epoch the producer wrote this block for. The consumer drops blocks of older epochs, so
    /// audio decoded before an open/seek/stop can never be heard after it.
    std::uint64_t epoch = 0;
    /// Device rate the block was rendered for; the consumer drops blocks of another rate (they
    /// would play at the wrong speed after a device switch, until the producer re-anchors).
    std::uint32_t output_rate = 0;
    std::uint32_t frame_count = 0;  ///< valid frames, at most kPcmBlockFrames
    /// Last block of the source: once consumed, the transport reports the end of playback.
    bool end_of_stream = false;
    /// Output-to-source mapping of the segment this block belongs to.
    SourceClock clock;
    /// The block's first frame, counted in output frames since the segment's anchor.
    std::int64_t first_output_frame = 0;
    /// Loop folding in effect for these positions.
    LoopFold loop;
    /// Positions never exceed this (the exact source length, when known): resampler and stretcher
    /// rounding must not report a frame past the end.
    std::int64_t last_source_frame = std::numeric_limits<std::int64_t>::max();

    /// Source position after `frames_played` frames of this block, exact (integer clock, no drift).
    std::int64_t source_frame_after(std::uint32_t frames_played) const noexcept {
        const std::int64_t unfolded = clock.source_frame_after(first_output_frame + frames_played);
        return loop.fold(std::min(unfolded, last_source_frame));
    }
};

/// Writable view of a block (producer side). `samples` holds kPcmBlockFrames stereo frames.
struct WritablePcmBlock {
    PcmBlockHeader* header = nullptr;  ///< header to fill
    std::span<float> samples;          ///< interleaved stereo storage of the block
};

/// Read-only view of a published block (consumer side); `samples` covers header->frame_count frames.
struct ReadablePcmBlock {
    const PcmBlockHeader* header = nullptr;  ///< header as published
    std::span<const float> samples;          ///< interleaved stereo frames
};

/// Fixed pool of blocks circulating through two preallocated moodycamel::ReaderWriterQueue of block
/// indices: `filled` carries written blocks to the callback in production order, `free` returns
/// played blocks to the decoder thread. Each queue has one producer and one consumer, holds every
/// index at once, and is used only through try_enqueue/peek/pop, so no operation allocates or blocks.
/// The queues live behind a pointer so this header does not expose the PRIVATE moodycamel dependency.
class PcmRing {
public:
    /// Preallocates room for at least `requested_frames` frames (whole blocks), capped so the ring
    /// never exceeds kPcmRingMaximumBytes. Throws std::bad_alloc only.
    explicit PcmRing(std::size_t requested_frames);
    ~PcmRing();

    PcmRing(const PcmRing&) = delete;
    PcmRing& operator=(const PcmRing&) = delete;

    /// Number of blocks in the pool.
    std::size_t block_capacity() const noexcept { return headers_.size(); }
    /// Frames the pool can hold.
    std::size_t frame_capacity() const noexcept { return block_capacity() * kPcmBlockFrames; }
    /// Upper bound of the bytes the ring owns: samples, headers and queue storage (always within
    /// kPcmRingMaximumBytes).
    std::size_t capacity_bytes() const noexcept;
    /// Largest number of blocks any ring can have under kPcmRingMaximumBytes.
    static std::size_t maximum_block_count() noexcept;

    // ---- producer (decoder thread)

    /// Free block to fill (the same one until committed), or nullopt when every block holds unread
    /// audio. The header is reset to defaults.
    std::optional<WritablePcmBlock> begin_write() noexcept;
    /// Publishes the block returned by begin_write() to the consumer. Requires a successful begin_write().
    void commit_write() noexcept;

    // ---- consumer (audio callback)

    /// Oldest published block, or nullopt when the ring is empty.
    std::optional<ReadablePcmBlock> front() const noexcept;
    /// Releases the block returned by front() back to the producer. Requires a non-empty front().
    void pop_front() noexcept;

    /// Published blocks not yet released by the consumer (approximate while either side runs).
    std::size_t readable_blocks() const noexcept;

private:
    struct BlockQueues;

    /// View of block `block` for the producer.
    WritablePcmBlock writable(std::uint32_t block) noexcept;

    std::vector<PcmBlockHeader> headers_;  ///< one header per block
    std::vector<float> samples_;           ///< kPcmBlockFrames stereo frames per block, contiguous
    std::unique_ptr<BlockQueues> queues_;  ///< the free and filled index queues
};

}  // namespace mso::audio::detail
