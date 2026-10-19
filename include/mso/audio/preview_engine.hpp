#pragma once
/// @file
/// Preview audio engine: streams one selected sample at a time to the output device.
///
/// Threads (docs/02_ARCHITECTURE.md §3, §8):
///  - control thread (the UI thread): open(), device selection and restart, post(), snapshot().
///    create() and destruction may happen on any thread (a worker pool may create the engine and
///    the UI thread destroy it), just never at the same time as another call;
///  - one decoder thread per engine: opens and decodes the source, folds channels to stereo,
///    resamples to the device rate and keeps the PCM ring prebuffered (at most 250 ms);
///  - one output thread per engine (System and Null backends): every audio backend and device call
///    runs there, so the backend's per-thread setup (COM on Windows) is made and undone on the
///    same thread; it also re-opens a lost endpoint by itself, with backoff;
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
    std::string value;  ///< "<backend>:<native id>", or empty for the system default

    /// True for the "follow the system default" id.
    bool is_system_default() const noexcept { return value.empty(); }
    bool operator==(const OutputDeviceId&) const = default;
};

/// One playback endpoint as listed by list_output_devices().
struct OutputDeviceInfo {
    OutputDeviceId id;               ///< id to pass to select_output_device() or to persist
    std::string name;                ///< UTF-8 display name
    bool is_system_default = false;  ///< the endpoint the system currently uses by default
};

/// Engine configuration. Out-of-range values are clamped to the documented limits.
struct PreviewOptions {
    OutputBackend backend = OutputBackend::System;  ///< where audio goes
    /// Endpoint to open; empty follows the system default. When it cannot be opened the system
    /// default plays instead, and the choice is kept for the next restart or selection.
    OutputDeviceId device;
    /// Requested device period, clamped to 20..60 ms. The actual period is reported in snapshots.
    std::uint32_t period_milliseconds = 20;
    /// Decoded audio kept ahead of the callback, clamped to 20..250 ms.
    std::uint32_t prebuffer_milliseconds = 200;
    /// Output rate; 0 uses the device's native rate (48 kHz for the Null and Offline backends).
    std::uint32_t sample_rate = 0;
    /// After the endpoint is lost, the first automatic re-open waits this long (clamped to
    /// 10..10,000 ms); each failed attempt doubles the wait...
    std::uint32_t device_retry_first_milliseconds = 500;
    /// ...up to this many milliseconds (clamped to the first wait..60,000 ms). Retries go on until
    /// an endpoint opens.
    std::uint32_t device_retry_longest_milliseconds = 10'000;
};

/// Single-sample preview player. Create with create(); all methods except post() and snapshot()
/// belong to the control thread. post() must be called from one thread at a time; snapshot() is
/// safe from any thread. Destruction closes the device on the output thread and joins the output
/// and decoder threads (bounded).
///
/// Losing the endpoint (headphones unplugged, audio service restarted) is reported in the snapshot
/// (output_device_lost, last_error DEVICE_LOST, state Error). The engine then re-opens the selected
/// endpoint, or the system default when it is gone, by itself with backoff (PreviewOptions); the
/// transport comes back where it was heard, with Playing turned into Paused. restart_output_device()
/// retries at once and also resumes playback.
class PreviewEngine {
public:
    /// Opens the backend and starts the output and decoder threads. If the endpoint cannot be opened
    /// the engine is still returned, with output_device_lost and DEVICE_LOST in its snapshot, and
    /// keeps retrying by itself. Fails with RESOURCE_LIMIT when buffers or threads cannot be
    /// allocated, DEVICE_LOST when the audio subsystem itself is unavailable, and INTERNAL for any
    /// other failure.
    [[nodiscard]] static Result<std::unique_ptr<PreviewEngine>> create(const PreviewOptions& options);

    virtual ~PreviewEngine() = default;
    PreviewEngine(const PreviewEngine&) = delete;
    PreviewEngine& operator=(const PreviewEngine&) = delete;

    /// Playback endpoints of the active backend (empty for Offline).
    [[nodiscard]] virtual Result<std::vector<OutputDeviceInfo>> list_output_devices() = 0;

    /// Switches to another endpoint, remembers it as the selection, and keeps the transport position
    /// (never restarts from the beginning). The current endpoint is only stopped once the new one
    /// has opened, and is restarted if the new one then fails to start. After DEVICE_LOST, a source
    /// that was playing comes back Paused where it was; other states come back unchanged.
    /// INVALID_ARGUMENT for the Offline backend.
    [[nodiscard]] virtual Result<void> select_output_device(const OutputDeviceId& device) = 0;

    /// Re-opens the selected endpoint now (the system default when it is gone), whether or not the
    /// current one still works: the "Retry" after a lost device. The transport resumes as it was,
    /// at the position last heard, and a source that was playing when the endpoint was lost plays
    /// again. On failure the endpoint stays lost, the error is returned and automatic retries go on.
    /// INVALID_ARGUMENT for the Offline backend.
    [[nodiscard]] virtual Result<void> restart_output_device() = 0;

    /// Starts a new source generation for `file`: the decoder thread opens it and prebuffers from
    /// the start. Does not auto-play. Old PCM is discarded; decode errors appear in the snapshot.
    [[nodiscard]] virtual Result<void> open(const std::filesystem::path& file) = 0;

    /// Same as open(path) with an already-opened decoder (tests, in-memory sources).
    [[nodiscard]] virtual Result<void> open(std::unique_ptr<media::Decoder> decoder) = 0;

    /// Queues a transport command through the bounded lock-free mailbox and wakes the decoder thread.
    virtual void post(TransportCommand command) noexcept = 0;

    /// Current transport and device status from atomics only.
    [[nodiscard]] virtual PreviewSnapshot snapshot() const noexcept = 0;

    /// Offline backend only: renders exactly stereo_frames.size() / 2 frames of interleaved stereo
    /// output on the calling thread, as the device callback would. An empty span only lets the
    /// renderer discard PCM of superseded generations.
    [[nodiscard]] virtual Result<void> render_offline(std::span<float> stereo_frames) = 0;

protected:
    PreviewEngine() = default;
};

}  // namespace mso::audio
