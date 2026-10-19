#pragma once
/// @file
/// Streaming short-time Fourier transform: turns arbitrarily sized blocks of mono samples into a
/// sequence of overlapping magnitude-spectrum frames without ever buffering more than one frame.
#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "mso/analysis/spectrum.hpp"

namespace mso::analysis {

/// Frames mono samples into Hann-windowed frames of `frame_size` advancing by `hop_size`, and
/// hands each frame's magnitude spectrum to a caller-supplied handler.
///
/// The first frame starts at sample 0; a trailing partial frame is never emitted, so the stream
/// is deterministic for a given sample sequence regardless of how it was split into blocks.
class SpectrogramStream {
public:
    /// @param frame_size FFT length in samples (>= 2).
    /// @param hop_size advance between frames in samples (1 .. frame_size).
    SpectrogramStream(std::size_t frame_size, std::size_t hop_size)
        : spectrum_(frame_size), hop_size_(std::clamp<std::size_t>(hop_size, 1, spectrum_.frame_size())) {
        pending_samples_.reserve(2 * spectrum_.frame_size());
    }

    /// Appends samples and calls `on_frame(std::span<const float> magnitudes)` once for every
    /// frame completed by them, in order.
    template <class FrameHandler>
    void add(std::span<const float> samples, FrameHandler&& on_frame) {
        const std::size_t frame_size = spectrum_.frame_size();
        while (!samples.empty()) {
            const std::size_t wanted = frame_size - pending_samples_.size();
            const std::size_t taken = std::min(wanted, samples.size());
            pending_samples_.insert(pending_samples_.end(), samples.begin(),
                                    samples.begin() + static_cast<std::ptrdiff_t>(taken));
            samples = samples.subspan(taken);
            if (pending_samples_.size() < frame_size) break;

            on_frame(spectrum_.compute(pending_samples_));
            pending_samples_.erase(pending_samples_.begin(),
                                   pending_samples_.begin() + static_cast<std::ptrdiff_t>(hop_size_));
        }
    }

    std::size_t frame_size() const noexcept { return spectrum_.frame_size(); }
    std::size_t hop_size() const noexcept { return hop_size_; }
    std::size_t bin_count() const noexcept { return spectrum_.bin_count(); }

private:
    MagnitudeSpectrum spectrum_;
    std::size_t hop_size_;
    std::vector<float> pending_samples_;  ///< at most one frame of not-yet-analysed samples
};

}  // namespace mso::analysis
