#pragma once
/// @file
/// Preview audio engine: streams one selected sample at a time to the output device.
///
/// Threads (docs/02_ARCHITECTURE.md §3, §8):
///  - control thread (the UI thread): create/destroy, open(), device selection, post(), snapshot();
///  - one decoder thread per engine: opens and decodes the source, folds channels to stereo,
///    resamples to the device rate and keeps the PCM ring prebuffered (at most 250 ms);
///  - the device's real-time callback: reads the ring, applies volume, writes the output. It never
///    allocates, locks, logs, touches files or decodes.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "mso/audio/preview_snapshot.hpp"
#include "mso/audio/transport.hpp"
#include "mso/domain/error.hpp"
#include "mso/media/media.hpp"

namespace mso::audio {

/// Where the engine sends its audio. Chosen at runtime, so tests and headless runs need no sound card.
enum class OutputBackend : std::uint8_t {
    /// The platform's shared-mode output at the device's native rate (the product default):
    /// WASAPI on Windows, PulseAudio then ALSA on Linux.
    System,
    Null,     ///< miniaudio's null device on every platform: real-time paced callbacks, output discarded
    Offline,  ///< no device: render_offline() runs the output callback on the caller's thread
};

/// Persistable endpoint identifier, "<backend>:<native id>" (e.g. the WASAPI endpoint ID or the
/// PulseAudio sink name). Empty means "the system default endpoint, following changes".
struct OutputDeviceId {
    std::string value;

    bool is_system_default() const noexcept { return value.empty(); }
    bool operator==(const OutputDeviceId&) const = default;
};

/// One playback endpoint as listed by list_output_devices().
struct OutputDeviceInfo {
    OutputDeviceId id;
    std::string name;  ///< UTF-8 display name
    bool is_system_default = false;
};

/// Engine configuration. Out-of-range values are clamped to the documented limits.
struct PreviewOptions {
    OutputBackend backend = OutputBackend::System;
    /// Endpoint to open; empty follows the system default.
    OutputDeviceId device;
    /// Requested device period, clamped to 20..60 ms. The actual period is reported in snapshots.
    std::uint32_t period_milliseconds = 20;
    /// Decoded audio kept ahead of the callback, clamped to 20..250 ms.
    std::uint32_t prebuffer_milliseconds = 200;
    /// Output rate; 0 uses the device's native rate (48 kHz for the Null and Offline backends).
    std::uint32_t sample_rate = 0;
};

/// Single-sample preview player. Create with create(); all methods except post() and snapshot()
/// belong to the control thread. post() must be called from one thread at a time; snapshot() is
/// safe from any thread. Destruction stops the device and joins the decoder thread (bounded).
class PreviewEngine {
public:
    /// Opens the backend and starts the decoder thread. If the endpoint cannot be opened the engine
    /// is still returned, with DEVICE_LOST in its snapshot, so select_output_device() can retry.
    /// Fails with RESOURCE_LIMIT when buffers cannot be allocated and DEVICE_LOST when the audio
    /// subsystem itself is unavailable.
    static Result<std::unique_ptr<PreviewEngine>> create(const PreviewOptions& options);

    virtual ~PreviewEngine() = default;
    PreviewEngine(const PreviewEngine&) = delete;
    PreviewEngine& operator=(const PreviewEngine&) = delete;

    /// Playback endpoints of the active backend (empty for Offline).
    virtual Result<std::vector<OutputDeviceInfo>> list_output_devices() = 0;

    /// Switches to another endpoint and keeps the transport position (never restarts from the
    /// beginning). After a DEVICE_LOST error a loaded source comes back Paused where it was.
    /// On failure the previously open endpoint, if any, stays in use.
    virtual Result<void> select_output_device(const OutputDeviceId& device) = 0;

    /// Starts a new source generation for `file`: the decoder thread opens it and prebuffers from
    /// the start. Does not auto-play. Old PCM is discarded; decode errors appear in the snapshot.
    virtual Result<void> open(const std::filesystem::path& file) = 0;

    /// Same as open(path) with an already-opened decoder (tests, in-memory sources).
    virtual Result<void> open(std::unique_ptr<media::Decoder> decoder) = 0;

    /// Queues a transport command through the bounded lock-free mailbox and wakes the decoder thread.
    virtual void post(TransportCommand command) noexcept = 0;

    /// Current transport and device status from atomics only.
    virtual PreviewSnapshot snapshot() const noexcept = 0;

    /// Offline backend only: renders exactly stereo_frames.size() / 2 frames of interleaved stereo
    /// output on the calling thread, as the device callback would. An empty span only lets the
    /// renderer discard PCM of superseded generations.
    virtual Result<void> render_offline(std::span<float> stereo_frames) = 0;

protected:
    PreviewEngine() = default;
};

}  // namespace mso::audio
