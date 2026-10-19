#include "mso/audio/detail/tempo_pitch_stretcher.hpp"

#include <cmath>

#include <signalsmith-stretch/signalsmith-stretch.h>

#include "mso/audio/detail/pcm_ring.hpp"

namespace mso::audio::detail {
namespace {

/// Fixed seed: identical input and settings always give identical output (reproducible previews/tests).
constexpr long kStretchRandomSeed = 0x5EED;

/// signalsmith reads and writes `buffer[channel][frame]`; this adapts interleaved stereo to that.
template <class Sample>
struct InterleavedStereo {
    Sample* frames;  ///< first sample of the interleaved buffer

    /// One channel of the buffer, indexed by frame.
    struct Channel {
        Sample* frames;  ///< first sample of the interleaved buffer
        int channel;     ///< 0 = left, 1 = right
        /// Sample of `frame` in this channel.
        Sample& operator[](int frame) const { return frames[frame * static_cast<int>(kOutputChannels) + channel]; }
    };
    /// View of `channel`.
    Channel operator[](int channel) const { return Channel{frames, channel}; }
};

}  // namespace

/// The signalsmith stretcher, seeded for reproducible output.
struct TempoPitchStretcher::Engine {
    signalsmith::stretch::SignalsmithStretch<float> stretch{kStretchRandomSeed};  ///< phase vocoder
};

TempoPitchStretcher::TempoPitchStretcher(std::uint32_t sample_rate)
    : engine_(std::make_unique<Engine>()), sample_rate_(sample_rate) {
    engine_->stretch.presetDefault(static_cast<int>(kOutputChannels), static_cast<float>(sample_rate));
}

TempoPitchStretcher::~TempoPitchStretcher() = default;

std::size_t TempoPitchStretcher::priming_frames(double tempo) const {
    return static_cast<std::size_t>(std::ceil(engine_->stretch.outputSeekLength(static_cast<float>(tempo))));
}

void TempoPitchStretcher::prime(std::span<const float> stereo_input, double pitch_semitones) {
    engine_->stretch.setTransposeSemitones(static_cast<float>(pitch_semitones));
    InterleavedStereo<const float> input{stereo_input.data()};
    engine_->stretch.outputSeek(input, static_cast<int>(stereo_input.size() / kOutputChannels));
}

void TempoPitchStretcher::process(std::span<const float> stereo_input, std::span<float> stereo_output) {
    InterleavedStereo<const float> input{stereo_input.data()};
    InterleavedStereo<float> output{stereo_output.data()};
    engine_->stretch.process(input, static_cast<int>(stereo_input.size() / kOutputChannels), output,
                             static_cast<int>(stereo_output.size() / kOutputChannels));
}

}  // namespace mso::audio::detail
