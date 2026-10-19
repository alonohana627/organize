// Preview audio: the internal building blocks tested directly, then the engine end to end against
// the Offline backend (deterministic, caller-driven output) and the null device (real-time pacing),
// fed by in-memory decoders. No sound card is needed and no test waits on a fixed sleep: every wait
// polls a condition against a deadline.
#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "mso/audio/detail/command_mailbox.hpp"
#include "mso/audio/detail/denormal_flush.hpp"
#include "mso/audio/detail/linear_ramp.hpp"
#include "mso/audio/detail/output_supervisor.hpp"
#include "mso/audio/detail/pcm_ring.hpp"
#include "mso/audio/detail/playback_controller.hpp"
#include "mso/audio/detail/playback_renderer.hpp"
#include "mso/audio/detail/playhead_history.hpp"
#include "mso/audio/detail/seqlock.hpp"
#include "mso/audio/detail/shared_playback_state.hpp"
#include "mso/audio/detail/source_position.hpp"
#include "mso/audio/detail/stereo_downmix.hpp"
#include "mso/audio/detail/wakeup_signal.hpp"
#include "mso/audio/fixed_text.hpp"
#include "mso/audio/preview_engine.hpp"
#include "test_support.hpp"

namespace {

using namespace mso;
using namespace mso::audio;
using namespace std::chrono_literals;

constexpr int kRate = 48000;
/// 5 ms fades plus margin: frames to skip before checking exact sample values after a cut.
constexpr std::size_t kSettleFrames = 480;
/// Generous bound for anything the decoder thread does in response to a command.
constexpr auto kDeadline = 5000ms;

/// Polls `done` (every millisecond, through test::wait_until) until it holds or `timeout` passes;
/// returns whether it held.
template <class Condition>
bool eventually(Condition done, std::chrono::milliseconds timeout = kDeadline) {
    return test::wait_until(std::function<bool()>(std::move(done)), timeout, 1ms);
}

// ---------------------------------------------------------------- synthetic sources

/// Deterministic in-memory decoder: every sample comes from a function of (frame, channel). It can
/// fail like a vanished file, throw like a broken library, or hold its reads at a gate frame until
/// released (to starve the callback on purpose).
class SyntheticDecoder final : public media::Decoder {
public:
    using SampleFunction = std::function<float(std::int64_t frame, int channel)>;

    SyntheticDecoder(int channels, int sample_rate, std::int64_t total_frames, SampleFunction sample)
        : channels_(channels), sample_rate_(sample_rate), total_frames_(total_frames), sample_(std::move(sample)) {}

    /// Reads at or past `frame` fail with FILE_MISSING.
    SyntheticDecoder& failing_at(std::int64_t frame) {
        failure_frame_ = frame;
        return *this;
    }
    /// Reads at or past `frame` throw std::runtime_error.
    SyntheticDecoder& throwing_at(std::int64_t frame) {
        throw_frame_ = frame;
        return *this;
    }
    /// Reads at or past `frame` block while `gate_open` is false.
    SyntheticDecoder& gated_at(std::int64_t frame, std::shared_ptr<std::atomic<bool>> gate_open) {
        gate_frame_ = frame;
        gate_open_ = std::move(gate_open);
        return *this;
    }
    /// Reports `estimate` as an inexact length (VBR-style) while the real end stays where it is.
    SyntheticDecoder& estimating_length(std::int64_t estimate) {
        estimated_frames_ = estimate;
        return *this;
    }

    int channels() const override { return channels_; }
    int sample_rate() const override { return sample_rate_; }
    std::optional<std::int64_t> total_frames() const override { return estimated_frames_.value_or(total_frames_); }
    bool frames_exact() const override { return !estimated_frames_; }
    std::int64_t position() const override { return position_; }
    std::string codec() const override { return "synthetic"; }

    Result<std::size_t> read(float* out, std::size_t frames) override {
        if (gate_open_ && position_ >= gate_frame_) {
            gate_open_->wait(false);
        }
        if (throw_frame_ && position_ >= *throw_frame_) {
            throw std::runtime_error("synthetic decoder failure");
        }
        if (failure_frame_ && position_ >= *failure_frame_) {
            return fail(ErrorCode::FileMissing, "synthetic source vanished");
        }
        std::int64_t end = std::min(total_frames_, position_ + static_cast<std::int64_t>(frames));
        for (const auto limit : {failure_frame_, throw_frame_}) {
            if (limit && position_ < *limit) {
                end = std::min(end, *limit);
            }
        }
        if (gate_open_ && position_ < gate_frame_) {
            end = std::min(end, gate_frame_);
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
    std::optional<std::int64_t> throw_frame_;
    std::int64_t gate_frame_ = 0;
    std::shared_ptr<std::atomic<bool>> gate_open_;
    std::optional<std::int64_t> estimated_frames_;
    std::int64_t position_ = 0;
};

/// A gate a test opens once: threads wait on it, the test releases them.
struct TestGate {
    std::atomic<bool> open{false};
    std::atomic<bool> reached{false};  ///< a thread has arrived at the gate

    void wait() {
        reached = true;
        open.wait(false);
    }
    void release() {
        open = true;
        open.notify_all();
    }
};

/// A decoder whose opening (the first channels() call) waits at a gate and can then throw, to hold
/// the decoder thread inside open() while the test looks at snapshots.
class GatedOpeningDecoder final : public media::Decoder {
public:
    GatedOpeningDecoder(std::shared_ptr<TestGate> gate, bool throws) : gate_(std::move(gate)), throws_(throws) {}

    int channels() const override {
        if (!opened_) {
            opened_ = true;
            gate_->wait();
            if (throws_) {
                throw std::runtime_error("synthetic failure while opening");
            }
        }
        return 2;
    }
    int sample_rate() const override { return kRate; }
    std::optional<std::int64_t> total_frames() const override { return kRate; }
    bool frames_exact() const override { return true; }
    std::int64_t position() const override { return position_; }
    std::string codec() const override { return "gated"; }
    Result<std::size_t> read(float* out, std::size_t frames) override {
        const auto count = std::min<std::size_t>(frames, static_cast<std::size_t>(kRate - position_));
        std::fill(out, out + count * 2, 0.25f);
        position_ += static_cast<std::int64_t>(count);
        return count;
    }
    Result<void> seek(std::int64_t frame) override {
        position_ = std::clamp<std::int64_t>(frame, 0, kRate);
        return {};
    }

private:
    std::shared_ptr<TestGate> gate_;
    bool throws_;
    mutable bool opened_ = false;
    std::int64_t position_ = 0;
};

std::unique_ptr<SyntheticDecoder> constant_source(float level, int channels = 2, int sample_rate = kRate,
                                                  std::int64_t total_frames = 10 * kRate) {
    return std::make_unique<SyntheticDecoder>(channels, sample_rate, total_frames,
                                              [level](std::int64_t, int) { return level; });
}

/// Every sample reveals its source frame: frame / 100,000.
constexpr float kRampScale = 100'000.0f;
float ramp_value(std::int64_t frame) {
    return static_cast<float>(frame) / kRampScale;
}
std::unique_ptr<SyntheticDecoder> ramp_source(std::int64_t total_frames = 10 * kRate) {
    return std::make_unique<SyntheticDecoder>(2, kRate, total_frames,
                                              [](std::int64_t frame, int) { return ramp_value(frame); });
}

constexpr double kToneHz = 1000.0;

/// A stereo sine at `frequency`, half scale.
std::unique_ptr<SyntheticDecoder> sine_source(double frequency, std::int64_t total_frames = 10 * kRate) {
    return std::make_unique<SyntheticDecoder>(2, kRate, total_frames, [frequency](std::int64_t frame, int) {
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

// ---------------------------------------------------------------- engine helpers

std::unique_ptr<PreviewEngine> make_offline_engine(std::uint32_t sample_rate = kRate) {
    PreviewOptions options;
    options.backend = OutputBackend::Offline;
    options.sample_rate = sample_rate;
    auto engine = PreviewEngine::create(options);
    REQUIRE(engine.has_value());
    return std::move(*engine);
}

/// Polls snapshots until `done` holds; fails the test after the deadline.
template <class Predicate>
PreviewSnapshot wait_until(const PreviewEngine& engine, Predicate done) {
    PreviewSnapshot snapshot = engine.snapshot();
    const bool held = eventually([&] {
        snapshot = engine.snapshot();
        return done(snapshot);
    });
    REQUIRE(held);
    return snapshot;
}

/// Pulls `frames` of stereo output the way a device callback would, giving the decoder thread the
/// chance to stay ahead whenever the transport is playing (the Offline backend has no real-time
/// pressure, so a starved chunk would only mean the test outran the decoder thread).
std::vector<float> pull(PreviewEngine& engine, std::size_t frames) {
    constexpr std::size_t kChunkFrames = 128;
    std::vector<float> output(frames * 2, std::numeric_limits<float>::quiet_NaN());
    for (std::size_t done = 0; done < frames;) {
        const std::size_t count = std::min(kChunkFrames, frames - done);
        // Near the end of a source the buffer legitimately runs low; the short deadline covers that.
        (void)eventually(
            [&] {
                const PreviewSnapshot snapshot = engine.snapshot();
                return snapshot.state != PreviewState::Playing || snapshot.buffered_milliseconds >= 3;
            },
            200ms);
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

/// Waits until every command posted so far has been applied (commands apply in order, and a volume
/// change is visible in the snapshot), then restores full volume.
void wait_for_commands(PreviewEngine& engine) {
    engine.post(transport::SetVolumeLinear{0.999});
    wait_until(engine, [](const PreviewSnapshot& snapshot) { return snapshot.volume == 0.999f; });
    engine.post(transport::SetVolumeLinear{1.0});
    wait_until(engine, [](const PreviewSnapshot& snapshot) { return snapshot.volume == 1.0f; });
}

bool all_samples_equal(std::span<const float> samples, float expected) {
    return std::all_of(samples.begin(), samples.end(), [expected](float sample) { return sample == expected; });
}

// ---------------------------------------------------------------- direct renderer fixture

/// A renderer with its ring and shared state, fed by hand-made blocks (no decoder thread).
struct RendererFixture {
    detail::PcmRing ring{4096};
    detail::SharedPlaybackState shared;
    detail::PlaybackRenderer renderer{ring, shared};

    explicit RendererFixture(std::uint32_t latency_frames = 0) { renderer.configure(kRate, latency_frames); }

    /// Queues one full block of `level` for `epoch` at `rate`, covering source frames from `first`.
    void write_block(std::uint64_t epoch, float level, std::int64_t first_output_frame = 0,
                     std::uint32_t rate = kRate) {
        auto block = ring.begin_write();
        REQUIRE(block.has_value());
        block->header->epoch = epoch;
        block->header->output_rate = rate;
        block->header->frame_count = detail::kPcmBlockFrames;
        block->header->clock = detail::SourceClock(0, kRate, detail::kUnitySpeedPermille, kRate);
        block->header->first_output_frame = first_output_frame;
        std::fill(block->samples.begin(), block->samples.end(), level);
        ring.commit_write();
    }

    void publish_gate(std::uint64_t epoch, bool output_enabled) {
        shared.transport_gate.store(detail::TransportGate{epoch, output_enabled}.packed());
    }

    std::vector<float> render(std::size_t frames) {
        std::vector<float> output(frames * 2);
        renderer.render(output);
        return output;
    }
};

}  // namespace

// ================================================================ building blocks

TEST_SUITE("audio") {
    TEST_CASE("FixedText truncates only at UTF-8 code-point boundaries") {
        CHECK(FixedText<8>::truncated_from("short").view() == "short");
        CHECK(FixedText<5>::truncated_from("exactly5").view() == "exact");
        // "é" is two bytes (C3 A9); it straddles the 4-byte limit and is dropped whole.
        CHECK(FixedText<4>::truncated_from("abc\xC3\xA9").view() == "abc");
        CHECK(FixedText<5>::truncated_from("abc\xC3\xA9").view() == "abc\xC3\xA9");
        // "€" is three bytes, "🎵" four: neither may be cut.
        CHECK(FixedText<4>::truncated_from("a\xE2\x82\xAC!").view() == "a\xE2\x82\xAC");
        CHECK(FixedText<3>::truncated_from("a\xE2\x82\xAC").view() == "a");
        CHECK(FixedText<4>::truncated_from("\xF0\x9F\x8E\xB5x").view() == "\xF0\x9F\x8E\xB5");
        CHECK(FixedText<3>::truncated_from("\xF0\x9F\x8E\xB5").empty());
    }

    TEST_CASE("Seqlock readers never see a half-written value") {
        struct Pair {
            std::int64_t value = 0;
            std::int64_t doubled = 0;
        };
        detail::Seqlock<Pair> cell;
        std::atomic<bool> writing{true};
        std::jthread writer([&] {
            for (std::int64_t value = 1; value <= 200'000; ++value) {
                cell.store({value, value * 2});
            }
            writing = false;
        });
        std::int64_t torn = 0;
        bool went_back = false;
        std::int64_t newest = 0;
        while (writing) {
            const Pair read = cell.load();
            torn += read.doubled != read.value * 2 ? 1 : 0;
            went_back = went_back || read.value < newest;
            newest = read.value;
        }
        CHECK_FALSE(went_back);
        CHECK(torn == 0);
        CHECK(cell.load().value == 200'000);
    }

    TEST_CASE("SourceClock maps output to source frames exactly and inverts") {
        const detail::SourceClock clock(1000, 44100, detail::kUnitySpeedPermille, 48000);
        CHECK(clock.source_frame_after(0) == 1000);
        CHECK(clock.source_frame_after(48000) == 1000 + 44100);
        // Ten hours at 48 kHz: still exact, no overflow.
        const std::int64_t ten_hours = std::int64_t{48000} * 3600 * 10;
        CHECK(clock.source_frame_after(ten_hours) == 1000 + std::int64_t{44100} * 3600 * 10);

        const detail::SourceClock double_speed(0, 48000, 2000, 48000);
        CHECK(double_speed.source_frame_after(12345) == 24690);
        for (std::int64_t source = 0; source < 5000; source += 7) {
            const std::int64_t output = clock.output_frames_reaching(source);
            CHECK(clock.source_frame_after(output) - clock.anchor_frame() >= source);
            if (output > 0) {
                CHECK(clock.source_frame_after(output - 1) - clock.anchor_frame() < source);
            }
        }
    }

    TEST_CASE("LoopFold wraps positions past the loop end and leaves others alone") {
        const detail::LoopFold fold{1000, 5800};
        CHECK(fold.is_active());
        CHECK(fold.fold(500) == 500);  // before the loop: unchanged
        CHECK(fold.fold(5799) == 5799);
        CHECK(fold.fold(5800) == 1000);  // the end wraps to the start
        CHECK(fold.fold(13000) == 3400);
        const detail::LoopFold inactive{};
        CHECK_FALSE(inactive.is_active());
        CHECK(inactive.fold(123456) == 123456);
    }

    TEST_CASE("LinearRamp lands exactly on its target in a bounded number of steps") {
        detail::LinearRamp ramp(0.0f, detail::LinearRamp::step_for(48000, 5.0));
        ramp.set_target(1.0f);
        int steps = 0;
        while (!ramp.is_settled()) {
            ramp.next();
            ++steps;
        }
        CHECK(ramp.value() == 1.0f);
        CHECK(steps >= 240);  // 5 ms at 48 kHz; float rounding may need one more step to land
        CHECK(steps <= 241);
        ramp.set_target(0.25f);
        for (int step = 0; step < 1000; ++step) {
            ramp.next();
        }
        CHECK(ramp.value() == 0.25f);
        ramp.restart_from(0.0f);  // value moves, target stays
        CHECK(ramp.value() == 0.0f);
        CHECK(ramp.target() == 0.25f);
        ramp.jump_to(0.5f);
        CHECK(ramp.is_settled());
        CHECK(ramp.next() == 0.5f);
    }

    TEST_CASE("WakeupSignal collapses notifications and times out without one") {
        detail::WakeupSignal signal;
        CHECK_FALSE(signal.wait_for(1ms));
        signal.notify();
        signal.notify();
        signal.notify();
        CHECK(signal.wait_for(0ms));
        CHECK_FALSE(signal.wait_for(0ms));  // three notifications, one wake

        std::jthread notifier([&] { signal.notify(); });
        CHECK(signal.wait_for(kDeadline));
    }

    TEST_CASE("StereoDownmix folds mono, stereo and 5.1 peak-safely") {
        const detail::StereoDownmix mono(1);
        CHECK(mono.gain(0, 0) == 1.0f);
        CHECK(mono.gain(0, 1) == 1.0f);

        const detail::StereoDownmix stereo(2);
        CHECK(stereo.gain(0, 0) == 1.0f);
        CHECK(stereo.gain(0, 1) == 0.0f);
        CHECK(stereo.gain(1, 1) == 1.0f);

        const detail::StereoDownmix surround(6);  // FL FR FC LFE BL BR
        float left_sum = 0.0f;
        float right_sum = 0.0f;
        for (std::size_t channel = 0; channel < 6; ++channel) {
            left_sum += surround.gain(channel, 0);
            right_sum += surround.gain(channel, 1);
        }
        CHECK(left_sum <= 1.0f + 1e-6f);
        CHECK(right_sum == doctest::Approx(left_sum));      // balanced image
        CHECK(surround.gain(3, 0) == 0.0f);                 // LFE dropped
        CHECK(surround.gain(2, 0) == surround.gain(2, 1));  // centre in the middle
        CHECK(surround.gain(0, 0) > surround.gain(4, 0));   // fronts above surrounds
    }

    TEST_CASE("PcmRing stays under its cap, keeps FIFO order and recycles blocks") {
        detail::PcmRing huge(std::size_t{1} << 30);
        CHECK(huge.capacity_bytes() <= detail::kPcmRingMaximumBytes);
        CHECK(huge.block_capacity() == detail::PcmRing::maximum_block_count());

        detail::PcmRing ring(4 * detail::kPcmBlockFrames);
        REQUIRE(ring.block_capacity() == 4);
        for (std::uint64_t epoch = 1; epoch <= 4; ++epoch) {
            auto block = ring.begin_write();
            REQUIRE(block.has_value());
            CHECK(block->header->epoch == 0);  // headers start from defaults
            block->header->epoch = epoch;
            block->header->frame_count = 3;
            ring.commit_write();
        }
        CHECK_FALSE(ring.begin_write().has_value());  // full
        CHECK(ring.readable_blocks() == 4);
        for (std::uint64_t epoch = 1; epoch <= 4; ++epoch) {
            const auto block = ring.front();
            REQUIRE(block.has_value());
            CHECK(block->header->epoch == epoch);
            CHECK(block->samples.size() == 3 * detail::kOutputChannels);
            ring.pop_front();
        }
        CHECK_FALSE(ring.front().has_value());
        CHECK(ring.begin_write().has_value());  // released blocks are writable again
    }

    TEST_CASE("PlayheadHistory reports the position the latency ago") {
        detail::PlayheadHistory history;
        CHECK_FALSE(history.audible(0, 0).has_value());
        for (std::uint64_t frame = 0; frame <= 1000; frame += 100) {
            history.record({frame, 1, static_cast<std::int64_t>(frame) * 2});
        }
        CHECK(history.audible(1000, 0)->position == 2000);
        CHECK(history.audible(1000, 250)->position == 1400);  // newest point at or before 750 is 700
        CHECK_FALSE(history.audible(1000, 2000).has_value());
    }

    TEST_CASE("PlayheadHistory carries the last heard position over to a new device") {
        detail::PlayheadHistory history;
        history.record({100, 1, 1000});
        history.record({200, 1, 1100});
        const auto heard = history.audible(250, 100);  // the point recorded at 100
        REQUIRE(heard.has_value());
        history.restart(heard);
        CHECK(history.audible(10, 480)->position == 1000);  // the new device has played nothing yet
        history.record({300, 2, 5000});
        CHECK(history.audible(400, 480)->position == 1000);  // its own point is still in its buffer
        CHECK(history.audible(800, 480)->position == 5000);  // and then it is heard
        history.restart(std::nullopt);
        CHECK_FALSE(history.audible(800, 480).has_value());
    }

    TEST_CASE("OutputRetrySchedule doubles the wait after each failure up to its cap") {
        const detail::OutputRetrySchedule schedule{500ms, 4000ms};
        CHECK(schedule.delay_after(0) == 500ms);
        CHECK(schedule.delay_after(1) == 1000ms);
        CHECK(schedule.delay_after(2) == 2000ms);
        CHECK(schedule.delay_after(3) == 4000ms);
        CHECK(schedule.delay_after(4) == 4000ms);
        CHECK(schedule.delay_after(1'000'000) == 4000ms);
        const detail::OutputRetrySchedule inverted{800ms, 100ms};  // a cap below the first wait
        CHECK(inverted.delay_after(0) == 800ms);
        CHECK(inverted.delay_after(5) == 800ms);
    }

#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)
    TEST_CASE("ScopedDenormalFlush flushes denormals to zero and restores the mode") {
        volatile float tiny = std::numeric_limits<float>::min();  // smallest normal float
        {
            const detail::ScopedDenormalFlush flushed;
            const float halved = tiny * 0.5f;  // a denormal result
            CHECK(halved == 0.0f);
        }
        const float halved = tiny * 0.5f;
        CHECK(halved > 0.0f);
    }
#endif

    // ---------------------------------------------------------------- command mailbox

    TEST_CASE("CommandMailbox coalesces consecutive values and keeps commands in order") {
        detail::CommandMailbox mailbox;
        for (std::int64_t target = 0; target < 100; ++target) {
            CHECK(mailbox.post(transport::SeekToFrame{target}));
        }
        CHECK(mailbox.post(transport::Play{}));
        CHECK(mailbox.post(transport::SetVolumeDecibels{-6.0206}));
        CHECK(mailbox.post(transport::SetVolumeLinear{0.25}));
        CHECK(mailbox.post(transport::SetLoop{LoopRegion{10, 20}}));
        CHECK(mailbox.post(transport::SetSeamFades{true}));
        CHECK(mailbox.post(transport::SetPlaybackMode{PlaybackMode::IndependentTempoPitch}));
        CHECK(mailbox.post(transport::SetPitch{50.0}));  // clamped to +24
        CHECK_FALSE(mailbox.post(transport::SetSpeed{std::numeric_limits<double>::quiet_NaN()}));

        auto seek = mailbox.take();
        REQUIRE(seek.has_value());
        CHECK(std::get<transport::SeekToFrame>(*seek).source_frame == 99);
        CHECK(std::holds_alternative<transport::Play>(*mailbox.take()));
        CHECK(std::get<transport::SetVolumeLinear>(*mailbox.take()).gain == doctest::Approx(0.25));
        CHECK(std::get<transport::SetLoop>(*mailbox.take()).region == LoopRegion{10, 20});
        CHECK(std::get<transport::SetSeamFades>(*mailbox.take()).enabled);
        CHECK(std::get<transport::SetPlaybackMode>(*mailbox.take()).mode == PlaybackMode::IndependentTempoPitch);
        CHECK(std::get<transport::SetPitch>(*mailbox.take()).semitones == kMaximumPitchSemitones);
        CHECK_FALSE(mailbox.take().has_value());
        CHECK(mailbox.dropped_count() == 1);

        CHECK(mailbox.post(transport::SeekToFraction{0.5}));
        CHECK(std::get<transport::SeekToFraction>(*mailbox.take()).fraction == doctest::Approx(0.5));
    }

    TEST_CASE("CommandMailbox always accepts Stop and delivers it between its neighbours") {
        detail::CommandMailbox mailbox;
        std::size_t accepted = 0;
        while (mailbox.post(accepted % 2 == 0 ? TransportCommand{transport::Play{}}
                                              : TransportCommand{transport::Pause{}})) {
            ++accepted;
        }
        CHECK(accepted >= detail::CommandMailbox::kGeneralCapacity);
        CHECK(mailbox.post(transport::Stop{}));  // the queue is full, Stop still fits
        for (std::size_t command = 0; command < accepted; ++command) {
            const auto taken = mailbox.take();
            REQUIRE(taken.has_value());
            CHECK_FALSE(std::holds_alternative<transport::Stop>(*taken));
        }
        CHECK(std::holds_alternative<transport::Stop>(*mailbox.take()));
        CHECK_FALSE(mailbox.take().has_value());

        // Stop, Play: Play comes second. Play, Stop: Stop comes second.
        CHECK(mailbox.post(transport::Stop{}));
        CHECK(mailbox.post(transport::Play{}));
        CHECK(std::holds_alternative<transport::Stop>(*mailbox.take()));
        CHECK(std::holds_alternative<transport::Play>(*mailbox.take()));
        CHECK(mailbox.post(transport::Play{}));
        CHECK(mailbox.post(transport::Stop{}));
        CHECK(std::holds_alternative<transport::Play>(*mailbox.take()));
        CHECK(std::holds_alternative<transport::Stop>(*mailbox.take()));
    }

    TEST_CASE("CommandMailbox never reorders Stop and a later command under concurrency") {
        // The producer posts Stop, Seek(n) for n = 1..N while the consumer drains concurrently. A
        // Stop delivered after the Seek posted behind it would end the run on a Stop instead of
        // Seek(N), and Seeks must arrive in increasing order.
        detail::CommandMailbox mailbox;
        constexpr std::int64_t kRounds = 20'000;
        std::atomic<bool> producing{true};
        bool stop_always_accepted = true;  // written by the producer, read after it finished
        std::jthread producer([&] {
            for (std::int64_t round = 1; round <= kRounds; ++round) {
                stop_always_accepted = mailbox.post(transport::Stop{}) && stop_always_accepted;
                while (!mailbox.post(transport::SeekToFrame{round})) {
                    std::this_thread::yield();  // full: wait for the consumer
                }
            }
            producing = false;
        });
        std::optional<TransportCommand> last;
        std::int64_t newest_seek = 0;
        bool ordered = true;
        for (;;) {
            const bool finished = !producing.load();  // read before take(): then an empty take is final
            const auto taken = mailbox.take();
            if (!taken) {
                if (finished) {
                    break;
                }
                std::this_thread::yield();
                continue;
            }
            if (const auto* seek = std::get_if<transport::SeekToFrame>(&*taken)) {
                ordered = ordered && seek->source_frame > newest_seek;
                newest_seek = seek->source_frame;
            }
            last = taken;
        }
        CHECK(ordered);
        CHECK(stop_always_accepted);
        REQUIRE(last.has_value());
        REQUIRE(std::holds_alternative<transport::SeekToFrame>(*last));
        CHECK(std::get<transport::SeekToFrame>(*last).source_frame == kRounds);
    }

    // ---------------------------------------------------------------- renderer

    TEST_CASE("Renderer never plays a new epoch under a stale output switch") {
        RendererFixture fixture;
        fixture.write_block(1, 0.5f);
        fixture.publish_gate(1, true);
        const auto playing = fixture.render(128);
        CHECK(playing[0] == 0.5f);  // a fresh epoch starts at full gain: the attack is untouched

        // Stop: the decoder thread publishes "epoch 2, output off" in one store, then writes epoch 2.
        fixture.publish_gate(2, false);
        fixture.write_block(2, 0.25f);
        const auto stopped = fixture.render(512);
        for (std::size_t frame = kSettleFrames; frame < 512; ++frame) {
            REQUIRE(stopped[frame * 2] == 0.0f);
        }
        CHECK(stopped[2 * 2] < 0.5f);                               // the cut decays instead of stepping
        CHECK(fixture.shared.progress.load().consumed_epoch == 1);  // nothing of epoch 2 consumed
    }

    TEST_CASE("Renderer re-reads the gate when it meets a block newer than its epoch") {
        RendererFixture fixture;
        fixture.publish_gate(1, true);
        fixture.write_block(1, 0.5f);
        fixture.render(256);
        // A block of epoch 2 shows up while the renderer still holds epoch 1. It re-reads the gate;
        // until the gate announces epoch 2 the block stays unplayed (and nothing spins).
        fixture.write_block(2, -0.25f);  // negative: never confused with the decay of 0.5
        const auto unannounced = fixture.render(512);
        CHECK(std::none_of(unannounced.begin(), unannounced.end(), [](float sample) { return sample < 0.0f; }));
        // Announced with output off (seek while paused): still nothing is consumed.
        fixture.publish_gate(2, false);
        const auto paused = fixture.render(512);
        CHECK(all_samples_equal(paused, 0.0f));
        CHECK(fixture.shared.progress.load().consumed_epoch == 1);
        // Output on: the fresh epoch plays from its first frame at full gain.
        fixture.publish_gate(2, true);
        const auto playing = fixture.render(128);
        CHECK(playing[0] == -0.25f);
        CHECK(fixture.shared.progress.load().consumed_epoch == 2);
    }

    TEST_CASE("Renderer drops blocks rendered for another device rate without counting underruns") {
        RendererFixture fixture;
        fixture.publish_gate(1, true);
        fixture.write_block(1, 0.5f);
        fixture.render(256);
        fixture.write_block(1, 0.5f, detail::kPcmBlockFrames, 44100);  // rendered for the previous device
        const auto output = fixture.render(1024);
        CHECK(all_samples_equal(std::span(output).subspan(kSettleFrames * 2), 0.0f));
        CHECK(fixture.ring.readable_blocks() == 0);
        CHECK(fixture.shared.underrun_count.load() == 0);
    }

    TEST_CASE("Renderer counts underruns and fades back in after one") {
        RendererFixture fixture;
        fixture.publish_gate(1, true);
        fixture.write_block(1, 0.5f);
        fixture.render(256);  // plays the whole block
        fixture.render(512);  // starves; the cut's decay has finished by the end
        CHECK(fixture.shared.underrun_count.load() == 1);
        fixture.write_block(1, 0.5f, detail::kPcmBlockFrames);
        const auto resumed = fixture.render(256);
        CHECK(resumed[0] < 0.1f);         // re-entering mid-signal fades in...
        CHECK(resumed[255 * 2] == 0.5f);  // ...over 5 ms
    }

    TEST_CASE("Renderer reports the audible position behind the device latency") {
        RendererFixture fixture(480);  // 10 ms device buffer
        fixture.publish_gate(1, true);
        for (std::int64_t block = 0; block < 8; ++block) {
            fixture.write_block(1, 0.5f, block * detail::kPcmBlockFrames);
        }
        fixture.render(256);
        CHECK(fixture.shared.progress.load().audible_epoch == 0);  // nothing has reached the speaker yet
        fixture.render(256);
        fixture.render(256);
        fixture.render(256);
        const detail::PlaybackProgress progress = fixture.shared.progress.load();
        CHECK(progress.consumed_frames == 1024);
        CHECK(progress.audible_epoch == 1);
        CHECK(progress.audible_frame == 512);  // newest callback boundary at least 480 frames back
    }

    TEST_CASE("Renderer declicks an epoch change that falls on a block boundary") {
        RendererFixture fixture;
        fixture.publish_gate(1, true);
        fixture.write_block(1, 0.5f);
        fixture.render(detail::kPcmBlockFrames);  // ends exactly on the block boundary: no block held
        // A seek while playing: new epoch, output still on, a block of another level.
        fixture.publish_gate(2, true);
        fixture.write_block(2, -0.25f);
        const auto output = fixture.render(detail::kPcmBlockFrames);
        // The old level (0.5) decays on top of the new epoch instead of stepping to -0.25. (In the
        // engine the new epoch also fades in, so the sum stays level; see the seek-while-playing test.)
        CHECK(output[0] == doctest::Approx(-0.25f + 0.5f));
        float largest_step = 0.0f;
        for (std::size_t frame = 1; frame < detail::kPcmBlockFrames; ++frame) {
            largest_step = std::max(largest_step, std::abs(output[frame * 2] - output[(frame - 1) * 2]));
        }
        CHECK(largest_step < 0.01f);
        CHECK(output[(detail::kPcmBlockFrames - 1) * 2] == -0.25f);  // decay over: the new epoch at full gain
    }

    TEST_CASE("Renderer reconfigured for a new device drops the old rate and keeps the heard position") {
        RendererFixture fixture(128);
        fixture.publish_gate(1, true);
        for (std::int64_t block = 0; block < 4; ++block) {
            fixture.write_block(1, 0.5f, block * detail::kPcmBlockFrames);
        }
        fixture.render(128);
        fixture.render(192);  // holds the second block, 64 frames in
        REQUIRE(fixture.shared.progress.load().audible_frame == 128);

        fixture.renderer.configure(44100, 4410);  // the device switched to 44.1 kHz
        const auto output = fixture.render(128);
        CHECK(all_samples_equal(output, 0.0f));      // nothing of the 48 kHz blocks, no decay carried over
        CHECK(fixture.ring.readable_blocks() == 0);  // the held block went back with the rest
        CHECK(fixture.shared.underrun_count.load() == 0);
        const detail::PlaybackProgress progress = fixture.shared.progress.load();
        CHECK(progress.audible_epoch == 1);  // still where the listener last heard the old device
        CHECK(progress.audible_frame == 128);
    }

    // ---------------------------------------------------------------- controller

    TEST_CASE("Controller recovers from a lost device paused where it was") {
        detail::PcmRing ring(96'000);
        detail::SharedPlaybackState shared;
        detail::CommandMailbox mailbox;
        detail::WakeupSignal wakeup;
        detail::PlaybackRenderer renderer(ring, shared);
        renderer.configure(kRate, 0);
        detail::OutputFacts facts;
        facts.sample_rate = kRate;
        shared.output_facts.store(facts);
        shared.output_available = true;
        shared.output_configuration = 1;

        detail::PlaybackController controller(ring, shared, mailbox, wakeup, 200);
        CHECK(controller.request_source(std::unique_ptr<media::Decoder>(constant_source(0.5f))) == 1);
        CHECK(shared.requested_generation == 1);
        const auto state_is = [&](PreviewState state) { return eventually([&] { return shared.state == state; }); };
        REQUIRE(state_is(PreviewState::Ready));

        // Ready survives a loss as Ready.
        shared.device_lost = true;
        wakeup.notify();
        REQUIRE(state_is(PreviewState::Error));
        CHECK(shared.last_error.load() == ErrorCode::DeviceLost);
        shared.device_lost = false;
        ++shared.output_configuration;
        wakeup.notify();
        REQUIRE(state_is(PreviewState::Ready));
        CHECK_FALSE(shared.last_error.load().has_value());

        // Playing comes back Paused at the position it had.
        REQUIRE(mailbox.post(transport::Play{}));
        wakeup.notify();
        REQUIRE(state_is(PreviewState::Playing));
        std::vector<float> output(4800 * 2);
        REQUIRE(eventually([&] { return shared.production.load().frames >= 4800; }));
        renderer.render(output);
        REQUIRE(shared.progress.load().audible_frame == 4800);

        shared.device_lost = true;
        wakeup.notify();
        REQUIRE(state_is(PreviewState::Error));
        shared.device_lost = false;
        ++shared.output_configuration;
        wakeup.notify();
        REQUIRE(state_is(PreviewState::Paused));
        CHECK(shared.epoch_start_frame.load() == 4800);  // never silently back to the start
        REQUIRE(mailbox.post(transport::Play{}));
        wakeup.notify();
        REQUIRE(state_is(PreviewState::Playing));
    }

    TEST_CASE("Controller re-anchors shape changes at the consumed position and device changes at the audible one") {
        detail::PcmRing ring(96'000);
        detail::SharedPlaybackState shared;
        detail::CommandMailbox mailbox;
        detail::WakeupSignal wakeup;
        detail::PlaybackRenderer renderer(ring, shared);
        renderer.configure(kRate, 2400);  // 50 ms device buffer
        detail::OutputFacts facts;
        facts.sample_rate = kRate;
        shared.output_facts.store(facts);
        shared.output_available = true;
        shared.output_configuration = 1;

        detail::PlaybackController controller(ring, shared, mailbox, wakeup, 200);
        controller.request_source(std::unique_ptr<media::Decoder>(constant_source(0.5f)));
        REQUIRE(eventually([&] { return shared.state == PreviewState::Ready; }));
        REQUIRE(mailbox.post(transport::Play{}));
        wakeup.notify();
        REQUIRE(eventually([&] { return shared.state == PreviewState::Playing; }));

        // Renders `callbacks` periods of 2400 frames, each once the decoder thread has produced it.
        std::vector<float> period(2400 * 2);
        const auto render_periods = [&](int callbacks) {
            for (int callback = 0; callback < callbacks; ++callback) {
                const std::uint64_t epoch = detail::TransportGate::unpacked(shared.transport_gate.load()).epoch;
                const std::int64_t consumed =
                    shared.progress.load().consumed_epoch == epoch ? shared.progress.load().consumed_frames : 0;
                REQUIRE(eventually([&] {
                    const detail::EpochProduction production = shared.production.load();
                    return production.epoch == epoch && production.frames >= consumed + 2400;
                }));
                renderer.render(period);
            }
        };
        render_periods(4);
        REQUIRE(shared.progress.load().consumed_position == 9600);
        REQUIRE(shared.progress.load().audible_frame == 7200);  // the last 2400 frames are still in the device

        // A speed change continues right after the audio already in the device: nothing is played twice.
        REQUIRE(mailbox.post(transport::SetSpeed{2.0}));
        wakeup.notify();
        REQUIRE(eventually([&] { return shared.settings.load().speed_ratio == 2.0; }));
        CHECK(shared.epoch_start_frame.load() == 9600);

        // A new device does not play the old one's buffer: it resumes from what was heard.
        render_periods(2);  // 4800 frames at 2x: consumed 19,200, heard 14,400
        REQUIRE(shared.progress.load().consumed_position == 19200);
        REQUIRE(shared.progress.load().audible_frame == 14400);
        ++shared.output_configuration;
        wakeup.notify();
        REQUIRE(eventually([&] { return shared.epoch_start_frame.load() == 14400; }));
        CHECK(shared.state == PreviewState::Playing);
    }

    TEST_CASE("OutputSupervisor re-opens a lost null device by itself, and restart resumes playback") {
        detail::PcmRing ring(96'000);
        detail::SharedPlaybackState shared;
        detail::CommandMailbox mailbox;
        detail::WakeupSignal decoder_wakeup;
        detail::PlaybackRenderer renderer(ring, shared);
        detail::OutputSupervisorOptions options;
        options.backend = OutputBackend::Null;
        options.retry = detail::OutputRetrySchedule{200ms, 400ms};
        auto supervisor = detail::OutputSupervisor::start(options, renderer, shared, decoder_wakeup);
        REQUIRE(supervisor.has_value());
        CHECK(shared.output_available);
        CHECK_FALSE(shared.device_lost);
        detail::PlaybackController controller(ring, shared, mailbox, decoder_wakeup, 200);

        const auto state_is = [&](PreviewState state) { return eventually([&] { return shared.state == state; }); };
        controller.request_source(std::unique_ptr<media::Decoder>(constant_source(0.25f)));
        REQUIRE(state_is(PreviewState::Ready));
        REQUIRE(mailbox.post(transport::Play{}));
        decoder_wakeup.notify();
        REQUIRE(state_is(PreviewState::Playing));
        REQUIRE(eventually([&] { return shared.progress.load().audible_frame > kRate / 20; }));

        // Unplugged: the endpoint's stop notification reports the loss.
        const std::uint64_t configuration = shared.output_configuration;
        (*supervisor)->on_device_lost();
        CHECK(shared.device_lost);
        REQUIRE(state_is(PreviewState::Error));
        CHECK(shared.last_error.load() == ErrorCode::DeviceLost);
        // The output thread re-opens an endpoint after the first retry delay; playback waits, paused
        // where it was heard.
        REQUIRE(eventually([&] { return !shared.device_lost && shared.state == PreviewState::Paused; }));
        CHECK(shared.output_configuration > configuration);
        CHECK(shared.epoch_start_frame.load() > 0);
        CHECK_FALSE(shared.last_error.load().has_value());

        // Lost again, then "Retry": the transport comes back exactly as it was, playing.
        REQUIRE(mailbox.post(transport::Play{}));
        decoder_wakeup.notify();
        REQUIRE(state_is(PreviewState::Playing));
        (*supervisor)->on_device_lost();
        REQUIRE(state_is(PreviewState::Error));
        REQUIRE((*supervisor)->restart().has_value());
        REQUIRE(state_is(PreviewState::Playing));
        CHECK_FALSE(shared.device_lost);
        CHECK_FALSE(shared.resume_on_restore);  // taken with the new configuration

        // A restart of a working endpoint keeps playing; a selection that cannot open changes nothing.
        REQUIRE((*supervisor)->restart().has_value());
        const auto missing = (*supervisor)->select(OutputDeviceId{"null:does-not-exist"});
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == ErrorCode::NotFound);
        CHECK_FALSE(shared.device_lost);
        const std::int64_t heard = shared.progress.load().audible_frame;
        REQUIRE(eventually([&] { return shared.progress.load().audible_frame > heard; }));
        CHECK(shared.state == PreviewState::Playing);
        supervisor->reset();  // the device stops before the controller and renderer go away
    }
}

// ================================================================ engine end to end

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

    TEST_CASE("a 44.1 kHz device plays a 48 kHz source at the right rate") {
        auto engine = make_offline_engine(44100);
        CHECK(engine->snapshot().output_sample_rate == 44100);
        open_and_wait_until_ready(*engine, constant_source(0.5f));
        play(*engine);
        pull(*engine, 44100);
        CHECK(engine->snapshot().position_frames == kRate);
    }

    TEST_CASE("play from the start leaves the attack untouched") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, ramp_source());
        engine->post(transport::SetVolumeLinear{1.0});
        play(*engine);
        const std::vector<float> output = pull(*engine, 1024);
        for (std::size_t frame = 0; frame < 1024; ++frame) {
            REQUIRE(output[frame * 2] == ramp_value(static_cast<std::int64_t>(frame)));
        }
    }

    TEST_CASE("a seek into the middle of the signal fades in over 5 ms") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(0.5f));
        engine->post(transport::SeekToFrame{24000});
        wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.position_frames == 24000 && snapshot.buffered_milliseconds >= 100;
        });
        play(*engine);
        const std::vector<float> output = pull(*engine, 1024);
        CHECK(output[0] == 0.0f);
        CHECK(output[120 * 2] > 0.2f);
        CHECK(output[120 * 2] < 0.3f);
        CHECK(output[240 * 2] == 0.5f);
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
        CHECK(device.output_latency_microseconds > 0);

        open_and_wait_until_ready(engine, constant_source(0.25f));
        play(engine);
        const auto start = std::chrono::steady_clock::now();
        wait_until(engine, [](const PreviewSnapshot& snapshot) { return snapshot.position_frames >= kRate / 10; });
        const auto elapsed = std::chrono::steady_clock::now() - start;
        CHECK(elapsed >= 50ms);  // paced like a device, not as fast as the CPU allows
    }

    TEST_CASE("select_output_device switches endpoints and keeps the old one on failure") {
        PreviewOptions options;
        options.backend = OutputBackend::Null;
        auto created = PreviewEngine::create(options);
        REQUIRE(created.has_value());
        PreviewEngine& engine = **created;
        auto devices = engine.list_output_devices();
        REQUIRE(devices.has_value());
        REQUIRE_FALSE(devices->empty());
        CHECK(devices->front().id.value.starts_with("null:"));

        open_and_wait_until_ready(engine, constant_source(0.25f));
        play(engine);
        wait_until(engine, [](const PreviewSnapshot& snapshot) { return snapshot.position_frames > 0; });

        const auto failed = engine.select_output_device(OutputDeviceId{"null:does-not-exist"});
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().code == ErrorCode::NotFound);
        const std::int64_t before = engine.snapshot().position_frames;
        wait_until(engine, [before](const PreviewSnapshot& snapshot) {
            return snapshot.output_device_available && snapshot.position_frames > before;  // still playing
        });

        REQUIRE(engine.select_output_device(devices->front().id).has_value());
        const std::int64_t switched_at = engine.snapshot().position_frames;
        CHECK(switched_at > 0);  // never restarts from the beginning
        wait_until(engine, [switched_at](const PreviewSnapshot& snapshot) {
            return snapshot.state == PreviewState::Playing && snapshot.position_frames > switched_at;
        });

        auto offline = make_offline_engine();
        CHECK(offline->select_output_device(OutputDeviceId{}).error().code == ErrorCode::InvalidArgument);
        CHECK(offline->list_output_devices()->empty());
    }

    TEST_CASE("a seek while playing never steps the output") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(0.5f));
        play(*engine);
        const std::vector<float> before = pull(*engine, 19 * detail::kPcmBlockFrames);  // ends on a block boundary
        REQUIRE(before[before.size() - 2] == 0.5f);

        engine->post(transport::SeekToFrame{24000});
        wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.position_frames == 24000 && snapshot.buffered_milliseconds >= 100;
        });
        const std::vector<float> after = pull(*engine, 1024);
        float largest_step = std::abs(after[0] - 0.5f);
        for (std::size_t frame = 1; frame < 1024; ++frame) {
            largest_step = std::max(largest_step, std::abs(after[frame * 2] - after[(frame - 1) * 2]));
        }
        CHECK(largest_step < 0.01f);  // the old level decays while the new one fades in
        CHECK(after[1023 * 2] == 0.5f);
        CHECK(engine->snapshot().position_frames == 24000 + 1024);
    }

    TEST_CASE("a loop past an inexact end shrinks to the real end and keeps the minimum length") {
        auto engine = make_offline_engine();
        auto source = constant_source(0.5f, 2, kRate, kRate);  // really one second long...
        source->estimating_length(2 * kRate);                  // ...estimated at two
        open_and_wait_until_ready(*engine, std::move(source));
        engine->post(transport::SetLoop{LoopRegion{kRate - 100, 2 * kRate - 1000}});
        engine->post(transport::SeekToFrame{kRate - 100});
        const auto shrunk = wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.loop.has_value() && snapshot.loop->end_frame == kRate;
        });
        constexpr std::int64_t kMinimumLoopFrames = kRate * kMinimumLoopMilliseconds / 1000;
        CHECK(shrunk.loop == LoopRegion{kRate - kMinimumLoopFrames, kRate});
    }

    TEST_CASE("a request that fails while opening never shows the next request as failed") {
        auto engine = make_offline_engine();
        const auto failing_gate = std::make_shared<TestGate>();
        const auto next_gate = std::make_shared<TestGate>();
        REQUIRE(engine->open(std::make_unique<GatedOpeningDecoder>(failing_gate, true)).has_value());
        REQUIRE(eventually([&] { return failing_gate->reached.load(); }));
        REQUIRE(engine->open(std::make_unique<GatedOpeningDecoder>(next_gate, false)).has_value());

        failing_gate->release();  // the first request throws while it is being opened
        REQUIRE(eventually([&] { return next_gate->reached.load(); }));
        const PreviewSnapshot opening = engine->snapshot();
        CHECK(opening.generation == 2);
        CHECK(opening.state == PreviewState::Loading);  // never Error: the failure belongs to generation 1
        CHECK_FALSE(opening.last_error.has_value());

        next_gate->release();
        const auto ready =
            wait_until(*engine, [](const PreviewSnapshot& snapshot) { return snapshot.state == PreviewState::Ready; });
        CHECK(ready.generation == 2);
        CHECK_FALSE(ready.last_error.has_value());
    }

    TEST_CASE("a null engine restarts its output device on request and keeps playing") {
        PreviewOptions options;
        options.backend = OutputBackend::Null;
        auto created = PreviewEngine::create(options);
        REQUIRE(created.has_value());
        PreviewEngine& engine = **created;
        CHECK_FALSE(engine.snapshot().output_device_lost);

        open_and_wait_until_ready(engine, constant_source(0.25f));
        play(engine);
        wait_until(engine, [](const PreviewSnapshot& snapshot) { return snapshot.position_frames > 0; });
        REQUIRE(engine.restart_output_device().has_value());
        const std::int64_t restarted_at = engine.snapshot().position_frames;
        CHECK(restarted_at > 0);  // never back to the beginning
        const PreviewSnapshot after = wait_until(engine, [restarted_at](const PreviewSnapshot& snapshot) {
            return snapshot.state == PreviewState::Playing && snapshot.position_frames > restarted_at;
        });
        CHECK(after.output_device_available);
        CHECK_FALSE(after.output_device_lost);

        auto offline = make_offline_engine();
        CHECK(offline->restart_output_device().error().code == ErrorCode::InvalidArgument);
        CHECK_FALSE(offline->snapshot().output_device_lost);
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
        const std::vector<float> resumed = pull(*engine, 4800);
        CHECK(resumed[0] < 0.01f);  // resuming mid-signal fades back in
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
        CHECK(all_samples_equal(first, 0.5f));  // from frame 0: no fade at all
        const std::uint64_t first_generation = engine->snapshot().generation;

        // Plenty of the old source is still buffered when the new one is opened.
        open_and_wait_until_ready(*engine, constant_source(-0.25f));
        CHECK(engine->snapshot().generation == first_generation + 1);
        CHECK(engine->snapshot().position_frames == 0);
        play(*engine);
        const std::vector<float> second = pull(*engine, 4800);
        CHECK(all_samples_equal(std::span(second).subspan(kSettleFrames * 2), -0.25f));
    }

    TEST_CASE("a loop wraps at the region end with an exact, unfaded seam") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, ramp_source());
        engine->post(transport::SetLoop{LoopRegion{1000, 5800}});
        engine->post(transport::SeekToFrame{1000});
        const auto ready = wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.position_frames == 1000 && snapshot.buffered_milliseconds >= 100;
        });
        CHECK(ready.loop == LoopRegion{1000, 5800});
        CHECK_FALSE(ready.seam_fades);
        play(*engine);

        const std::vector<float> output = pull(*engine, 12000);  // 2.5 passes through the 4800-frame region

        // 1000 + 12000 frames unfolded = 13000, folded back into [1000, 5800).
        CHECK(engine->snapshot().position_frames == 3400);
        // After the seek's fade-in every frame is the source frame, seams included.
        for (std::size_t frame = kSettleFrames; frame < 12000; ++frame) {
            REQUIRE(output[frame * 2] == ramp_value(1000 + static_cast<std::int64_t>(frame) % 4800));
        }
    }

    TEST_CASE("seam fades are opt-in and reported") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(0.5f));
        engine->post(transport::SetSeamFades{true});
        engine->post(transport::SetLoop{LoopRegion{0, 4800}});
        const auto ready = wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.seam_fades && snapshot.loop.has_value() && snapshot.buffered_milliseconds >= 100;
        });
        CHECK(ready.loop == LoopRegion{0, 4800});
        play(*engine);
        const std::vector<float> output = pull(*engine, 9600);
        CHECK(output[4799 * 2] < 0.01f);  // faded out just before the seam...
        CHECK(output[4800 * 2] == 0.0f);  // ...and back in after it
        CHECK(output[2400 * 2] == 0.5f);  // the middle of the loop is untouched
    }

    TEST_CASE("loops shorter than the minimum are lengthened") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(0.5f));
        engine->post(transport::SetLoop{LoopRegion{1000, 1010}});
        const auto looped =
            wait_until(*engine, [](const PreviewSnapshot& snapshot) { return snapshot.loop.has_value(); });
        CHECK(looped.loop == LoopRegion{1000, 1000 + kRate * kMinimumLoopMilliseconds / 1000});

        engine->post(transport::SetLoop{LoopRegion{10 * kRate - 5, 10 * kRate + 100}});  // past the end
        const auto clamped = wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.loop && snapshot.loop->end_frame == 10 * kRate;
        });
        CHECK(clamped.loop->end_frame - clamped.loop->start_frame == kRate * kMinimumLoopMilliseconds / 1000);

        engine->post(transport::SetLoop{std::nullopt});
        wait_until(*engine, [](const PreviewSnapshot& snapshot) { return !snapshot.loop.has_value(); });
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
        for (std::size_t frame = 0; frame < 4800; ++frame) {
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
        for (const float sample : output) {
            REQUIRE(sample <= 1.0f);
            REQUIRE(sample > 0.99f);
        }
    }

    TEST_CASE("prebuffering fills to the target and stays within the 250 ms bound") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, constant_source(0.5f));
        const auto filled =
            wait_until(*engine, [](const PreviewSnapshot& snapshot) { return snapshot.buffered_milliseconds >= 195; });
        CHECK(filled.buffered_milliseconds <= 250);
        wait_for_commands(*engine);  // the decoder thread has run further passes meanwhile
        CHECK(engine->snapshot().buffered_milliseconds <= 250);
    }

    TEST_CASE("a starved callback counts an underrun and recovers") {
        auto gate = std::make_shared<std::atomic<bool>>(false);
        auto engine = make_offline_engine();
        auto source = constant_source(0.5f);
        source->gated_at(kRate / 2, gate);
        open_and_wait_until_ready(*engine, std::move(source));
        play(*engine);
        std::vector<float> chunk(256 * 2);
        // Without waiting for the decoder thread, the callback runs dry at the gate.
        const bool starved = eventually([&] {
            REQUIRE(engine->render_offline(chunk).has_value());
            return engine->snapshot().underrun_count > 0;
        });
        *gate = true;
        gate->notify_all();
        REQUIRE(starved);
        CHECK(engine->snapshot().state == PreviewState::Playing);
        wait_until(*engine, [](const PreviewSnapshot& snapshot) { return snapshot.buffered_milliseconds >= 100; });
        const std::vector<float> resumed = pull(*engine, 4800);
        CHECK(resumed[(4800 - 1) * 2] == 0.5f);
    }

    TEST_CASE("a source that vanishes mid-play stops, flushes and reports FILE_MISSING") {
        auto engine = make_offline_engine();
        auto source = constant_source(0.5f);
        source->failing_at(kRate / 2);
        open_and_wait_until_ready(*engine, std::move(source));
        play(*engine);
        pull(*engine, kRate);

        const auto failed =
            wait_until(*engine, [](const PreviewSnapshot& snapshot) { return snapshot.state == PreviewState::Error; });
        CHECK(failed.last_error == ErrorCode::FileMissing);
        CHECK(failed.buffered_milliseconds == 0);
        const std::vector<float> after = pull(*engine, 4800);
        CHECK(all_samples_equal(std::span(after).subspan(kSettleFrames * 2), 0.0f));

        engine->post(transport::Play{});  // replay is disabled until a new open()
        wait_for_commands(*engine);
        CHECK(engine->snapshot().state == PreviewState::Error);
    }

    TEST_CASE("an exception thrown by a decoder fails the source instead of the process") {
        auto engine = make_offline_engine();
        auto source = constant_source(0.5f);
        source->throwing_at(kRate / 2);
        open_and_wait_until_ready(*engine, std::move(source));
        play(*engine);
        pull(*engine, kRate);
        const auto failed =
            wait_until(*engine, [](const PreviewSnapshot& snapshot) { return snapshot.state == PreviewState::Error; });
        CHECK(failed.last_error == ErrorCode::Internal);

        open_and_wait_until_ready(*engine, constant_source(0.5f));  // the engine keeps working
        CHECK_FALSE(engine->snapshot().last_error.has_value());
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

    // ---------------------------------------------------------------- independent tempo/pitch

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

    TEST_CASE("seek and loop work in tempo/pitch mode") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, sine_source(kToneHz));
        engine->post(transport::SetPlaybackMode{PlaybackMode::IndependentTempoPitch});
        engine->post(transport::SeekToFrame{24000});
        wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.playback_mode == PlaybackMode::IndependentTempoPitch && snapshot.position_frames == 24000 &&
                   snapshot.buffered_milliseconds >= 100;
        });
        play(*engine);
        pull(*engine, 4800);
        CHECK(engine->snapshot().position_frames == 24000 + 4800);

        engine->post(transport::Pause{});
        engine->post(transport::SetLoop{LoopRegion{12000, 36000}});
        engine->post(transport::SeekToFrame{12000});
        wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.state == PreviewState::Paused && snapshot.position_frames == 12000 &&
                   snapshot.loop == LoopRegion{12000, 36000} && snapshot.buffered_milliseconds >= 100;
        });
        play(*engine);
        const std::vector<float> output = pull(*engine, 30000);
        CHECK(engine->snapshot().position_frames == 12000 + 6000);  // 42000 unfolded, folded into the loop
        CHECK(dominant_frequency(output) == doctest::Approx(kToneHz).epsilon(0.03));
    }

    TEST_CASE("tempo/pitch mode ends exactly at the source end") {
        auto engine = make_offline_engine();
        open_and_wait_until_ready(*engine, sine_source(kToneHz, kRate / 2));
        engine->post(transport::SetPlaybackMode{PlaybackMode::IndependentTempoPitch});
        engine->post(transport::SetSpeed{0.5});
        wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.speed_ratio == 0.5 && snapshot.buffered_milliseconds >= 100;
        });
        play(*engine);
        std::int64_t furthest = 0;
        for (int chunk = 0; chunk < 30 && engine->snapshot().state == PreviewState::Playing; ++chunk) {
            pull(*engine, 4800);
            furthest = std::max(furthest, engine->snapshot().position_frames);
        }
        CHECK(furthest <= kRate / 2);  // the playhead never runs past the last frame
        wait_until(*engine, [](const PreviewSnapshot& snapshot) {
            return snapshot.state == PreviewState::Stopped && snapshot.position_frames == 0;
        });
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
}
