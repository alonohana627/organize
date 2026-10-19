#include "mso/audio/output_device.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <span>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mso::audio {
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

}  // namespace

// ---------------------------------------------------------------- OutputContext

Result<std::unique_ptr<OutputContext>> OutputContext::create(OutputBackend backend) {
    std::unique_ptr<OutputContext> context(new (std::nothrow) OutputContext());
    if (!context) {
        return fail(ErrorCode::ResourceLimit, "out of memory for the audio context");
    }
    const std::vector<ma_backend> backends =
        backend == OutputBackend::Null ? std::vector<ma_backend>{ma_backend_null} : system_backends();
    const ma_context_config config = ma_context_config_init();
    if (ma_context_init(backends.empty() ? nullptr : backends.data(), static_cast<ma_uint32>(backends.size()), &config,
                        &context->context_) != MA_SUCCESS) {
        return fail(ErrorCode::DeviceLost, "no audio output backend is available");
    }
    context->initialized_ = true;
    return context;
}

OutputContext::~OutputContext() {
    if (initialized_) {
        ma_context_uninit(&context_);
    }
}

Result<std::vector<OutputDeviceInfo>> OutputContext::list_devices() {
    ma_device_info* playback_devices = nullptr;
    ma_uint32 playback_count = 0;
    if (ma_context_get_devices(&context_, &playback_devices, &playback_count, nullptr, nullptr) != MA_SUCCESS) {
        return fail(ErrorCode::DeviceLost, "cannot enumerate output devices", true);
    }
    std::vector<OutputDeviceInfo> devices;
    devices.reserve(playback_count);
    for (const ma_device_info& info : std::span(playback_devices, playback_count)) {
        devices.push_back(OutputDeviceInfo{OutputDeviceId{id_text(info.id)}, text_of(info.name, sizeof(info.name)),
                                           info.isDefault != MA_FALSE});
    }
    return devices;
}

Result<std::optional<ma_device_id>> OutputContext::resolve(const OutputDeviceId& device) {
    if (device.is_system_default()) {
        return std::optional<ma_device_id>{};
    }
    ma_device_info* playback_devices = nullptr;
    ma_uint32 playback_count = 0;
    if (ma_context_get_devices(&context_, &playback_devices, &playback_count, nullptr, nullptr) != MA_SUCCESS) {
        return fail(ErrorCode::DeviceLost, "cannot enumerate output devices", true);
    }
    for (const ma_device_info& info : std::span(playback_devices, playback_count)) {
        if (id_text(info.id) == device.value) {
            return std::optional<ma_device_id>{info.id};
        }
    }
    return fail(ErrorCode::NotFound, "output device is not connected", true);
}

std::string OutputContext::id_text(const ma_device_id& id) const {
    switch (context_.backend) {
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

// ---------------------------------------------------------------- OutputDevice

Result<std::unique_ptr<OutputDevice>> OutputDevice::open(OutputContext& context, const OutputDeviceId& device,
                                                         const OutputDeviceSettings& settings,
                                                         PlaybackRenderer& renderer, SharedPlaybackState& shared,
                                                         WakeupSignal& wakeup) {
    auto native_id = context.resolve(device);
    if (!native_id) {
        return std::unexpected(native_id.error());
    }
    std::unique_ptr<OutputDevice> output(new (std::nothrow) OutputDevice(renderer, shared, wakeup));
    if (!output) {
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
    config.dataCallback = &OutputDevice::on_audio;
    config.notificationCallback = &OutputDevice::on_notification;
    config.pUserData = output.get();

    if (ma_device_init(context.handle(), &config, &output->device_) != MA_SUCCESS) {
        return fail(ErrorCode::DeviceLost, "cannot open the output device", true);
    }
    output->initialized_ = true;
    return output;
}

OutputDevice::~OutputDevice() {
    closing_.store(true, std::memory_order_release);
    if (initialized_) {
        ma_device_uninit(&device_);
    }
}

Result<void> OutputDevice::start() {
    renderer_.configure(sample_rate());
    if (ma_device_start(&device_) != MA_SUCCESS) {
        return fail(ErrorCode::DeviceLost, "cannot start the output device", true);
    }
    return {};
}

std::uint32_t OutputDevice::period_microseconds() const noexcept {
    const std::uint64_t internal_rate = device_.playback.internalSampleRate;
    if (internal_rate == 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::uint64_t{period_frames()} * 1'000'000u / internal_rate);
}

std::string OutputDevice::name() {
    std::array<char, MA_MAX_DEVICE_NAME_LENGTH + 1> buffer{};
    if (ma_device_get_name(&device_, ma_device_type_playback, buffer.data(), buffer.size(), nullptr) != MA_SUCCESS) {
        return text_of(device_.playback.name, sizeof(device_.playback.name));
    }
    return text_of(buffer.data(), buffer.size());
}

void OutputDevice::on_audio(ma_device* device, void* output, const void* /*input*/, ma_uint32 frame_count) {
    auto* self = static_cast<OutputDevice*>(device->pUserData);
    self->renderer_.render(std::span<float>(static_cast<float*>(output), std::size_t{frame_count} * kOutputChannels));
}

void OutputDevice::on_notification(const ma_device_notification* notification) {
    auto* self = static_cast<OutputDevice*>(notification->pDevice->pUserData);
    if (notification->type == ma_device_notification_type_stopped && !self->closing_.load(std::memory_order_acquire)) {
        self->shared_.device_lost.store(true, std::memory_order_release);
        self->wakeup_.notify();
    }
}

}  // namespace mso::audio
