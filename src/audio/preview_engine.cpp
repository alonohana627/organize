#include "mso/audio/preview_engine.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <new>
#include <system_error>
#include <type_traits>

#include "mso/audio/detail/command_mailbox.hpp"
#include "mso/audio/detail/output_supervisor.hpp"
#include "mso/audio/detail/pcm_ring.hpp"
#include "mso/audio/detail/playback_controller.hpp"
#include "mso/audio/detail/playback_renderer.hpp"
#include "mso/audio/detail/shared_playback_state.hpp"
#include "mso/audio/detail/wakeup_signal.hpp"

namespace mso::audio {
namespace {

using detail::CommandMailbox;
using detail::OutputFacts;
using detail::OutputSupervisor;
using detail::OutputSupervisorOptions;
using detail::PcmRing;
using detail::PlaybackController;
using detail::PlaybackRenderer;
using detail::SharedPlaybackState;
using detail::SourceOrigin;
using detail::WakeupSignal;

constexpr std::uint32_t kMinimumPeriodMilliseconds = 20;
constexpr std::uint32_t kMaximumPeriodMilliseconds = 60;
constexpr std::uint32_t kMinimumPrebufferMilliseconds = 20;
constexpr std::uint32_t kMaximumPrebufferMilliseconds = 250;
constexpr std::uint32_t kMinimumOutputRate = 8000;
/// Highest output rate accepted (miniaudio's MA_MAX_SAMPLE_RATE).
constexpr std::uint32_t kMaximumOutputRate = 384000;
/// Rate assumed by the Offline backend when none is given.
constexpr std::uint32_t kFallbackOutputRate = 48000;
/// Limits of the automatic device re-open schedule (PreviewOptions::device_retry_*).
constexpr std::uint32_t kMinimumRetryMilliseconds = 10;
constexpr std::uint32_t kMaximumFirstRetryMilliseconds = 10'000;
constexpr std::uint32_t kMaximumLongestRetryMilliseconds = 60'000;
/// Twice the longest prebuffer at the highest rate: a fresh epoch can prebuffer fully while the
/// callback still holds a superseded one. 192,000 frames, about 1.5 MiB, well under the 8 MiB cap.
constexpr std::size_t kRingFrames = 2 * std::size_t{kMaximumPrebufferMilliseconds} * kMaximumOutputRate / 1000;
constexpr std::string_view kOfflineDeviceName = "Offline renderer";

static_assert(kRingFrames <= 8u * 1024u * 1024u / (2 * sizeof(float)), "the ring must fit its 8 MiB cap");

/// Options with every field clamped to its documented range.
PreviewOptions normalized(PreviewOptions options) {
    options.period_milliseconds =
        std::clamp(options.period_milliseconds, kMinimumPeriodMilliseconds, kMaximumPeriodMilliseconds);
    options.prebuffer_milliseconds =
        std::clamp(options.prebuffer_milliseconds, kMinimumPrebufferMilliseconds, kMaximumPrebufferMilliseconds);
    if (options.sample_rate != 0) {
        options.sample_rate = std::clamp(options.sample_rate, kMinimumOutputRate, kMaximumOutputRate);
    }
    options.device_retry_first_milliseconds =
        std::clamp(options.device_retry_first_milliseconds, kMinimumRetryMilliseconds, kMaximumFirstRetryMilliseconds);
    options.device_retry_longest_milliseconds =
        std::clamp(options.device_retry_longest_milliseconds, options.device_retry_first_milliseconds,
                   kMaximumLongestRetryMilliseconds);
    return options;
}

/// What the output thread needs from the (normalized) engine options.
OutputSupervisorOptions supervisor_options(const PreviewOptions& options) {
    OutputSupervisorOptions supervisor;
    supervisor.backend = options.backend;
    supervisor.device = options.device;
    supervisor.device_settings = detail::OutputDeviceSettings{options.sample_rate, options.period_milliseconds};
    supervisor.retry.first_delay = std::chrono::milliseconds{options.device_retry_first_milliseconds};
    supervisor.retry.longest_delay = std::chrono::milliseconds{options.device_retry_longest_milliseconds};
    return supervisor;
}

/// Runs a device request of the control thread, turning the exceptions that handing it to the
/// output thread can raise into errors.
template <class Request>
std::invoke_result_t<Request&> guarded_device_request(Request request) {
    try {
        return request();
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceLimit, "out of memory handing a request to the audio output thread");
    } catch (const std::exception&) {
        return fail(ErrorCode::Internal, "cannot hand the request to the audio output thread");
    }
}

/// PreviewEngine over miniaudio: wires the shared state, PCM ring, mailbox, renderer, output thread
/// and decoder thread together and assembles snapshots.
class MiniaudioPreviewEngine final : public PreviewEngine {
public:
    /// Allocates the ring and mailbox; may throw std::bad_alloc (create() converts it).
    explicit MiniaudioPreviewEngine(const PreviewOptions& options)
        : options_(options), ring_(kRingFrames), renderer_(ring_, shared_) {}

    ~MiniaudioPreviewEngine() override {
        output_.reset();      // stop the callback and close the backend on the output thread first
        controller_.reset();  // then join the decoder thread
    }

    /// Starts the output thread (or the Offline output), then the decoder thread. An endpoint that
    /// cannot be opened is reported in the snapshot and retried, not fatal.
    Result<void> start() {
        if (options_.backend == OutputBackend::Offline) {
            start_offline_output();
        } else {
            auto output = OutputSupervisor::start(supervisor_options(options_), renderer_, shared_, wakeup_);
            if (!output) {
                return std::unexpected(output.error());
            }
            output_ = std::move(*output);
        }
        controller_ =
            std::make_unique<PlaybackController>(ring_, shared_, mailbox_, wakeup_, options_.prebuffer_milliseconds);
        return {};
    }

    Result<std::vector<OutputDeviceInfo>> list_output_devices() override {
        if (!output_) {
            return std::vector<OutputDeviceInfo>{};
        }
        return guarded_device_request([this] { return output_->list_devices(); });
    }

    Result<void> select_output_device(const OutputDeviceId& device) override {
        if (!output_) {
            return fail(ErrorCode::InvalidArgument, "the offline backend has no output devices");
        }
        return guarded_device_request([this, &device] { return output_->select(device); });
    }

    Result<void> restart_output_device() override {
        if (!output_) {
            return fail(ErrorCode::InvalidArgument, "the offline backend has no output device to restart");
        }
        return guarded_device_request([this] { return output_->restart(); });
    }

    Result<void> open(const std::filesystem::path& file) override {
        if (file.empty()) {
            return fail(ErrorCode::InvalidArgument, "no file to preview");
        }
        return request_source([&file] { return SourceOrigin{file}; });
    }

    Result<void> open(std::unique_ptr<media::Decoder> decoder) override {
        if (!decoder) {
            return fail(ErrorCode::InvalidArgument, "no decoder to preview");
        }
        return request_source([&decoder] { return SourceOrigin{std::move(decoder)}; });
    }

    void post(TransportCommand command) noexcept override {
        // A rejected command is counted in the mailbox and reported as dropped_command_count.
        if (!mailbox_.post(command)) {
            return;
        }
        wakeup_.notify();
    }

    PreviewSnapshot snapshot() const noexcept override;

    Result<void> render_offline(std::span<float> stereo_frames) override {
        if (options_.backend != OutputBackend::Offline) {
            return fail(ErrorCode::InvalidArgument, "render_offline needs the offline backend");
        }
        if (stereo_frames.size() % detail::kOutputChannels != 0) {
            return fail(ErrorCode::InvalidArgument, "stereo output needs an even sample count");
        }
        renderer_.render(stereo_frames);
        return {};
    }

private:
    /// Publishes the Offline backend as the output (no latency: render_offline is the device).
    void start_offline_output() {
        OutputFacts facts;
        facts.sample_rate = options_.sample_rate != 0 ? options_.sample_rate : kFallbackOutputRate;
        facts.name = FixedText<kOutputDeviceNameCapacity>::truncated_from(kOfflineDeviceName);
        renderer_.configure(facts.sample_rate, 0);
        shared_.output_facts.store(facts);
        shared_.output_available.store(true, std::memory_order_release);
        shared_.output_configuration.fetch_add(1, std::memory_order_acq_rel);
    }

    /// Hands the source `make_origin` builds to the decoder thread under the next generation number.
    /// One barrier covers both steps that can throw: building the origin (a path copy) and the
    /// hand-off. Nothing is published when either throws.
    template <class MakeOrigin>
    Result<void> request_source(MakeOrigin make_origin) {
        try {
            controller_->request_source(make_origin());
            return {};
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::ResourceLimit, "out of memory queuing the preview source");
        } catch (const std::system_error&) {
            return fail(ErrorCode::Internal, "cannot hand the source to the decoder thread");
        }
    }

    const PreviewOptions options_;                    ///< normalized options
    SharedPlaybackState shared_;                      ///< status shared by all threads
    PcmRing ring_;                                    ///< decoded audio for the callback
    CommandMailbox mailbox_;                          ///< transport commands for the decoder thread
    WakeupSignal wakeup_;                             ///< wakes the decoder thread
    PlaybackRenderer renderer_;                       ///< the callback's work
    std::unique_ptr<OutputSupervisor> output_;        ///< the output thread; null for the Offline backend
    std::unique_ptr<PlaybackController> controller_;  ///< the decoder thread
};

PreviewSnapshot MiniaudioPreviewEngine::snapshot() const noexcept {
    PreviewSnapshot snapshot;
    const std::uint64_t requested = shared_.requested_generation.load(std::memory_order_acquire);
    const bool loading = requested != shared_.active_generation.load(std::memory_order_acquire);
    snapshot.generation = requested;
    snapshot.state = loading ? PreviewState::Loading : shared_.state.load(std::memory_order_acquire);
    snapshot.last_error = loading ? std::nullopt : shared_.last_error.load();

    const OutputFacts facts = shared_.output_facts.load();
    const bool device_lost = shared_.device_lost.load(std::memory_order_acquire);
    snapshot.output_device_name = facts.name;
    snapshot.output_device_available = shared_.output_available.load(std::memory_order_acquire) && !device_lost;
    snapshot.output_device_lost = device_lost;
    snapshot.output_sample_rate = facts.sample_rate;
    snapshot.output_period_frames = facts.period_frames;
    snapshot.output_period_microseconds = facts.period_microseconds;
    snapshot.output_latency_microseconds = facts.latency_microseconds;

    const detail::PlaybackSettings settings = shared_.settings.load();
    snapshot.volume = shared_.target_volume.load(std::memory_order_relaxed);
    snapshot.playback_mode = settings.mode;
    snapshot.speed_ratio = settings.speed_ratio;
    snapshot.pitch_semitones = settings.pitch_semitones;
    snapshot.seam_fades = settings.seam_fades;
    snapshot.underrun_count = shared_.underrun_count.load(std::memory_order_relaxed);
    snapshot.dropped_command_count = mailbox_.dropped_count();
    if (loading) {
        return snapshot;  // source fields would still describe the previous generation
    }

    if (settings.loop_end_frame > settings.loop_start_frame) {
        snapshot.loop = LoopRegion{settings.loop_start_frame, settings.loop_end_frame};
    }
    snapshot.source_sample_rate = shared_.source_sample_rate.load(std::memory_order_relaxed);
    snapshot.source_channels = shared_.source_channels.load(std::memory_order_relaxed);
    const std::int64_t total = shared_.total_frames.load(std::memory_order_relaxed);
    if (total >= 0) {
        snapshot.total_frames = total;
        snapshot.total_frames_exact = shared_.total_frames_exact.load(std::memory_order_relaxed);
    }

    const std::uint64_t epoch =
        detail::TransportGate::unpacked(shared_.transport_gate.load(std::memory_order_acquire)).epoch;
    const detail::PlaybackProgress progress = shared_.progress.load();
    snapshot.position_frames = progress.audible_epoch == epoch
                                   ? progress.audible_frame
                                   : shared_.epoch_start_frame.load(std::memory_order_relaxed);
    // Production and consumption are each published as an (epoch, count) pair, so a count is only
    // ever subtracted from one of the same epoch.
    const detail::EpochProduction production = shared_.production.load();
    const std::int64_t played = progress.consumed_epoch == production.epoch ? progress.consumed_frames : 0;
    const std::int64_t buffered = std::max<std::int64_t>(0, production.frames - played);
    if (facts.sample_rate != 0) {
        snapshot.buffered_milliseconds = static_cast<std::uint32_t>(buffered * 1000 / std::int64_t{facts.sample_rate});
    }
    return snapshot;
}

}  // namespace

Result<std::unique_ptr<PreviewEngine>> PreviewEngine::create(const PreviewOptions& options) {
    try {
        auto engine = std::make_unique<MiniaudioPreviewEngine>(normalized(options));
        if (auto started = engine->start(); !started) {
            return std::unexpected(started.error());
        }
        return std::unique_ptr<PreviewEngine>(std::move(engine));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceLimit, "out of memory creating the preview engine");
    } catch (const std::system_error&) {
        // std::thread and std::mutex report a failure to get a thread or a lock this way.
        return fail(ErrorCode::ResourceLimit, "cannot start the preview engine's threads");
    } catch (const std::exception&) {
        return fail(ErrorCode::Internal, "cannot create the preview engine");
    }
}

}  // namespace mso::audio
