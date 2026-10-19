#include "mso/audio/detail/output_device.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <new>
#include <optional>
#include <span>

#include <miniaudio.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mso::audio::detail {
namespace {

/// Backends tried for OutputBackend::System, in priority order. Backends not compiled into the
/// vendored miniaudio are skipped by ma_context_init.
std::vector<ma_backend> system_backends() {
#if defined(_WIN32)
    return {ma_backend_wasapi};
#elif defined(__linux__)
    return {ma_backend_pulseaudio, ma_backend_alsa};
#else
    return {};  // miniaudio's default order for the platform
#endif
}

/// Text of a fixed-size, NUL-padded char id field.
std::string text_of(const char* field, std::size_t capacity) {
    return std::string(field, strnlen(field, capacity));
}

#ifdef _WIN32
/// UTF-8 form of a WASAPI endpoint id (UTF-16, NUL-terminated within its fixed array).
std::string utf8_of_endpoint_id(const ma_wchar_win32* wide, std::size_t capacity) {
    const int wide_length = static_cast<int>(wcsnlen(wide, capacity));
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, wide_length, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide, wide_length, utf8.data(), bytes, nullptr, nullptr);
    }
    return utf8;
}
#endif

/// `frames` at `from_rate` expressed at `to_rate`.
std::uint32_t frames_at_rate(std::uint64_t frames, std::uint64_t from_rate, std::uint64_t to_rate) noexcept {
    if (from_rate == 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(frames * to_rate / from_rate);
}

}  // namespace

// ---------------------------------------------------------------- OutputContext

/// The miniaudio context and the id mapping that needs it.
struct OutputContext::Engine {
    ma_context context{};      ///< valid once `initialized`
    bool initialized = false;  ///< ma_context_init succeeded

    Engine() = default;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine() {
        if (initialized) {
            ma_context_uninit(&context);
        }
    }

    /// "<backend>:<native id>" text for a native id of this context's backend.
    std::string id_text(const ma_device_id& id) const {
        switch (context.backend) {
#ifdef _WIN32
            case ma_backend_wasapi:
                return "wasapi:" + utf8_of_endpoint_id(id.wasapi, std::size(id.wasapi));
#endif
            case ma_backend_pulseaudio:
                return "pulse:" + text_of(id.pulse, sizeof(id.pulse));
            case ma_backend_alsa:
                return "alsa:" + text_of(id.alsa, sizeof(id.alsa));
            case ma_backend_null:
                return "null:" + std::to_string(id.nullbackend);
            default:
                return {};
        }
    }

    /// Current playback endpoints (miniaudio owns the array until the next enumeration).
    Result<std::span<const ma_device_info>> playback_devices() {
        ma_device_info* devices = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(&context, &devices, &count, nullptr, nullptr) != MA_SUCCESS) {
            return fail(ErrorCode::DeviceLost, "cannot enumerate output devices", true);
        }
        return std::span<const ma_device_info>(devices, count);
    }

    /// Native id for `device`, or nullopt for the system default. NOT_FOUND when it is not present.
    Result<std::optional<ma_device_id>> resolve(const OutputDeviceId& device) {
        if (device.is_system_default()) {
            return std::optional<ma_device_id>{};
        }
        auto devices = playback_devices();
        if (!devices) {
            return std::unexpected(devices.error());
        }
        for (const ma_device_info& info : *devices) {
            if (id_text(info.id) == device.value) {
                return std::optional<ma_device_id>{info.id};
            }
        }
        return fail(ErrorCode::NotFound, "output device is not connected", true);
    }
};

OutputContext::OutputContext() = default;

OutputContext::~OutputContext() = default;

Result<std::unique_ptr<OutputContext>> OutputContext::create(OutputBackend backend) {
    std::unique_ptr<OutputContext> context(new (std::nothrow) OutputContext());
    if (!context) {
        return fail(ErrorCode::ResourceLimit, "out of memory for the audio context");
    }
    context->engine_.reset(new (std::nothrow) Engine());
    if (!context->engine_) {
        return fail(ErrorCode::ResourceLimit, "out of memory for the audio context");
    }
    const std::vector<ma_backend> backends =
        backend == OutputBackend::Null ? std::vector<ma_backend>{ma_backend_null} : system_backends();
    const ma_context_config config = ma_context_config_init();
    if (ma_context_init(backends.empty() ? nullptr : backends.data(), static_cast<ma_uint32>(backends.size()), &config,
                        &context->engine_->context) != MA_SUCCESS) {
        return fail(ErrorCode::DeviceLost, "no audio output backend is available");
    }
    context->engine_->initialized = true;
    return context;
}

Result<std::vector<OutputDeviceInfo>> OutputContext::list_devices() {
    auto devices = engine_->playback_devices();
    if (!devices) {
        return std::unexpected(devices.error());
    }
    std::vector<OutputDeviceInfo> result;
    result.reserve(devices->size());
    for (const ma_device_info& info : *devices) {
        result.push_back(OutputDeviceInfo{OutputDeviceId{engine_->id_text(info.id)},
                                          text_of(info.name, sizeof(info.name)), info.isDefault != MA_FALSE});
    }
    return result;
}

// ---------------------------------------------------------------- OutputDevice

/// The miniaudio device and what its callbacks reach through pUserData. Fixed heap address:
/// ma_device holds pointers into itself and miniaudio holds a pointer to this object.
struct OutputDevice::Engine {
    Engine(PlaybackRenderer& playback_renderer, OutputDeviceListener& device_listener,
           bool opened_as_system_default) noexcept
        : renderer(playback_renderer), listener(device_listener), follows_system_default(opened_as_system_default) {}
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine() {
        expecting_stop.store(true, std::memory_order_release);
        if (initialized) {
            ma_device_uninit(&device);
        }
    }

    /// Real-time callback: renders straight into miniaudio's output buffer and counts itself for
    /// the output thread's watchdog (one relaxed, lock-free increment).
    static void on_audio(ma_device* device, void* output, const void* /*input*/, ma_uint32 frame_count) {
        auto* engine = static_cast<Engine*>(device->pUserData);
        engine->renderer.render(
            std::span<float>(static_cast<float*>(output), std::size_t{frame_count} * kOutputChannels));
        engine->callback_count.fetch_add(1, std::memory_order_relaxed);
    }

    /// Device events, passed on to the listener. A stop this object asked for is not an event.
    static void on_notification(const ma_device_notification* notification) {
        auto* engine = static_cast<Engine*>(notification->pDevice->pUserData);
        switch (notification->type) {
            case ma_device_notification_type_started:
                engine->listener.on_output_device_event(OutputDeviceEvent::Started);
                break;
            case ma_device_notification_type_stopped:
                if (!engine->expecting_stop.load(std::memory_order_acquire)) {
                    engine->listener.on_output_device_event(OutputDeviceEvent::Stopped);
                }
                break;
            case ma_device_notification_type_rerouted:
                engine->listener.on_output_device_event(OutputDeviceEvent::Rerouted);
                break;
            default:
                break;  // interruptions and unlocks concern mobile platforms only
        }
    }

    PlaybackRenderer& renderer;                    ///< what the callback runs
    OutputDeviceListener& listener;                ///< told about the backend's events
    const bool follows_system_default;             ///< opened without an explicit endpoint id
    std::atomic<bool> expecting_stop{true};        ///< our own stop/close: not an event
    std::atomic<std::uint64_t> callback_count{0};  ///< data callbacks run so far
    bool initialized = false;                      ///< ma_device_init succeeded
    ma_device device{};                            ///< the endpoint
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "the callback counts itself lock-free");

OutputDevice::OutputDevice() = default;

OutputDevice::~OutputDevice() = default;

Result<std::unique_ptr<OutputDevice>> OutputDevice::open(OutputContext& context, const OutputDeviceId& device,
                                                         const OutputDeviceSettings& settings,
                                                         PlaybackRenderer& renderer, OutputDeviceListener& listener) {
    auto native_id = context.engine_->resolve(device);
    if (!native_id) {
        return std::unexpected(native_id.error());
    }
    std::unique_ptr<OutputDevice> output(new (std::nothrow) OutputDevice());
    if (!output) {
        return fail(ErrorCode::ResourceLimit, "out of memory for the output device");
    }
    output->engine_.reset(new (std::nothrow) Engine(renderer, listener, device.is_system_default()));
    if (!output->engine_) {
        return fail(ErrorCode::ResourceLimit, "out of memory for the output device");
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = static_cast<ma_uint32>(kOutputChannels);
    config.playback.pDeviceID = *native_id ? &**native_id : nullptr;  // copied during init
    config.playback.shareMode = ma_share_mode_shared;
    config.sampleRate = settings.sample_rate;
    config.periodSizeInMilliseconds = settings.period_milliseconds;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.noPreSilencedOutputBuffer = MA_TRUE;  // the renderer writes every sample
    config.noClip = MA_TRUE;                     // the renderer clamps already
    config.dataCallback = &Engine::on_audio;
    config.notificationCallback = &Engine::on_notification;
    config.pUserData = output->engine_.get();

    if (ma_device_init(&context.engine_->context, &config, &output->engine_->device) != MA_SUCCESS) {
        return fail(ErrorCode::DeviceLost, "cannot open the output device", true);
    }
    output->engine_->initialized = true;
    return output;
}

Result<void> OutputDevice::start() {
    const OutputFacts device_facts = facts();
    engine_->renderer.configure(device_facts.sample_rate, device_facts.latency_frames);
    engine_->expecting_stop.store(false, std::memory_order_release);
    if (ma_device_start(&engine_->device) != MA_SUCCESS) {
        engine_->expecting_stop.store(true, std::memory_order_release);
        return fail(ErrorCode::DeviceLost, "cannot start the output device", true);
    }
    return {};
}

void OutputDevice::stop() noexcept {
    engine_->expecting_stop.store(true, std::memory_order_release);
    // A failed stop means the device is already stopped or gone; either way no callback runs after
    // this returns, which is all a caller relies on.
    (void)ma_device_stop(&engine_->device);
}

OutputDeviceHealth OutputDevice::health() const noexcept {
    OutputDeviceHealth result;
    result.running = ma_device_get_state(&engine_->device) == ma_device_state_started;
    result.callback_count = engine_->callback_count.load(std::memory_order_relaxed);
    return result;
}

bool OutputDevice::follows_system_default() const noexcept {
    return engine_->follows_system_default;
}

OutputFacts OutputDevice::facts() const {
    const ma_device& device = engine_->device;
    OutputFacts result;
    result.sample_rate = device.sampleRate;
    result.period_frames = device.playback.internalPeriodSizeInFrames;
    const std::uint64_t internal_rate = device.playback.internalSampleRate;
    result.period_microseconds = frames_at_rate(result.period_frames, internal_rate, 1'000'000);
    // The device buffer (all periods) is what lies between the callback and the speaker.
    const std::uint64_t buffer_frames = std::uint64_t{device.playback.internalPeriodSizeInFrames} *
                                        std::max<ma_uint32>(device.playback.internalPeriods, 1);
    result.latency_frames = frames_at_rate(buffer_frames, internal_rate, device.sampleRate);
    result.latency_microseconds = frames_at_rate(buffer_frames, internal_rate, 1'000'000);

    std::array<char, MA_MAX_DEVICE_NAME_LENGTH + 1> name{};
    if (ma_device_get_name(&engine_->device, ma_device_type_playback, name.data(), name.size(), nullptr) ==
        MA_SUCCESS) {
        result.name = FixedText<kOutputDeviceNameCapacity>::truncated_from(text_of(name.data(), name.size()));
    } else {
        result.name = FixedText<kOutputDeviceNameCapacity>::truncated_from(
            text_of(device.playback.name, sizeof(device.playback.name)));
    }
    return result;
}

}  // namespace mso::audio::detail
