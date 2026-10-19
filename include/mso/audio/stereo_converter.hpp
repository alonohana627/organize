#pragma once
/// @file
/// Channel fold and sample-rate conversion to the device format, delegated to miniaudio's
/// ma_data_converter. Internal to mso_audio (exposes miniaudio types).
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <miniaudio.h>

#include "mso/domain/error.hpp"

namespace mso::audio {

/// Frames consumed from the input and produced into the output by one conversion call.
struct ConversionProgress {
    std::size_t input_frames = 0;
    std::size_t output_frames = 0;
};

/// Converts interleaved float32 frames of any channel count to interleaved stereo float32,
/// resampling by an exact integer ratio (source rate × speed : output rate). With a 1:1 ratio and a
/// stereo source the conversion is a bit-exact pass-through.
class StereoConverter {
public:
    /// Converter for `input_channels` channels and the reduced rate ratio
    /// `source_units : output_units` (see SourceClock). Fails with RESOURCE_LIMIT/INVALID_ARGUMENT.
    static Result<std::unique_ptr<StereoConverter>> create(std::size_t input_channels, std::int64_t source_units,
                                                           std::int64_t output_units);

    ~StereoConverter();
    StereoConverter(const StereoConverter&) = delete;
    StereoConverter& operator=(const StereoConverter&) = delete;

    /// Converts as much of `input` (interleaved, input_channels wide) as fits into `stereo_output`.
    ConversionProgress process(std::span<const float> input, std::span<float> stereo_output) noexcept;

private:
    StereoConverter() = default;

    std::size_t input_channels_ = 0;
    ma_data_converter converter_{};  ///< holds pointers into itself: the object never moves
};

}  // namespace mso::audio
