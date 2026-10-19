#include "mso/audio/detail/stereo_downmix.hpp"

#include <algorithm>

#include <miniaudio.h>

namespace mso::audio::detail {
namespace {

constexpr float kMinusThreeDecibels = 0.70710678f;
constexpr float kMinusSixDecibels = 0.5f;

/// Left/right contribution of one speaker position before peak normalization.
std::array<float, 2> gains_for_position(ma_channel position) noexcept {
    switch (position) {
        case MA_CHANNEL_MONO:
            return {1.0f, 1.0f};
        case MA_CHANNEL_FRONT_LEFT:
            return {1.0f, 0.0f};
        case MA_CHANNEL_FRONT_RIGHT:
            return {0.0f, 1.0f};
        case MA_CHANNEL_FRONT_CENTER:
            return {kMinusThreeDecibels, kMinusThreeDecibels};
        case MA_CHANNEL_LFE:
            return {0.0f, 0.0f};
        case MA_CHANNEL_FRONT_LEFT_CENTER:
        case MA_CHANNEL_BACK_LEFT:
        case MA_CHANNEL_SIDE_LEFT:
        case MA_CHANNEL_TOP_FRONT_LEFT:
        case MA_CHANNEL_TOP_BACK_LEFT:
            return {kMinusThreeDecibels, 0.0f};
        case MA_CHANNEL_FRONT_RIGHT_CENTER:
        case MA_CHANNEL_BACK_RIGHT:
        case MA_CHANNEL_SIDE_RIGHT:
        case MA_CHANNEL_TOP_FRONT_RIGHT:
        case MA_CHANNEL_TOP_BACK_RIGHT:
            return {0.0f, kMinusThreeDecibels};
        default:
            return {kMinusSixDecibels, kMinusSixDecibels};  // centre-line and auxiliary channels
    }
}

}  // namespace

StereoDownmix::StereoDownmix(std::size_t channel_count) : gains_(std::max<std::size_t>(channel_count, 1)) {
    if (gains_.size() == 1) {
        gains_[0] = {1.0f, 1.0f};
    } else if (gains_.size() == 2) {
        gains_[0] = {1.0f, 0.0f};
        gains_[1] = {0.0f, 1.0f};
    } else {
        std::vector<ma_channel> positions(gains_.size());
        ma_channel_map_init_standard(ma_standard_channel_map_microsoft, positions.data(), positions.size(),
                                     static_cast<ma_uint32>(positions.size()));
        std::array<float, 2> row_sums{0.0f, 0.0f};
        for (std::size_t channel = 0; channel < gains_.size(); ++channel) {
            gains_[channel] = gains_for_position(positions[channel]);
            row_sums[0] += gains_[channel][0];
            row_sums[1] += gains_[channel][1];
        }
        const float largest_sum = std::max(row_sums[0], row_sums[1]);
        if (largest_sum > 1.0f) {
            for (auto& gains : gains_) {
                gains[0] /= largest_sum;
                gains[1] /= largest_sum;
            }
        }
    }
    row_pointers_.reserve(gains_.size());
    for (auto& gains : gains_) {
        row_pointers_.push_back(gains.data());
    }
}

}  // namespace mso::audio::detail
