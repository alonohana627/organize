#pragma once
/// @file
/// Streaming short-time Fourier transform: turns arbitrarily sized blocks of mono samples into a
/// sequence of overlapping magnitude-spectrum frames without ever buffering more than one frame.
#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include "mso/analysis/spectrum.hpp"

namespace mso::analysis {

/// Frames mono samples into Hann-windowed frames of `frame_size` advancing by `hop_size`, and
/// hands each frame's magnitude spectrum to a caller-supplied handler.
///
/// The first frame starts at sample 0; a trailing partial frame is never emitted, so the stream
/// is deterministic for a given sample sequence regardless of how it was split into blocks.
/// Samples live in a ring buffer of exactly one frame: advancing by a hop overwrites the oldest
/// hop of samples in place, and the frame is handed to the FFT as its two wrapped halves, so no
/// sample is ever moved or copied twice.
class SpectrogramStream {
public:
    /// @param frame_size FFT length in samples (values below 2 are raised to 2).
    /// @param hop_size advance between frames in samples, clamped to [1, frame_size].
    SpectrogramStream(std::size_t frame_size, std::size_t hop_size)
        : spectrum_(frame_size),
          hop_size_(std::clamp<std::size_t>(hop_size, 1, spectrum_.frame_size())),
          ring_(spectrum_.frame_size(), 0.0F),
          samples_until_frame_(spectrum_.frame_size()) {}

    /// Appends samples and calls `on_frame(std::span<const float> magnitudes)` once for every
    /// frame completed by them, in order.
    template <class FrameHandler>
    void add(std::span<const float> samples, FrameHandler&& on_frame) {
        const std::size_t capacity = ring_.size();
        while (!samples.empty()) {
            const std::size_t taken = std::min(samples_until_frame_, samples.size());
            // Copy into the ring, wrapping at its end; after the copy write_position_ is the oldest sample.
            const std::size_t first_part = std::min(taken, capacity - write_position_);
            std::copy_n(samples.begin(), first_part, ring_.begin() + static_cast<std::ptrdiff_t>(write_position_));
            std::copy_n(samples.begin() + static_cast<std::ptrdiff_t>(first_part), taken - first_part, ring_.begin());
            write_position_ = (write_position_ + taken) % capacity;
            samples_until_frame_ -= taken;
            samples = samples.subspan(taken);
            if (samples_until_frame_ > 0) break;

            const std::span<const float> ring(ring_);
            on_frame(spectrum_.compute(ring.subspan(write_position_), ring.first(write_position_)));
            samples_until_frame_ = hop_size_;
        }
    }

    /// Samples per frame.
    std::size_t frame_size() const noexcept { return spectrum_.frame_size(); }
    /// Samples between the starts of consecutive frames.
    std::size_t hop_size() const noexcept { return hop_size_; }
    /// Magnitudes per frame.
    std::size_t bin_count() const noexcept { return spectrum_.bin_count(); }

private:
    MagnitudeSpectrum spectrum_;
    std::size_t hop_size_;
    std::vector<float> ring_;              ///< the most recent frame_size() samples, oldest at write_position_
    std::size_t write_position_ = 0;       ///< where the next sample goes
    std::size_t samples_until_frame_ = 0;  ///< samples still needed to complete the next frame
};

}  // namespace mso::analysis
