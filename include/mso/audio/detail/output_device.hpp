#pragma once
/// @file
/// miniaudio-backed playback endpoints: backend context, enumeration and one open device.
/// Backend-agnostic (the platform's system backend: WASAPI on Windows, PulseAudio/ALSA on Linux;
/// or the null backend). miniaudio itself stays behind pointers, so this header needs no miniaudio.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mso/audio/detail/playback_renderer.hpp"
#include "mso/audio/detail/shared_playback_state.hpp"
#include "mso/audio/preview_engine.hpp"
#include "mso/domain/error.hpp"

namespace mso::audio::detail {

/// Connection to one audio backend. Enumerates playback endpoints and maps persisted
/// OutputDeviceId text to the backend's native id. Used only by the engine's output thread
/// (OutputSupervisor), which also destroys it: WASAPI initializes COM on the creating thread and
/// uninitializes it on the destroying one.
class OutputContext {
public:
    /// Initializes the system backend (or the null backend). Fails with DEVICE_LOST when the
    /// audio subsystem is unavailable.
    [[nodiscard]] static Result<std::unique_ptr<OutputContext>> create(OutputBackend backend);

    ~OutputContext();
    OutputContext(const OutputContext&) = delete;
    OutputContext& operator=(const OutputContext&) = delete;

    /// Playback endpoints of this backend.
    [[nodiscard]] Result<std::vector<OutputDeviceInfo>> list_devices();

private:
    friend class OutputDevice;
    struct Engine;

    OutputContext();

    std::unique_ptr<Engine> engine_;  ///< the ma_context
};

/// Device parameters chosen by the engine.
struct OutputDeviceSettings {
    std::uint32_t sample_rate = 0;           ///< 0 = native rate
    std::uint32_t period_milliseconds = 20;  ///< requested period
};

/// Receives an endpoint's unexpected stop (unplugged, invalidated, or the audio service went away).
/// Called on miniaudio's device thread, never inside the real-time data callback.
class DeviceLossListener {
public:
    /// The endpoint stopped without being asked to.
    virtual void on_device_lost() noexcept = 0;

protected:
    DeviceLossListener() = default;
    ~DeviceLossListener() = default;
    DeviceLossListener(const DeviceLossListener&) = default;
    DeviceLossListener& operator=(const DeviceLossListener&) = default;
};

/// One opened playback endpoint: stereo float32 in shared mode. Its real-time callback feeds the
/// PlaybackRenderer; an unexpected stop (endpoint unplugged or invalidated) is reported to the
/// DeviceLossListener. Opened stopped; start() starts it. stop() pauses it deliberately (not
/// reported as a loss), so a device switch can fall back to it. Output thread only.
class OutputDevice {
public:
    /// Opens (without starting) the endpoint `device` of `context`. NOT_FOUND for an unknown id,
    /// DEVICE_LOST when the endpoint refuses to open. `renderer` and `loss_listener` must outlive it.
    [[nodiscard]] static Result<std::unique_ptr<OutputDevice>> open(OutputContext& context,
                                                                    const OutputDeviceId& device,
                                                                    const OutputDeviceSettings& settings,
                                                                    PlaybackRenderer& renderer,
                                                                    DeviceLossListener& loss_listener);

    /// Stops the callback (joining miniaudio's device thread) and closes the endpoint.
    ~OutputDevice();
    OutputDevice(const OutputDevice&) = delete;
    OutputDevice& operator=(const OutputDevice&) = delete;

    /// Configures the renderer for this device (rate, latency) and starts the callback.
    [[nodiscard]] Result<void> start();
    /// Stops the callback deliberately; returns once no callback runs. start() resumes it.
    void stop() noexcept;

    /// Rate, period, latency and name of the endpoint, for SharedPlaybackState::output_facts.
    [[nodiscard]] OutputFacts facts() const;

private:
    struct Engine;

    OutputDevice();

    std::unique_ptr<Engine> engine_;  ///< the ma_device and what its callbacks need
};

}  // namespace mso::audio::detail
