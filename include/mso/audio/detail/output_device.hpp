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

/// What the backend reported about an open endpoint, outside the real-time data callback.
enum class OutputDeviceEvent : std::uint8_t {
    /// The endpoint started (also when the backend restarts it by itself after rerouting it).
    Started,
    /// The endpoint stopped without being asked to. Not by itself a loss: the backend also stops a
    /// system-default endpoint while it moves it to a new default device (Windows does this when a
    /// headset is plugged in), and starts it again afterwards. The output thread decides from the
    /// endpoint's health (OutputWatchdog).
    Stopped,
    /// The backend moved a system-default endpoint to the new default device. The endpoint keeps
    /// its stream rate; its name, period and latency may have changed.
    Rerouted,
};

/// Receives an open endpoint's events. Called on miniaudio's device and notification threads,
/// never inside the real-time data callback; implementations only record the event and wake the
/// output thread.
class OutputDeviceListener {
public:
    /// The backend reported `event`.
    virtual void on_output_device_event(OutputDeviceEvent event) noexcept = 0;

protected:
    OutputDeviceListener() = default;
    ~OutputDeviceListener() = default;
    OutputDeviceListener(const OutputDeviceListener&) = default;
    OutputDeviceListener& operator=(const OutputDeviceListener&) = default;
};

/// What the output thread can observe of an open endpoint at any moment, without help from the
/// backend's notifications (which are not sent when a stop fails, for example after an unplug).
struct OutputDeviceHealth {
    bool running = false;              ///< the backend reports the endpoint started
    std::uint64_t callback_count = 0;  ///< data callbacks so far; stops advancing when the endpoint stalls
};

/// One opened playback endpoint: stereo float32 in shared mode. Its real-time callback feeds the
/// PlaybackRenderer and counts itself (health()); the backend's events (an unexpected stop, a
/// reroute) go to the OutputDeviceListener. Opened stopped; start() starts it. stop() pauses it
/// deliberately (never reported as an event), so a device switch can fall back to it. Output
/// thread only, except health(), which any thread may call.
class OutputDevice {
public:
    /// Opens (without starting) the endpoint `device` of `context`. NOT_FOUND for an unknown id,
    /// DEVICE_LOST when the endpoint refuses to open. `renderer` and `listener` must outlive it.
    [[nodiscard]] static Result<std::unique_ptr<OutputDevice>> open(OutputContext& context,
                                                                    const OutputDeviceId& device,
                                                                    const OutputDeviceSettings& settings,
                                                                    PlaybackRenderer& renderer,
                                                                    OutputDeviceListener& listener);

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

    /// Whether the backend reports the endpoint started, and how many data callbacks have run.
    /// Lock-free; safe from any thread.
    [[nodiscard]] OutputDeviceHealth health() const noexcept;

    /// True when the endpoint was opened as "the system default": the backend may then stop it,
    /// move it to a new default device and start it again by itself (OutputDeviceEvent::Rerouted).
    [[nodiscard]] bool follows_system_default() const noexcept;

private:
    struct Engine;

    OutputDevice();

    std::unique_ptr<Engine> engine_;  ///< the ma_device and what its callbacks need
};

}  // namespace mso::audio::detail
