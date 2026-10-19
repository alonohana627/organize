#include "mso/audio/pcm_ring.hpp"

#include <algorithm>

namespace mso::audio {
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

PcmRing::PcmRing(std::size_t requested_frames)
    : headers_(block_count_for(requested_frames)),
      samples_(headers_.size() * kBlockSampleCount, 0.0f),
      free_blocks_(headers_.size()),
      filled_blocks_(headers_.size()) {
    for (std::uint32_t block = 0; block < headers_.size(); ++block) {
        free_blocks_.try_enqueue(block);  // preallocated capacity: cannot fail
    }
}

std::size_t PcmRing::capacity_bytes() const noexcept {
    return block_capacity() * kBytesPerBlock + kQueueOverheadAllowance;
}

std::optional<WritablePcmBlock> PcmRing::begin_write() noexcept {
    const std::uint32_t* block = free_blocks_.peek();
    if (block == nullptr) {
        return std::nullopt;
    }
    headers_[*block] = PcmBlockHeader{};
    return writable(*block);
}

void PcmRing::commit_write() noexcept {
    const std::uint32_t block = *free_blocks_.peek();
    free_blocks_.pop();
    filled_blocks_.try_enqueue(block);  // capacity covers every block: cannot fail
}

std::optional<ReadablePcmBlock> PcmRing::front() const noexcept {
    const std::uint32_t* block = filled_blocks_.peek();
    if (block == nullptr) {
        return std::nullopt;
    }
    const PcmBlockHeader& header = headers_[*block];
    const std::size_t sample_count = std::min<std::size_t>(header.frame_count, kPcmBlockFrames) * kOutputChannels;
    return ReadablePcmBlock{&header,
                            std::span<const float>(samples_).subspan(*block * kBlockSampleCount, sample_count)};
}

void PcmRing::pop_front() noexcept {
    const std::uint32_t block = *filled_blocks_.peek();
    filled_blocks_.pop();
    free_blocks_.try_enqueue(block);  // capacity covers every block: cannot fail
}

WritablePcmBlock PcmRing::writable(std::uint32_t block) noexcept {
    return WritablePcmBlock{&headers_[block],
                            std::span<float>(samples_).subspan(block * kBlockSampleCount, kBlockSampleCount)};
}

}  // namespace mso::audio
