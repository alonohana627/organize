#include "mso/audio/preview_engine.hpp"

#include <algorithm>
#include <exception>
#include <new>
#include <system_error>

#include "mso/audio/detail/command_mailbox.hpp"
#include "mso/audio/detail/output_device.hpp"
#include "mso/audio/detail/pcm_ring.hpp"
#include "mso/audio/detail/playback_controller.hpp"
#include "mso/audio/detail/playback_renderer.hpp"
#include "mso/audio/detail/shared_playback_state.hpp"
#include "mso/audio/detail/wakeup_signal.hpp"

namespace mso::audio {
namespace {

using detail::CommandMailbox;
using detail::OutputContext;
using detail::OutputDevice;
using detail::OutputDeviceSettings;
using detail::OutputFacts;
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
    return options;
}

/// PreviewEngine over miniaudio: wires the shared state, PCM ring, mailbox, renderer, output device
/// and decoder thread together and assembles snapshots.
class MiniaudioPreviewEngine final : public PreviewEngine {
public:
    /// Allocates the ring and mailbox; may throw std::bad_alloc (create() converts it).
    explicit MiniaudioPreviewEngine(const PreviewOptions& options)
        : options_(options), ring_(kRingFrames), renderer_(ring_, shared_) {}

    ~MiniaudioPreviewEngine() override {
        device_.reset();      // stop the callback first
        controller_.reset();  // then join the decoder thread
    }

    /// Opens the backend and its device (a device failure is reported, not fatal), then starts
    /// the decoder thread.
    Result<void> start() {
        if (options_.backend == OutputBackend::Offline) {
            start_offline_output();
        } else {
            auto context = OutputContext::create(options_.backend);
            if (!context) {
                return std::unexpected(context.error());
            }
            context_ = std::move(*context);
            auto device =
                OutputDevice::open(*context_, options_.device, device_settings(), renderer_, shared_, wakeup_);
            if (!device) {
                mark_device_lost();
            } else {
                // A failed start is recorded as DEVICE_LOST by install_device; the engine stays
                // usable and select_output_device() can retry, so creation still succeeds.
                (void)install_device(std::move(*device));
            }
        }
        controller_ =
            std::make_unique<PlaybackController>(ring_, shared_, mailbox_, wakeup_, options_.prebuffer_milliseconds);
        return {};
    }

    Result<std::vector<OutputDeviceInfo>> list_output_devices() override {
        if (!context_) {
            return std::vector<OutputDeviceInfo>{};
        }
        return context_->list_devices();
    }

    Result<void> select_output_device(const OutputDeviceId& device) override {
        if (!context_) {
            return fail(ErrorCode::InvalidArgument, "the offline backend has no output devices");
        }
        auto opened = OutputDevice::open(*context_, device, device_settings(), renderer_, shared_, wakeup_);
        if (!opened) {
            return std::unexpected(opened.error());  // the current endpoint keeps playing
        }
        // The renderer must never run on two callbacks: stop the old device (keeping it open as a
        // fallback) before the new one starts.
        if (device_) {
            device_->stop();
        }
        shared_.device_lost.store(false, std::memory_order_release);  // no callback runs now
        if (auto started = (*opened)->start(); !started) {
            if (!device_ || !device_->start()) {
                mark_device_lost();
            }
            return started;
        }
        device_ = std::move(*opened);
        publish_device();
        return {};
    }

    Result<void> open(const std::filesystem::path& file) override {
        if (file.empty()) {
            return fail(ErrorCode::InvalidArgument, "no file to preview");
        }
        try {
            return request_source(SourceOrigin{file});
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::ResourceLimit, "out of memory queuing the preview source");
        }
    }

    Result<void> open(std::unique_ptr<media::Decoder> decoder) override {
        if (!decoder) {
            return fail(ErrorCode::InvalidArgument, "no decoder to preview");
        }
        return request_source(SourceOrigin{std::move(decoder)});
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
    /// Device parameters from the options.
    OutputDeviceSettings device_settings() const {
        return OutputDeviceSettings{options_.sample_rate, options_.period_milliseconds};
    }

    /// Publishes the Offline backend as the output (no latency: render_offline is the device).
    void start_offline_output() {
        OutputFacts facts;
        facts.sample_rate = options_.sample_rate != 0 ? options_.sample_rate : kFallbackOutputRate;
        facts.name = FixedText<kOutputDeviceNameCapacity>::truncated_from(kOfflineDeviceName);
        renderer_.configure(facts.sample_rate, 0);
        publish_facts(facts);
    }

    /// Starts an opened device and publishes it; on failure records DEVICE_LOST.
    Result<void> install_device(std::unique_ptr<OutputDevice> device) {
        shared_.device_lost.store(false, std::memory_order_release);  // no device is running now
        if (auto started = device->start(); !started) {
            mark_device_lost();
            return started;
        }
        device_ = std::move(device);
        publish_device();
        return {};
    }

    /// Publishes the open device's facts.
    void publish_device() { publish_facts(device_->facts()); }

    /// Publishes `facts`, then tells the decoder thread to adopt them.
    void publish_facts(const OutputFacts& facts) {
        shared_.output_facts.store(facts);
        shared_.output_available.store(true, std::memory_order_release);
        shared_.output_configuration.fetch_add(1, std::memory_order_acq_rel);
        wakeup_.notify();
    }

    /// Records that no endpoint is usable.
    void mark_device_lost() {
        shared_.output_available.store(false, std::memory_order_release);
        shared_.device_lost.store(true, std::memory_order_release);
        wakeup_.notify();
    }

    /// Hands a new source to the decoder thread under the next generation number.
    Result<void> request_source(SourceOrigin origin) {
        try {
            const std::uint64_t generation = shared_.requested_generation.load(std::memory_order_relaxed) + 1;
            shared_.requested_generation.store(generation, std::memory_order_release);
            controller_->request_source(generation, std::move(origin));
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
    std::unique_ptr<OutputContext> context_;          ///< null for the Offline backend
    std::unique_ptr<OutputDevice> device_;            ///< null while no endpoint is open
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
    snapshot.output_device_name = facts.name;
    snapshot.output_device_available = shared_.output_available.load(std::memory_order_acquire) &&
                                       !shared_.device_lost.load(std::memory_order_acquire);
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
    const std::int64_t produced = shared_.epoch_produced_frames.load(std::memory_order_acquire);
    const std::int64_t played = progress.consumed_epoch == epoch ? progress.consumed_frames : 0;
    const std::int64_t buffered = std::max<std::int64_t>(0, produced - played);
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
    } catch (const std::exception&) {
        return fail(ErrorCode::ResourceLimit, "cannot start the preview decoder thread");
    }
}

}  // namespace mso::audio
