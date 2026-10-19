#include "mso/audio/detail/pcm_ring.hpp"

#include <algorithm>
#include <cassert>

#include <readerwriterqueue/readerwriterqueue.h>

namespace mso::audio::detail {
namespace {

constexpr std::size_t kBlockSampleCount = kPcmBlockFrames * kOutputChannels;
/// Samples, header, and one index slot in each of the two queues, doubled for moodycamel's
/// power-of-two rounding.
constexpr std::size_t kBytesPerBlock =
    kBlockSampleCount * sizeof(float) + sizeof(PcmBlockHeader) + 2 * 2 * sizeof(std::uint32_t);
/// Room left under the cap for moodycamel's fixed per-queue bookkeeping.
constexpr std::size_t kQueueOverheadAllowance = 64 * 1024;
constexpr std::size_t kMaximumBlocks = (kPcmRingMaximumBytes - kQueueOverheadAllowance) / kBytesPerBlock;

static_assert(kMaximumBlocks >= 2, "the ring cap must hold at least two blocks");

std::size_t block_count_for(std::size_t requested_frames) noexcept {
    const std::size_t wanted = (requested_frames + kPcmBlockFrames - 1) / kPcmBlockFrames;
    return std::clamp<std::size_t>(wanted, 2, kMaximumBlocks);
}

}  // namespace

/// The two index queues. Both are sized for every block, so try_enqueue can never fail while the
/// pool's invariant holds (each index is in exactly one queue or held by one side).
struct PcmRing::BlockQueues {
    explicit BlockQueues(std::size_t blocks) : free_blocks(blocks), filled_blocks(blocks) {}

    moodycamel::ReaderWriterQueue<std::uint32_t> free_blocks;    ///< callback → decoder thread
    moodycamel::ReaderWriterQueue<std::uint32_t> filled_blocks;  ///< decoder thread → callback
};

PcmRing::PcmRing(std::size_t requested_frames)
    : headers_(block_count_for(requested_frames)),
      samples_(headers_.size() * kBlockSampleCount, 0.0f),
      queues_(std::make_unique<BlockQueues>(headers_.size())) {
    for (std::uint32_t block = 0; block < headers_.size(); ++block) {
        [[maybe_unused]] const bool queued = queues_->free_blocks.try_enqueue(block);
        assert(queued && "the free queue is preallocated for every block");
    }
}

PcmRing::~PcmRing() = default;

std::size_t PcmRing::capacity_bytes() const noexcept {
    return block_capacity() * kBytesPerBlock + kQueueOverheadAllowance;
}

std::size_t PcmRing::maximum_block_count() noexcept {
    return kMaximumBlocks;
}

std::optional<WritablePcmBlock> PcmRing::begin_write() noexcept {
    const std::uint32_t* block = queues_->free_blocks.peek();
    if (block == nullptr) {
        return std::nullopt;
    }
    headers_[*block] = PcmBlockHeader{};
    return writable(*block);
}

void PcmRing::commit_write() noexcept {
    const std::uint32_t block = *queues_->free_blocks.peek();
    queues_->free_blocks.pop();
    [[maybe_unused]] const bool queued = queues_->filled_blocks.try_enqueue(block);
    assert(queued && "the filled queue is preallocated for every block");
}

std::optional<ReadablePcmBlock> PcmRing::front() const noexcept {
    const std::uint32_t* block = queues_->filled_blocks.peek();
    if (block == nullptr) {
        return std::nullopt;
    }
    const PcmBlockHeader& header = headers_[*block];
    const std::size_t sample_count = std::min<std::size_t>(header.frame_count, kPcmBlockFrames) * kOutputChannels;
    return ReadablePcmBlock{&header,
                            std::span<const float>(samples_).subspan(*block * kBlockSampleCount, sample_count)};
}

void PcmRing::pop_front() noexcept {
    const std::uint32_t block = *queues_->filled_blocks.peek();
    queues_->filled_blocks.pop();
    [[maybe_unused]] const bool queued = queues_->free_blocks.try_enqueue(block);
    assert(queued && "the free queue is preallocated for every block");
}

std::size_t PcmRing::readable_blocks() const noexcept {
    return queues_->filled_blocks.size_approx();
}

WritablePcmBlock PcmRing::writable(std::uint32_t block) noexcept {
    return WritablePcmBlock{&headers_[block],
                            std::span<float>(samples_).subspan(block * kBlockSampleCount, kBlockSampleCount)};
}

}  // namespace mso::audio::detail
