#include "mso/audio/detail/stereo_converter.hpp"

#include <limits>
#include <new>
#include <string>

#include <miniaudio.h>

#include "mso/audio/detail/pcm_ring.hpp"
#include "mso/audio/detail/stereo_downmix.hpp"

namespace mso::audio::detail {

/// miniaudio's converter state. It holds pointers into itself, so it lives at a fixed heap address.
struct StereoConverter::Engine {
    ma_data_converter converter{};  ///< valid once `initialized`
    bool initialized = false;       ///< ma_data_converter_init succeeded

    Engine() = default;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine() {
        if (initialized) {
            ma_data_converter_uninit(&converter, nullptr);
        }
    }
};

StereoConverter::StereoConverter() = default;

StereoConverter::~StereoConverter() = default;

Result<std::unique_ptr<StereoConverter>> StereoConverter::create(std::size_t input_channels, std::int64_t source_units,
                                                                 std::int64_t output_units) {
    if (input_channels == 0 || input_channels > MA_MAX_CHANNELS) {
        return fail(ErrorCode::UnsupportedCodec, "unsupported channel count " + std::to_string(input_channels));
    }
    constexpr std::int64_t kLargestRate = std::numeric_limits<ma_uint32>::max();
    if (source_units <= 0 || output_units <= 0 || source_units > kLargestRate || output_units > kLargestRate) {
        return fail(ErrorCode::InvalidArgument, "resampling ratio out of range");
    }

    std::unique_ptr<StereoConverter> converter(new (std::nothrow) StereoConverter());
    if (!converter) {
        return fail(ErrorCode::ResourceLimit, "out of memory for the preview converter");
    }
    converter->engine_.reset(new (std::nothrow) Engine());
    if (!converter->engine_) {
        return fail(ErrorCode::ResourceLimit, "out of memory for the preview converter");
    }

    // miniaudio copies the gains into its own heap during init, so `downmix` may die afterwards.
    StereoDownmix downmix(input_channels);
    ma_data_converter_config config = ma_data_converter_config_init(
        ma_format_f32, ma_format_f32, static_cast<ma_uint32>(input_channels), static_cast<ma_uint32>(kOutputChannels),
        static_cast<ma_uint32>(source_units), static_cast<ma_uint32>(output_units));
    config.channelMixMode = ma_channel_mix_mode_custom_weights;
    config.ppChannelWeights = downmix.weight_rows();
    config.resampling.algorithm = ma_resample_algorithm_linear;
    config.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER;

    if (ma_data_converter_init(&config, nullptr, &converter->engine_->converter) != MA_SUCCESS) {
        return fail(ErrorCode::ResourceLimit, "cannot initialize the preview converter");
    }
    converter->engine_->initialized = true;
    converter->input_channels_ = input_channels;
    return converter;
}

ConversionProgress StereoConverter::process(std::span<const float> input, std::span<float> stereo_output) noexcept {
    ma_uint64 input_frames = input.size() / input_channels_;
    ma_uint64 output_frames = stereo_output.size() / kOutputChannels;
    if (ma_data_converter_process_pcm_frames(&engine_->converter, input.data(), &input_frames, stereo_output.data(),
                                             &output_frames) != MA_SUCCESS) {
        return {};
    }
    return {static_cast<std::size_t>(input_frames), static_cast<std::size_t>(output_frames)};
}

}  // namespace mso::audio::detail
