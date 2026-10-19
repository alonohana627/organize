// Preview engine behavior against the Offline backend (deterministic, caller-driven output) and the
// null device (real-time pacing), fed by an in-memory decoder. No sound card is needed.
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "mso/audio/preview_engine.hpp"

namespace {

using namespace mso;
using namespace mso::audio;
using namespace std::chrono_literals;

constexpr int kRate = 48000;
/// 5 ms declick ramps plus margin: frames to skip before checking exact sample values.
constexpr std::size_t kSettleFrames = 480;

/// Deterministic in-memory decoder: every sample comes from a function of (frame, channel), and it
/// can be told to fail like a file that vanished mid-read.
class SyntheticDecoder final : public media::Decoder {
public:
    using SampleFunction = std::function<float(std::int64_t frame, int channel)>;

    SyntheticDecoder(int channels, int sample_rate, std::int64_t total_frames, SampleFunction sample,
                     std::optional<std::int64_t> failure_frame = std::nullopt)
        : channels_(channels),
          sample_rate_(sample_rate),
          total_frames_(total_frames),
          sample_(std::move(sample)),
          failure_frame_(failure_frame) {}

    int channels() const override { return channels_; }
    int sample_rate() const override { return sample_rate_; }
    std::optional<std::int64_t> total_frames() const override { return total_frames_; }
    bool frames_exact() const override { return true; }
    std::int64_t position() const override { return position_; }
    std::string codec() const override { return "synthetic"; }

    Result<std::size_t> read(float* out, std::size_t frames) override {
        if (failure_frame_ && position_ >= *failure_frame_) {
            return fail(ErrorCode::FileMissing, "synthetic source vanished");
        }
        std::int64_t end = std::min(total_frames_, position_ + static_cast<std::int64_t>(frames));
        if (failure_frame_) {
            end = std::min(end, *failure_frame_);
        }
        const auto count = static_cast<std::size_t>(end - position_);
        for (std::size_t frame = 0; frame < count; ++frame) {
            for (int channel = 0; channel < channels_; ++channel) {
                out[frame * static_cast<std::size_t>(channels_) + static_cast<std::size_t>(channel)] =
                    sample_(position_ + static_cast<std::int64_t>(frame), channel);
            }
        }
        position_ = end;
        return count;
    }

    Result<void> seek(std::int64_t frame) override {
        position_ = std::clamp<std::int64_t>(frame, 0, total_frames_);
        return {};
    }

private:
    int channels_;
    int sample_rate_;
    std::int64_t total_frames_;
    SampleFunction sample_;
    std::optional<std::int64_t> failure_frame_;
    std::int64_t position_ = 0;
};

std::unique_ptr<media::Decoder> constant_source(float level, int channels = 2, int sample_rate = kRate,
                                                std::int64_t total_frames = 10 * kRate) {
    return std::make_unique<SyntheticDecoder>(channels, sample_rate, total_frames,
                                              [level](std::int64_t, int) { return level; });
}

constexpr double kToneHz = 1000.0;

/// Ten seconds of a stereo sine at `frequency`, half scale.
std::unique_ptr<media::Decoder> sine_source(double frequency) {
    return std::make_unique<SyntheticDecoder>(2, kRate, 10 * kRate, [frequency](std::int64_t frame, int) {
        return static_cast<float>(
            0.5 * std::sin(2.0 * 3.14159265358979323846 * frequency * static_cast<double>(frame) / kRate));
    });
}

/// Frequency of the strongest tone in the left channel, from rising zero crossings after the first
/// 100 ms (start-up transients excluded).
double dominant_frequency(const std::vector<float>& stereo) {
    const std::size_t first_frame = kRate / 10;
    const std::size_t frame_count = stereo.size() / 2;
    std::size_t first_crossing = 0;
    std::size_t last_crossing = 0;
    std::size_t crossings = 0;
    for (std::size_t frame = first_frame + 1; frame < frame_count; ++frame) {
        if (stereo[(frame - 1) * 2] < 0.0f && stereo[frame * 2] >= 0.0f) {
            if (crossings == 0) {
                first_crossing = frame;
            }
            last_crossing = frame;
            ++crossings;
        }
    }
    if (crossings < 2) {
        return 0.0;
    }
    return static_cast<double>(crossings - 1) * kRate / static_cast<double>(last_crossing - first_crossing);
}

std::unique_ptr<PreviewEngine> make_offline_engine(std::uint32_t sample_rate = kRate) {
    PreviewOptions options;
    options.backend = OutputBackend::Offline;
    options.sample_rate = sample_rate;
    auto engine = PreviewEngine::create(options);
    REQUIRE(engine.has_value());
    return std::move(*engine);
}

/// Polls snapshots until `done` holds; fails the test after `timeout`.
template <class Predicate>
PreviewSnapshot wait_until(const PreviewEngine& engine, Predicate done, std::chrono::milliseconds timeout = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    PreviewSnapshot snapshot = engine.snapshot();
    while (!done(snapshot) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
        snapshot = engine.snapshot();
    }
    REQUIRE(done(snapshot));
    return snapshot;
}

/// Pulls `frames` of stereo output the way a device callback would, giving the decoder thread a
/// moment to stay ahead whenever the transport is playing.
std::vector<float> pull(PreviewEngine& engine, std::size_t frames) {
    constexpr std::size_t kChunkFrames = 128;
    std::vector<float> output(frames * 2, std::numeric_limits<float>::quiet_NaN());
    for (std::size_t done = 0; done < frames;) {
        const std::size_t count = std::min(kChunkFrames, frames - done);
        const auto deadline = std::chrono::steady_clock::now() + 200ms;
        for (PreviewSnapshot snapshot = engine.snapshot();
             snapshot.state == PreviewState::Playing && snapshot.buffered_milliseconds < 3 &&
             std::chrono::steady_clock::now() < deadline;
             snapshot = engine.snapshot()) {
            std::this_thread::sleep_for(100us);
        }
        REQUIRE(engine.render_offline(std::span<float>(output).subspan(done * 2, count * 2)).has_value());
        done += count;
    }
    return output;
}

void open_and_wait_until_ready(PreviewEngine& engine, std::unique_ptr<media::Decoder> decoder) {
    REQUIRE(engine.open(std::move(decoder)).has_value());
    wait_until(engine, [](const PreviewSnapshot& snapshot) {
        return snapshot.state == PreviewState::Ready && snapshot.buffered_milliseconds >= 100;
    });
}

void play(PreviewEngine& engine) {
    engine.post(transport::Play{});
    wait_until(engine, [](const PreviewSnapshot& snapshot) { return snapshot.state == PreviewState::Playing; });
}

bool all_samples_equal(std::span<const float> samples, float expected) {
    return std::all_of(samples.begin(), samples.end(), [expected](float sample) { return sample == expected; });
}

}  // namespace

TEST_SUITE("audio") {
    TEST_CASE("play advances the position at the source rate, exactly, through resampling") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(0.5f, 1, 44100, 10 * 44100));
        CHECK(engine->snapshot().source_sample_rate == 44100);
        CHECK(engine->snapshot().source_channels == 1);
        play(*engine);

        pull(*engine, kRate);  // one second of device output

        const PreviewSnapshot snapshot = engine->snapshot();
        CHECK(snapshot.position_frames == 44100);
        CHECK(snapshot.underrun_count == 0);
    }

    TEST_CASE("the null device plays in real time without a sound card") {
        PreviewOptions options;
        options.backend = OutputBackend::Null;
        auto created = PreviewEngine::create(options);
        REQUIRE(created.has_value());
        PreviewEngine& engine = **created;

        const PreviewSnapshot device = engine.snapshot();
        CHECK(device.output_device_available);
        CHECK_FALSE(device.output_device_name.empty());
        CHECK(device.output_sample_rate == 48000);
        CHECK(device.output_period_frames > 0);
        auto devices = engine.list_output_devices();
        REQUIRE(devices.has_value());
        CHECK_FALSE(devices->empty());

        open_and_wait_until_ready(engine, constant_source(0.25f));
        play(engine);
        std::this_thread::sleep_for(300ms);
        const PreviewSnapshot playing = engine.snapshot();
        CHECK(playing.position_frames > kRate / 10);
        CHECK(playing.position_frames < kRate);
    }

    TEST_CASE("pause holds the position and play resumes from it") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(0.5f));
        play(*engine);
        pull(*engine, 4800);

        engine->post(transport::Pause{});
        const std::int64_t paused_at = wait_until(*engine, [](const PreviewSnapshot& snapshot) {
                                           return snapshot.state == PreviewState::Paused;
                                       }).position_frames;
        pull(*engine, 4800);  // the 5 ms fade-out still consumes a few frames
        const std::int64_t held = engine->snapshot().position_frames;
        CHECK(held - paused_at <= kRate / 200 + 1);

        const std::vector<float> silent = pull(*engine, 4800);
        CHECK(all_samples_equal(silent, 0.0f));
        CHECK(engine->snapshot().position_frames == held);

        play(*engine);
        pull(*engine, 4800);
        CHECK(engine->snapshot().position_frames == held + 4800);
    }

    TEST_CASE("seek lands on the requested frame and repeated seeks coalesce") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(0.5f));

        engine->post(transport::SeekToFrame{12345});
        wait_until(*engine, [](const PreviewSnapshot& snapshot) { return snapshot.position_frames == 12345; });
        play(*engine);
        pull(*engine, 4800);
        CHECK(engine->snapshot().position_frames == 12345 + 4800);

        engine->post(transport::Pause{});
        engine->post(transport::SeekToFraction{0.5});
        const auto half = wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.state == PreviewState::Paused && snapshot.position_frames == 5 * kRate;
        });
        CHECK(half.total_frames == 10 * kRate);

        for (std::int64_t target = 0; target < 200; ++target) {  // a scrubbed seek bar
            engine->post(transport::SeekToFrame{target * 1000});
        }
        const auto scrubbed =
            wait_until(*engine, [](const PreviewSnapshot& snapshot) { return snapshot.position_frames == 199'000; });
        CHECK(scrubbed.dropped_command_count == 0);
    }

    TEST_CASE("stop rewinds to the start and silences output") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(0.5f));
        play(*engine);
        pull(*engine, 9600);

        engine->post(transport::Stop{});
        const auto stopped = wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.state == PreviewState::Stopped && snapshot.position_frames == 0;
        });
        CHECK_FALSE(stopped.last_error.has_value());

        const std::vector<float> after = pull(*engine, 4800);
        CHECK(all_samples_equal(std::span(after).subspan(kSettleFrames * 2), 0.0f));
        CHECK(engine->snapshot().position_frames == 0);
    }

    TEST_CASE("opening a new source invalidates the old generation's PCM") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(0.5f));
        play(*engine);
        const std::vector<float> first = pull(*engine, 4800);
        CHECK(all_samples_equal(std::span(first).subspan(kSettleFrames * 2), 0.5f));
        const std::uint64_t first_generation = engine->snapshot().generation;

        // Plenty of the old source is still buffered when the new one is opened.
        open_and_wait_until_ready(*engine, constant_source(-0.25f));
        CHECK(engine->snapshot().generation == first_generation + 1);
        CHECK(engine->snapshot().position_frames == 0);
        play(*engine);
        const std::vector<float> second = pull(*engine, 4800);
        CHECK(all_samples_equal(std::span(second).subspan(kSettleFrames * 2), -0.25f));
    }

    TEST_CASE("a loop wraps at the region end and never plays past it") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(
            *engine, std::make_unique<SyntheticDecoder>(2, kRate, 10 * kRate, [](std::int64_t frame, int) {
                return static_cast<float>(frame) / 100'000.0f;  // the sample value reveals its source frame
            }));
        engine->post(transport::SetLoop{LoopRegion{1000, 5800}});
        engine->post(transport::SeekToFrame{1000});
        wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.position_frames == 1000 && snapshot.buffered_milliseconds >= 100;
        });
        play(*engine);

        const std::vector<float> output = pull(*engine, 12000);  // 2.5 passes through the 4800-frame region

        // 1000 + 12000 frames unfolded = 13000, folded back into [1000, 5800).
        CHECK(engine->snapshot().position_frames == 3400);
        const auto [lowest, highest] = std::minmax_element(output.begin(), output.end());
        CHECK(*lowest >= 0.0f);
        CHECK(*highest < 5800.0f / 100'000.0f);
    }

    TEST_CASE("speed 2x advances the source twice as fast") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(0.5f));
        engine->post(transport::SetSpeed{2.0});
        play(*engine);
        pull(*engine, kRate);
        CHECK(engine->snapshot().position_frames == 2 * kRate);
    }

    TEST_CASE("volume changes glide per sample and settle on the target") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(0.5f));
        play(*engine);
        pull(*engine, 4800);

        engine->post(transport::SetVolumeDecibels{-6.0206});
        wait_until(*engine, [](const PreviewSnapshot& snapshot) { return std::abs(snapshot.volume - 0.5f) < 1e-4f; });
        const std::vector<float> output = pull(*engine, 4800);

        float largest_step = 0.0f;
        for (std::size_t sample = 2; sample < output.size(); sample += 2) {
            largest_step = std::max(largest_step, std::abs(output[sample] - output[sample - 2]));
        }
        CHECK(output.front() > 0.49f);        // the change starts from the old level...
        CHECK(largest_step < 0.5f / 900.0f);  // ...moves by at most a 20 ms full-scale glide step...
        CHECK(output.back() == doctest::Approx(0.25f).epsilon(1e-4));  // ...and lands on the target
    }

    TEST_CASE("NaN and infinite decoder output is silenced") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine,
                                  std::make_unique<SyntheticDecoder>(2, kRate, 10 * kRate, [](std::int64_t frame, int) {
                                      switch (frame % 3) {
                                          case 0:
                                              return std::numeric_limits<float>::quiet_NaN();
                                          case 1:
                                              return std::numeric_limits<float>::infinity();
                                          default:
                                              return 0.25f;
                                      }
                                  }));
        play(*engine);
        const std::vector<float> output = pull(*engine, 4800);

        CHECK(std::all_of(output.begin(), output.end(), [](float sample) { return std::isfinite(sample); }));
        for (std::size_t frame = kSettleFrames; frame < 4800; ++frame) {
            const float expected = frame % 3 == 2 ? 0.25f : 0.0f;
            REQUIRE(output[frame * 2] == expected);
            REQUIRE(output[frame * 2 + 1] == expected);
        }
    }

    TEST_CASE("surround sources fold to stereo without clipping") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(1.0f, 6));  // 5.1 at full scale
        play(*engine);
        const std::vector<float> output = pull(*engine, 4800);
        for (std::size_t sample = kSettleFrames * 2; sample < output.size(); ++sample) {
            REQUIRE(output[sample] <= 1.0f);
            REQUIRE(output[sample] > 0.99f);
        }
    }

    TEST_CASE("prebuffering stays within the 250 ms bound") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(0.5f));
        std::this_thread::sleep_for(100ms);  // the decoder thread has had time to overfill, if it would
        CHECK(engine->snapshot().buffered_milliseconds <= 250);
    }

    TEST_CASE("a source that vanishes mid-play stops, flushes and reports FILE_MISSING") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine,
                                  std::make_unique<SyntheticDecoder>(
                                      2, kRate, 10 * kRate, [](std::int64_t, int) { return 0.5f; }, kRate / 2));
        play(*engine);
        pull(*engine, kRate);

        const auto failed =
            wait_until(*engine, [](const PreviewSnapshot& snapshot) { return snapshot.state == PreviewState::Error; });
        CHECK(failed.last_error == ErrorCode::FileMissing);
        CHECK(failed.buffered_milliseconds == 0);
        const std::vector<float> after = pull(*engine, 4800);
        CHECK(all_samples_equal(std::span(after).subspan(kSettleFrames * 2), 0.0f));

        engine->post(transport::Play{});  // replay is disabled until a new open()
        std::this_thread::sleep_for(20ms);
        CHECK(engine->snapshot().state == PreviewState::Error);
    }

    TEST_CASE("reaching the end of the source stops and rewinds") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(0.5f, 2, kRate, 4800));
        play(*engine);
        pull(*engine, 9600);
        wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.state == PreviewState::Stopped && snapshot.position_frames == 0;
        });
    }

    TEST_CASE("Stop gets through a full mailbox and keeps its place in order") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(0.5f));
        play(*engine);
        pull(*engine, 4800);

        for (int command = 0; command < 1000; ++command) {  // far more than the mailbox holds
            engine->post(command % 2 == 0 ? TransportCommand{transport::Pause{}} : TransportCommand{transport::Play{}});
        }
        engine->post(transport::Stop{});
        const auto stopped = wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.state == PreviewState::Stopped && snapshot.position_frames == 0;
        });
        CHECK(stopped.dropped_command_count > 0);

        engine->post(transport::Stop{});  // Stop, then a seek posted after it, lands where asked
        engine->post(transport::SeekToFrame{4321});
        wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.state == PreviewState::Stopped && snapshot.position_frames == 4321;
        });
    }

    TEST_CASE("pitch shift in tempo/pitch mode moves the dominant frequency by the semitone ratio") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, sine_source(kToneHz));
        engine->post(transport::SetPlaybackMode{PlaybackMode::IndependentTempoPitch});
        engine->post(transport::SetPitch{12.0});
        wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.playback_mode == PlaybackMode::IndependentTempoPitch && snapshot.pitch_semitones == 12.0 &&
                   snapshot.buffered_milliseconds >= 100;
        });
        play(*engine);
        const std::vector<float> output = pull(*engine, kRate);

        CHECK(dominant_frequency(output) == doctest::Approx(2.0 * kToneHz).epsilon(0.03));
        CHECK(engine->snapshot().position_frames == kRate);  // speed 1: the source advances in real time
    }

    TEST_CASE("tempo change in tempo/pitch mode keeps the frequency") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, sine_source(kToneHz));
        engine->post(transport::SetPlaybackMode{PlaybackMode::IndependentTempoPitch});
        engine->post(transport::SetSpeed{1.5});
        wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.playback_mode == PlaybackMode::IndependentTempoPitch && snapshot.speed_ratio == 1.5 &&
                   snapshot.buffered_milliseconds >= 100;
        });
        play(*engine);
        const std::vector<float> output = pull(*engine, kRate);

        CHECK(dominant_frequency(output) == doctest::Approx(kToneHz).epsilon(0.03));
        CHECK(engine->snapshot().position_frames == kRate * 3 / 2);
    }

    TEST_CASE("varispeed makes pitch follow speed and ignores the pitch setting") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, sine_source(kToneHz));
        engine->post(transport::SetPitch{-12.0});
        engine->post(transport::SetSpeed{1.5});
        wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.speed_ratio == 1.5 && snapshot.buffered_milliseconds >= 100;
        });
        CHECK(engine->snapshot().playback_mode == PlaybackMode::Varispeed);
        play(*engine);
        const std::vector<float> output = pull(*engine, kRate / 2);
        CHECK(dominant_frequency(output) == doctest::Approx(1.5 * kToneHz).epsilon(0.03));
    }

    TEST_CASE("extreme tempo/pitch settings stay finite and bounded") {
        for (const auto& [speed, pitch] : {std::pair{4.0, 24.0}, std::pair{0.25, -24.0}, std::pair{4.0, -24.0}}) {
            CAPTURE(speed);
            CAPTURE(pitch);
            auto engine = make_offline_engine();
            open_and_wait_until_ready(*engine, sine_source(kToneHz));
            engine->post(transport::SetPlaybackMode{PlaybackMode::IndependentTempoPitch});
            engine->post(transport::SetSpeed{speed});
            engine->post(transport::SetPitch{pitch});
            wait_until(*engine, [pitch](const PreviewSnapshot& snapshot) {
                return snapshot.pitch_semitones == pitch && snapshot.buffered_milliseconds >= 50;
            });
            play(*engine);
            const std::vector<float> output = pull(*engine, kRate / 4);
            CHECK(std::all_of(output.begin(), output.end(),
                              [](float sample) { return std::isfinite(sample) && std::abs(sample) <= 1.0f; }));
            CHECK(engine->snapshot().state == PreviewState::Playing);
        }
    }

}  // TEST_SUITE("audio")
