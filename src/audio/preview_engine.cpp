#include "mso/audio/preview_engine.hpp"

#include <algorithm>
#include <new>
#include <system_error>

#include "mso/audio/command_mailbox.hpp"
#include "mso/audio/output_device.hpp"
#include "mso/audio/pcm_ring.hpp"
#include "mso/audio/playback_controller.hpp"
#include "mso/audio/playback_renderer.hpp"
#include "mso/audio/shared_playback_state.hpp"
#include "mso/audio/wakeup_signal.hpp"

namespace mso::audio {
namespace {

constexpr std::uint32_t kMinimumPeriodMilliseconds = 20;
constexpr std::uint32_t kMaximumPeriodMilliseconds = 60;
constexpr std::uint32_t kMinimumPrebufferMilliseconds = 20;
constexpr std::uint32_t kMaximumPrebufferMilliseconds = 250;
constexpr std::uint32_t kMinimumOutputRate = 8000;
constexpr std::uint32_t kMaximumOutputRate = 384000;
/// Rate assumed by the Offline backend (and for production while no device is open).
constexpr std::uint32_t kFallbackOutputRate = 48000;
/// Twice the longest prebuffer at 192 kHz: a fresh epoch can prebuffer fully while the callback
/// still holds a superseded one (768 KiB, well under the 8 MiB cap).
constexpr std::size_t kRingFrames = 2 * std::size_t{kMaximumPrebufferMilliseconds} * 192'000 / 1000;
constexpr std::string_view kOfflineDeviceName = "Offline renderer";

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
    /// Allocates the ring; may throw std::bad_alloc (create() converts it).
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
            if (device) {
                (void)install_device(std::move(*device));  // a failed start is recorded as DEVICE_LOST
            } else {
                mark_device_lost();
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
            return std::unexpected(opened.error());  // keep using the current endpoint
        }
        // The renderer must never run on two callbacks: the old device stops before the new starts.
        device_.reset();
        return install_device(std::move(*opened));
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
        mailbox_.post(command);
        wakeup_.notify();
    }

    PreviewSnapshot snapshot() const noexcept override;

    Result<void> render_offline(std::span<float> stereo_frames) override {
        if (options_.backend != OutputBackend::Offline) {
            return fail(ErrorCode::InvalidArgument, "render_offline needs the offline backend");
        }
        if (stereo_frames.size() % kOutputChannels != 0) {
            return fail(ErrorCode::InvalidArgument, "stereo output needs an even sample count");
        }
        renderer_.render(stereo_frames);
        return {};
    }

private:
    OutputDeviceSettings device_settings() const {
        return OutputDeviceSettings{options_.sample_rate, options_.period_milliseconds};
    }

    void start_offline_output() {
        const std::uint32_t rate = options_.sample_rate != 0 ? options_.sample_rate : kFallbackOutputRate;
        renderer_.configure(rate);
        shared_.output_sample_rate.store(rate, std::memory_order_relaxed);
        shared_.output_device_name.store(kOfflineDeviceName);
        shared_.output_available.store(true, std::memory_order_release);
        shared_.output_configuration.fetch_add(1, std::memory_order_acq_rel);
    }

    /// Starts an opened device and publishes it; on failure records DEVICE_LOST.
    Result<void> install_device(std::unique_ptr<OutputDevice> device) {
        shared_.device_lost.store(false, std::memory_order_release);  // no device is running now
        if (auto started = device->start(); !started) {
            mark_device_lost();
            return started;
        }
        device_ = std::move(device);
        shared_.output_sample_rate.store(device_->sample_rate(), std::memory_order_relaxed);
        shared_.output_period_frames.store(device_->period_frames(), std::memory_order_relaxed);
        shared_.output_period_microseconds.store(device_->period_microseconds(), std::memory_order_relaxed);
        shared_.output_device_name.store(device_->name());
        shared_.output_available.store(true, std::memory_order_release);
        shared_.output_configuration.fetch_add(1, std::memory_order_acq_rel);
        wakeup_.notify();
        return {};
    }

    void mark_device_lost() {
        shared_.output_available.store(false, std::memory_order_release);
        shared_.device_lost.store(true, std::memory_order_release);
        wakeup_.notify();
    }

    Result<void> request_source(SourceOrigin origin) {
        try {
            const std::uint64_t generation = shared_.requested_generation.load(std::memory_order_relaxed) + 1;
            shared_.requested_generation.store(generation, std::memory_order_release);
            controller_->request_source(generation, std::move(origin));
            return {};
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::ResourceLimit, "out of memory queuing the preview source");
        }
    }

    const PreviewOptions options_;
    SharedPlaybackState shared_;
    PcmRing ring_;
    CommandMailbox mailbox_;
    WakeupSignal wakeup_;
    PlaybackRenderer renderer_;
    std::unique_ptr<OutputContext> context_;  ///< null for the Offline backend
    std::unique_ptr<OutputDevice> device_;    ///< null while no endpoint is open
    std::unique_ptr<PlaybackController> controller_;
};

PreviewSnapshot MiniaudioPreviewEngine::snapshot() const noexcept {
    PreviewSnapshot snapshot;
    const std::uint64_t requested = shared_.requested_generation.load(std::memory_order_acquire);
    const bool loading = requested != shared_.active_generation.load(std::memory_order_acquire);
    snapshot.generation = requested;
    snapshot.state = loading ? PreviewState::Loading : shared_.state.load(std::memory_order_acquire);
    snapshot.last_error = loading ? std::nullopt : shared_.last_error.load();

    snapshot.output_device_name = shared_.output_device_name.load();
    snapshot.output_device_available = shared_.output_available.load(std::memory_order_acquire) &&
                                       !shared_.device_lost.load(std::memory_order_acquire);
    snapshot.output_sample_rate = shared_.output_sample_rate.load(std::memory_order_relaxed);
    snapshot.output_period_frames = shared_.output_period_frames.load(std::memory_order_relaxed);
    snapshot.output_period_microseconds = shared_.output_period_microseconds.load(std::memory_order_relaxed);
    snapshot.volume = shared_.target_volume.load(std::memory_order_relaxed);
    snapshot.playback_mode = shared_.playback_mode.load(std::memory_order_relaxed);
    snapshot.speed_ratio = shared_.speed_ratio.load(std::memory_order_relaxed);
    snapshot.pitch_semitones = shared_.pitch_semitones.load(std::memory_order_relaxed);
    snapshot.underrun_count = shared_.underrun_count.load(std::memory_order_relaxed);
    snapshot.dropped_command_count = mailbox_.dropped_count();
    if (loading) {
        return snapshot;  // source fields would still describe the previous generation
    }

    snapshot.source_sample_rate = shared_.source_sample_rate.load(std::memory_order_relaxed);
    snapshot.source_channels = shared_.source_channels.load(std::memory_order_relaxed);
    const std::int64_t total = shared_.total_frames.load(std::memory_order_relaxed);
    if (total >= 0) {
        snapshot.total_frames = total;
        snapshot.total_frames_exact = shared_.total_frames_exact.load(std::memory_order_relaxed);
    }

    const std::uint64_t epoch = shared_.pcm_epoch.load(std::memory_order_acquire);
    const bool played_this_epoch = shared_.played_epoch.load(std::memory_order_acquire) == epoch;
    snapshot.position_frames = played_this_epoch ? shared_.played_frame.load(std::memory_order_relaxed)
                                                 : shared_.epoch_start_frame.load(std::memory_order_relaxed);
    const std::int64_t produced = shared_.epoch_produced_frames.load(std::memory_order_acquire);
    const std::int64_t played = played_this_epoch ? shared_.played_epoch_frames.load(std::memory_order_relaxed) : 0;
    const std::int64_t buffered = std::max<std::int64_t>(0, produced - played);
    if (snapshot.output_sample_rate != 0) {
        snapshot.buffered_milliseconds =
            static_cast<std::uint32_t>(buffered * 1000 / std::int64_t{snapshot.output_sample_rate});
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
        return fail(ErrorCode::ResourceLimit, "cannot start the preview decoder thread");
    }
}

}  // namespace mso::audio
