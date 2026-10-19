#pragma once
/// @file
/// Preallocated pool of stereo float blocks tagged with PCM epochs, passed between the decoder
/// thread (only producer) and the audio callback (only consumer) through moodycamel SPSC queues.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <readerwriterqueue/readerwriterqueue.h>

#include "mso/audio/source_position.hpp"

namespace mso::audio {

/// The ring always carries interleaved stereo; every source is folded to two channels first.
inline constexpr std::size_t kOutputChannels = 2;
/// Frames per ring block: ~5 ms at 48 kHz, the granularity of epoch switches and position updates.
inline constexpr std::uint32_t kPcmBlockFrames = 256;
/// Hard memory cap of the ring, headers included (docs/02_ARCHITECTURE.md §8).
inline constexpr std::size_t kPcmRingMaximumBytes = 8u * 1024u * 1024u;

/// Describes one block: which epoch produced it and which source frames it covers.
struct PcmBlockHeader {
    /// PCM epoch the producer wrote this block for. The consumer drops blocks of older epochs, so
    /// audio decoded before an open/seek/stop can never be heard after it.
    std::uint64_t epoch = 0;
    std::uint32_t frame_count = 0;  ///< valid frames, at most kPcmBlockFrames
    /// Last block of the source: once consumed, the transport reports the end of playback.
    bool end_of_stream = false;
    /// Output-to-source mapping of the segment this block belongs to, and the block's first frame
    /// counted in output frames since that segment's anchor.
    SourceClock clock;
    std::int64_t first_output_frame = 0;
    /// Loop folding in effect for these positions.
    LoopFold loop;

    /// Source position after `frames_played` frames of this block, exact (integer clock, no drift).
    std::int64_t source_frame_after(std::uint32_t frames_played) const noexcept {
        return loop.fold(clock.source_frame_after(first_output_frame + frames_played));
    }
};

/// Writable view of a block slot (producer side). `samples` holds kPcmBlockFrames stereo frames.
struct WritablePcmBlock {
    PcmBlockHeader* header = nullptr;
    std::span<float> samples;
};

/// Read-only view of a published block (consumer side); `samples` covers header->frame_count frames.
struct ReadablePcmBlock {
    const PcmBlockHeader* header = nullptr;
    std::span<const float> samples;
};

/// Fixed pool of blocks circulating through two preallocated moodycamel::ReaderWriterQueue<index>:
/// `filled` carries written blocks to the callback in production order, `free` returns played blocks
/// to the decoder thread. Each queue has one producer and one consumer, both can hold every block
/// index at once, and only try_enqueue/peek/pop are used, so no operation allocates or blocks.
class PcmRing {
public:
    /// Preallocates room for at least `requested_frames` frames (whole blocks), capped so the ring
    /// never exceeds kPcmRingMaximumBytes. Throws std::bad_alloc only.
    explicit PcmRing(std::size_t requested_frames);

    PcmRing(const PcmRing&) = delete;
    PcmRing& operator=(const PcmRing&) = delete;

    std::size_t block_capacity() const noexcept { return headers_.size(); }
    std::size_t frame_capacity() const noexcept { return block_capacity() * kPcmBlockFrames; }
    /// Upper bound of the bytes the ring owns: samples, headers and queue slots (always within
    /// kPcmRingMaximumBytes).
    std::size_t capacity_bytes() const noexcept;

    // ---- producer (decoder thread)

    /// Free block to fill (the same one until committed), or nullopt when every block holds unread audio.
    std::optional<WritablePcmBlock> begin_write() noexcept;
    /// Publishes the block returned by begin_write() to the consumer. Requires a successful begin_write().
    void commit_write() noexcept;

    // ---- consumer (audio callback)

    /// Oldest published block, or nullopt when the ring is empty.
    std::optional<ReadablePcmBlock> front() const noexcept;
    /// Releases the block returned by front() back to the producer. Requires a non-empty front().
    void pop_front() noexcept;

    /// Published blocks not yet released by the consumer (approximate while either side runs).
    std::size_t readable_blocks() const noexcept { return filled_blocks_.size_approx(); }

private:
    WritablePcmBlock writable(std::uint32_t block) noexcept;

    std::vector<PcmBlockHeader> headers_;
    std::vector<float> samples_;
    moodycamel::ReaderWriterQueue<std::uint32_t> free_blocks_;    ///< callback → decoder thread
    moodycamel::ReaderWriterQueue<std::uint32_t> filled_blocks_;  ///< decoder thread → callback
};

}  // namespace mso::audio
