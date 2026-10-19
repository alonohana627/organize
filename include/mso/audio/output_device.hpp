#pragma once
/// @file
/// miniaudio-backed playback endpoints: backend context, enumeration and one open device.
/// Internal to mso_audio (exposes miniaudio types). Backend-agnostic: the platform's system
/// backend (WASAPI on Windows, PulseAudio/ALSA on Linux) or the null backend.
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <miniaudio.h>

#include "mso/audio/playback_renderer.hpp"
#include "mso/audio/preview_engine.hpp"
#include "mso/audio/shared_playback_state.hpp"
#include "mso/audio/wakeup_signal.hpp"
#include "mso/domain/error.hpp"

namespace mso::audio {

/// Connection to one audio backend. Enumerates playback endpoints and maps persisted
/// OutputDeviceId text to the backend's native id. Control thread only.
class OutputContext {
public:
    /// Initializes the system backend (or the null backend). Fails with DEVICE_LOST when the
    /// audio subsystem is unavailable.
    static Result<std::unique_ptr<OutputContext>> create(OutputBackend backend);

    ~OutputContext();
    OutputContext(const OutputContext&) = delete;
    OutputContext& operator=(const OutputContext&) = delete;

    Result<std::vector<OutputDeviceInfo>> list_devices();

    /// Native id for `device`, or nullopt for the system default. NOT_FOUND when it is not present.
    Result<std::optional<ma_device_id>> resolve(const OutputDeviceId& device);

    ma_context* handle() noexcept { return &context_; }

private:
    OutputContext() = default;

    /// "<backend>:<native id>" text for a native id of this context's backend.
    std::string id_text(const ma_device_id& id) const;

    ma_context context_{};
    bool initialized_ = false;
};

/// Device parameters chosen by the engine.
struct OutputDeviceSettings {
    std::uint32_t sample_rate = 0;  ///< 0 = native rate
    std::uint32_t period_milliseconds = 20;
};

/// One opened playback endpoint: stereo float32 in shared mode. Its real-time callback feeds the
/// PlaybackRenderer; an unexpected stop (endpoint unplugged or invalidated) raises
/// SharedPlaybackState::device_lost and wakes the decoder thread. Opened stopped; start() starts it.
class OutputDevice {
public:
    static Result<std::unique_ptr<OutputDevice>> open(OutputContext& context, const OutputDeviceId& device,
                                                      const OutputDeviceSettings& settings, PlaybackRenderer& renderer,
                                                      SharedPlaybackState& shared, WakeupSignal& wakeup);

    /// Stops the callback (joining miniaudio's device thread) and closes the endpoint.
    ~OutputDevice();
    OutputDevice(const OutputDevice&) = delete;
    OutputDevice& operator=(const OutputDevice&) = delete;

    /// Configures the renderer for this device's rate and starts the callback.
    Result<void> start();

    /// Rate of the stereo stream the callback produces.
    std::uint32_t sample_rate() const noexcept { return device_.sampleRate; }
    /// Actual period negotiated with the endpoint, in endpoint frames.
    std::uint32_t period_frames() const noexcept { return device_.playback.internalPeriodSizeInFrames; }
    std::uint32_t period_microseconds() const noexcept;
    std::string name();

private:
    OutputDevice(PlaybackRenderer& renderer, SharedPlaybackState& shared, WakeupSignal& wakeup) noexcept
        : renderer_(renderer), shared_(shared), wakeup_(wakeup) {}

    static void on_audio(ma_device* device, void* output, const void* input, ma_uint32 frame_count);
    static void on_notification(const ma_device_notification* notification);

    PlaybackRenderer& renderer_;
    SharedPlaybackState& shared_;
    WakeupSignal& wakeup_;
    std::atomic<bool> closing_{false};  ///< set before uninit so our own stop is not reported as a loss
    bool initialized_ = false;
    ma_device device_{};  ///< holds pointers into itself: the object never moves
};

}  // namespace mso::audio
