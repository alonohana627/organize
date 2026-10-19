#pragma once
/// @file
/// Channel-layout fold of any source to stereo, expressed as a gain matrix for miniaudio's converter.
#include <array>
#include <cstddef>
#include <vector>

namespace mso::audio {

/// Gains from each source channel to the left and right outputs. Decoders report only a channel
/// count, so channels are assumed in the standard WAVE/Microsoft order (FL FR FC LFE BL BR SL SR ...).
///  - mono feeds both sides at unity; stereo passes through;
///  - wider layouts use the ITU-R BS.775 style coefficients (centre and surrounds at -3 dB, LFE
///    dropped), then both rows are scaled by one common factor so each output's gains sum to at most
///    1: a full-scale input can never clip after the fold (peak-safe) and the image stays balanced.
class StereoDownmix {
public:
    /// Fold for `channel_count` source channels (1 or more).
    explicit StereoDownmix(std::size_t channel_count);

    // Row pointers refer into gains_; moving keeps the vector's buffer, copying would not.
    StereoDownmix(const StereoDownmix&) = delete;
    StereoDownmix& operator=(const StereoDownmix&) = delete;
    StereoDownmix(StereoDownmix&&) noexcept = default;
    StereoDownmix& operator=(StereoDownmix&&) noexcept = default;

    std::size_t input_channels() const noexcept { return gains_.size(); }
    /// Gain from source channel `input_channel` to output 0 (left) or 1 (right).
    float gain(std::size_t input_channel, std::size_t output_channel) const noexcept {
        return gains_[input_channel][output_channel];
    }

    /// Row pointers in miniaudio's float** [in][out] layout; valid while this object lives.
    float** weight_rows() noexcept { return row_pointers_.data(); }

private:
    std::vector<std::array<float, 2>> gains_;
    std::vector<float*> row_pointers_;
};

}  // namespace mso::audio
