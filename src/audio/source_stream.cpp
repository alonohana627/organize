#include "mso/audio/detail/source_stream.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <string>

#include "mso/audio/detail/sample_safety.hpp"
#include "mso/audio/detail/tempo_pitch_stretcher.hpp"

namespace mso::audio::detail {
namespace {

/// Source frames decoded per read: small enough to keep the decoder thread responsive.
constexpr std::size_t kSourceChunkFrames = 1024;
/// Length of the fade-in at a mid-signal start and of each seam fade.
constexpr std::int64_t kFadeMilliseconds = 5;
/// Highest source rate accepted (miniaudio's MA_MAX_SAMPLE_RATE); keeps the rate ratio in 32 bits.
constexpr int kMaximumSourceRate = 384000;
/// miniaudio's channel limit (MA_MAX_CHANNELS), restated so this file needs no miniaudio header.
constexpr int kMaximumSourceChannels = 254;

}  // namespace

SourceStream::SourceStream(std::unique_ptr<media::Decoder> decoder) : decoder_(std::move(decoder)) {}

SourceStream::~SourceStream() = default;

Result<std::unique_ptr<SourceStream>> SourceStream::create(std::unique_ptr<media::Decoder> decoder) {
    if (!decoder) {
        return fail(ErrorCode::InvalidArgument, "no decoder");
    }
    const int channels = decoder->channels();
    const int rate = decoder->sample_rate();
    if (channels <= 0 || channels > kMaximumSourceChannels) {
        return fail(ErrorCode::UnsupportedCodec, "unsupported channel count " + std::to_string(channels));
    }
    if (rate <= 0 || rate > kMaximumSourceRate) {
        return fail(ErrorCode::UnsupportedCodec, "unsupported sample rate " + std::to_string(rate));
    }
    std::unique_ptr<SourceStream> stream(new SourceStream(std::move(decoder)));
    stream->channels_ = static_cast<std::size_t>(channels);
    stream->source_rate_ = rate;
    stream->ramp_frames_ = std::max<std::int64_t>(1, rate * kFadeMilliseconds / 1000);
    stream->minimum_loop_frames_ = std::max<std::int64_t>(1, rate * kMinimumLoopMilliseconds / 1000);
    stream->staged_.assign(kSourceChunkFrames * stream->channels_, 0.0f);
    return stream;
}

std::optional<std::int64_t> SourceStream::exact_total_frames() const {
    if (!decoder_->frames_exact()) {
        return std::nullopt;
    }
    return decoder_->total_frames();
}

std::optional<LoopRegion> SourceStream::normalized_loop(const std::optional<LoopRegion>& requested) const {
    if (!requested || requested->end_frame <= requested->start_frame) {
        return std::nullopt;
    }
    LoopRegion region{std::max<std::int64_t>(requested->start_frame, 0), requested->end_frame};
    const std::optional<std::int64_t> total = exact_total_frames();
    if (total) {
        region.end_frame = std::min(region.end_frame, *total);
    }
    if (region.end_frame - region.start_frame < minimum_loop_frames_) {
        region.end_frame = region.start_frame + minimum_loop_frames_;
        if (total && region.end_frame > *total) {
            region.end_frame = *total;
            region.start_frame = std::max<std::int64_t>(0, *total - minimum_loop_frames_);
        }
    }
    if (region.end_frame <= region.start_frame) {
        return std::nullopt;  // an empty source has nothing to loop
    }
    return region;
}

std::optional<LoopRegion> SourceStream::effective_loop() const noexcept {
    return configured_loop_;
}

Result<void> SourceStream::restart(std::int64_t source_frame, const PlaybackShape& shape, std::uint32_t output_rate) {
    const std::optional<std::int64_t> total = exact_total_frames();
    std::int64_t target = std::max<std::int64_t>(source_frame, 0);
    if (total) {
        target = std::min(target, *total);
    }
    if (auto sought = decoder_->seek(target); !sought) {
        return std::unexpected(sought.error());
    }
    const std::int64_t anchor = decoder_->position();

    configured_loop_ = normalized_loop(shape.loop);
    loop_ = LoopFold{};
    // A loop applies when playback starts before its end; starting past it plays on to the end.
    if (configured_loop_ && anchor < configured_loop_->end_frame) {
        loop_ = LoopFold{configured_loop_->start_frame, configured_loop_->end_frame};
    }
    seam_fades_ = shape.seam_fades;

    clock_ = SourceClock(anchor, source_rate_, shape.speed_permille, output_rate);
    mode_ = shape.mode;
    // Varispeed resamples by rate × speed; the stretch mode resamples at 1× and lets the
    // stretcher change the tempo, so pitch is free to move independently.
    const SourceClock conversion =
        mode_ == PlaybackMode::Varispeed ? clock_ : SourceClock(anchor, source_rate_, kUnitySpeedPermille, output_rate);
    auto converter = StereoConverter::create(channels_, conversion.source_units(), conversion.output_units());
    if (!converter) {
        return std::unexpected(converter.error());
    }
    converter_ = std::move(*converter);

    read_position_ = anchor;
    // Starting at frame 0 keeps the attack intact; a start inside the signal fades in over 5 ms.
    frames_until_fade_in_ends_ = anchor > 0 ? ramp_frames_ : 0;
    output_frames_since_anchor_ = 0;
    staged_frames_ = 0;
    staged_offset_ = 0;
    input_exhausted_ = false;
    stretch_input_frames_ = 0;
    stretch_output_end_.reset();
    if (mode_ == PlaybackMode::IndependentTempoPitch) {
        return prime_stretcher(shape, output_rate);
    }
    return {};
}

Result<void> SourceStream::prime_stretcher(const PlaybackShape& shape, std::uint32_t output_rate) {
    try {
        if (!stretcher_ || stretcher_->sample_rate() != output_rate) {
            stretcher_ = std::make_unique<TempoPitchStretcher>(output_rate);
        }
        tempo_ = SourceClock(0, 1, shape.speed_permille, 1);  // stretcher input frames per output frame
        const double tempo = static_cast<double>(shape.speed_permille) / static_cast<double>(kUnitySpeedPermille);
        priming_frames_ = stretcher_->priming_frames(tempo);
        const auto largest_block_input = static_cast<std::size_t>(tempo_.source_frame_after(kPcmBlockFrames)) + 1;
        stretch_input_.resize(std::max(priming_frames_, largest_block_input) * kOutputChannels);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceLimit, "out of memory for the tempo/pitch stretcher");
    }
    const auto priming = std::span<float>(stretch_input_).first(priming_frames_ * kOutputChannels);
    auto converted = convert_into(priming);
    if (!converted) {
        return std::unexpected(converted.error());
    }
    std::fill(priming.begin() + static_cast<std::ptrdiff_t>(*converted * kOutputChannels), priming.end(), 0.0f);
    stretcher_->prime(priming, shape.pitch_semitones);
    account_stretch_input(*converted, priming_frames_);
    return {};
}

Result<void> SourceStream::render_block(PcmBlockHeader& header, std::span<float> stereo_samples) {
    const std::size_t capacity = std::min<std::size_t>(stereo_samples.size() / kOutputChannels, kPcmBlockFrames);
    const auto block = stereo_samples.first(capacity * kOutputChannels);
    auto written = mode_ == PlaybackMode::Varispeed ? convert_into(block) : stretch_into(block);
    if (!written) {
        return std::unexpected(written.error());
    }

    header.frame_count = static_cast<std::uint32_t>(*written);
    header.clock = clock_;
    header.first_output_frame = output_frames_since_anchor_;
    output_frames_since_anchor_ += static_cast<std::int64_t>(*written);
    header.loop = loop_;
    header.last_source_frame = exact_total_frames().value_or(header.last_source_frame);
    if (mode_ == PlaybackMode::Varispeed) {
        // The resampler's few frames of filter delay are not flushed: inaudible for a preview.
        header.end_of_stream = input_exhausted_ && staged_frames_remaining() == 0;
    } else {
        header.end_of_stream = stretch_output_end_ && output_frames_since_anchor_ >= *stretch_output_end_;
    }
    return {};
}

Result<std::size_t> SourceStream::convert_into(std::span<float> stereo_output) {
    const std::size_t capacity = stereo_output.size() / kOutputChannels;
    std::size_t written = 0;
    while (written < capacity) {
        if (staged_frames_remaining() == 0) {
            if (input_exhausted_) {
                break;
            }
            if (auto refilled = refill_staging(); !refilled) {
                return std::unexpected(refilled.error());
            }
            if (staged_frames_remaining() == 0) {
                break;  // the source just ran out
            }
        }
        const auto staged =
            std::span<const float>(staged_).subspan(staged_offset_ * channels_, staged_frames_remaining() * channels_);
        const ConversionProgress progress =
            converter_->process(staged, stereo_output.subspan(written * kOutputChannels));
        if (progress.input_frames == 0 && progress.output_frames == 0) {
            // With input and output space available the converter always moves; a stall means it
            // failed, and retrying would spin the decoder thread on empty blocks.
            return fail(ErrorCode::Internal, "the preview converter stopped making progress");
        }
        staged_offset_ += progress.input_frames;
        written += progress.output_frames;
    }
    return written;
}

Result<std::size_t> SourceStream::stretch_into(std::span<float> stereo_output) {
    const std::size_t output_frames = stereo_output.size() / kOutputChannels;
    if (stretch_output_end_ && output_frames_since_anchor_ >= *stretch_output_end_) {
        return std::size_t{0};
    }
    // Input for this block follows the exact tempo ratio, so tempo never drifts from the clock.
    const auto first = static_cast<std::size_t>(tempo_.source_frame_after(output_frames_since_anchor_));
    const auto last = static_cast<std::size_t>(
        tempo_.source_frame_after(output_frames_since_anchor_ + static_cast<std::int64_t>(output_frames)));
    const auto input = std::span<float>(stretch_input_).first((last - first) * kOutputChannels);

    std::size_t converted = 0;
    if (!input_exhausted_) {
        auto result = convert_into(input);
        if (!result) {
            return std::unexpected(result.error());
        }
        converted = *result;
        account_stretch_input(converted, input.size() / kOutputChannels);
    }
    // Past the end of the source the stretcher drains its look-ahead on silence.
    std::fill(input.begin() + static_cast<std::ptrdiff_t>(converted * kOutputChannels), input.end(), 0.0f);
    stretcher_->process(input, stereo_output);

    if (stretch_output_end_) {
        const std::int64_t remaining = *stretch_output_end_ - output_frames_since_anchor_;
        return static_cast<std::size_t>(
            std::clamp<std::int64_t>(remaining, 0, static_cast<std::int64_t>(output_frames)));
    }
    return output_frames;
}

void SourceStream::account_stretch_input(std::size_t converted, std::size_t requested) noexcept {
    stretch_input_frames_ += static_cast<std::int64_t>(converted);
    if (converted < requested && !stretch_output_end_) {
        // Output frame k carries stretcher input tempo(k): the sound ends where the input did.
        stretch_output_end_ = tempo_.output_frames_reaching(stretch_input_frames_);
    }
}

Result<void> SourceStream::refill_staging() {
    staged_frames_ = 0;
    staged_offset_ = 0;
    // Two attempts: the second covers a loop whose end lies beyond the real end of the file.
    for (int attempt = 0; attempt < 2 && staged_frames_ == 0; ++attempt) {
        if (loop_.is_active() && read_position_ >= loop_.end_frame) {
            if (auto wrapped = wrap_to_loop_start(); !wrapped) {
                return wrapped;
            }
        }
        std::size_t wanted = kSourceChunkFrames;
        if (loop_.is_active()) {
            wanted = std::min<std::size_t>(wanted, static_cast<std::size_t>(loop_.end_frame - read_position_));
        }
        auto read = decoder_->read(staged_.data(), wanted);
        if (!read) {
            return std::unexpected(read.error());
        }
        staged_frames_ = std::min(*read, wanted);
        if (staged_frames_ == 0) {
            if (!loop_.is_active() || read_position_ <= loop_.start_frame) {
                break;
            }
            // The source ended inside the loop (its length was an estimate): the real end is the
            // loop end from now on, for reading and for reported positions. The loop keeps its
            // minimum length, starting earlier when the shorter end would leave less.
            loop_.end_frame = read_position_;
            loop_.start_frame =
                std::min(loop_.start_frame, std::max<std::int64_t>(0, loop_.end_frame - minimum_loop_frames_));
            configured_loop_ = LoopRegion{loop_.start_frame, loop_.end_frame};
            if (auto wrapped = wrap_to_loop_start(); !wrapped) {
                return wrapped;
            }
        }
    }
    if (staged_frames_ == 0) {
        input_exhausted_ = true;
        return {};
    }
    replace_non_finite_with_silence(std::span<float>(staged_).first(staged_frames_ * channels_));
    apply_fades();
    read_position_ += static_cast<std::int64_t>(staged_frames_);
    return {};
}

Result<void> SourceStream::wrap_to_loop_start() {
    if (auto sought = decoder_->seek(loop_.start_frame); !sought) {
        return std::unexpected(sought.error());
    }
    // Compressed decoders may land a few frames off; continue from where the decoder really is.
    read_position_ = decoder_->position();
    if (seam_fades_) {
        frames_until_fade_in_ends_ = ramp_frames_;
    }
    return {};
}

void SourceStream::apply_fades() noexcept {
    const auto chunk_frames = static_cast<std::int64_t>(staged_frames_);
    const bool touches_fade_in = frames_until_fade_in_ends_ > 0;
    const bool touches_seam_fade_out =
        seam_fades_ && loop_.is_active() && loop_.end_frame - (read_position_ + chunk_frames) < ramp_frames_;
    if (!touches_fade_in && !touches_seam_fade_out) {
        return;
    }
    const auto ramp_length = static_cast<float>(ramp_frames_);
    for (std::int64_t frame = 0; frame < chunk_frames; ++frame) {
        float gain = 1.0f;
        if (frame < frames_until_fade_in_ends_) {
            gain = static_cast<float>(ramp_frames_ - frames_until_fade_in_ends_ + frame) / ramp_length;
        }
        if (touches_seam_fade_out) {
            const std::int64_t frames_to_loop_end = loop_.end_frame - (read_position_ + frame);
            if (frames_to_loop_end < ramp_frames_) {
                gain = std::min(gain, static_cast<float>(frames_to_loop_end) / ramp_length);
            }
        }
        if (gain < 1.0f) {
            float* samples = staged_.data() + static_cast<std::size_t>(frame) * channels_;
            for (std::size_t channel = 0; channel < channels_; ++channel) {
                samples[channel] *= gain;
            }
        }
    }
    frames_until_fade_in_ends_ = std::max<std::int64_t>(0, frames_until_fade_in_ends_ - chunk_frames);
}

}  // namespace mso::audio::detail
