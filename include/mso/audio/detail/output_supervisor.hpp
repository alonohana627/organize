#pragma once
/// @file
/// The preview engine's output thread: owns the audio backend and the open endpoint, serves the
/// control thread's device requests, and re-opens a lost endpoint by itself with backoff.
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <vector>

#include "mso/audio/detail/output_device.hpp"
#include "mso/audio/detail/playback_renderer.hpp"
#include "mso/audio/detail/shared_playback_state.hpp"
#include "mso/audio/detail/wakeup_signal.hpp"
#include "mso/audio/preview_engine.hpp"
#include "mso/domain/error.hpp"

namespace mso::audio::detail {

/// When the output thread retries a lost endpoint: `first_delay` after the loss, then twice as long
/// after every failed attempt, never waiting longer than `longest_delay`.
struct OutputRetrySchedule {
    std::chrono::milliseconds first_delay{500};       ///< wait before the first attempt
    std::chrono::milliseconds longest_delay{10'000};  ///< cap of the doubling

    /// Wait before the attempt that follows `failed_attempts` failures (0 for the first attempt).
    [[nodiscard]] std::chrono::milliseconds delay_after(std::uint32_t failed_attempts) const noexcept;
};

/// How the output thread opens endpoints.
struct OutputSupervisorOptions {
    OutputBackend backend = OutputBackend::System;  ///< System or Null (Offline has no output thread)
    OutputDeviceId device;                          ///< endpoint to open first; empty = system default
    OutputDeviceSettings device_settings;           ///< rate and period of every endpoint opened
    OutputRetrySchedule retry;                      ///< automatic re-open after a loss
};

/// Runs every miniaudio context and device call of a System or Null engine on one thread of its own:
/// backend creation, enumeration, open, start, stop and close. That keeps the backend's per-thread
/// setup balanced whichever threads create, use and destroy the engine (WASAPI initializes COM on
/// the thread that creates the context and uninitializes it on the thread that destroys it).
///
/// The control thread's requests (list, select, restart) run there while the caller waits for the
/// result. When the endpoint is lost (SharedPlaybackState::device_lost), the thread re-opens the
/// selected endpoint, falling back to the system default, on the OutputRetrySchedule until one
/// starts. A new endpoint is published like a device switch: the decoder thread re-anchors at the
/// audible position and restores the transport (see PlaybackController).
///
/// The renderer never runs on two callbacks: a lost endpoint is closed, and a working one stopped,
/// before its replacement starts.
class OutputSupervisor final : public DeviceLossListener {
public:
    /// Starts the output thread, initializes the backend there and opens `options.device` (falling
    /// back to the system default). Fails with DEVICE_LOST when the audio subsystem itself is
    /// unavailable. An endpoint that cannot be opened is not a failure: it is recorded as lost and
    /// retried automatically. `renderer`, `shared` and `decoder_wakeup` must outlive the supervisor.
    [[nodiscard]] static Result<std::unique_ptr<OutputSupervisor>> start(const OutputSupervisorOptions& options,
                                                                         PlaybackRenderer& renderer,
                                                                         SharedPlaybackState& shared,
                                                                         WakeupSignal& decoder_wakeup);

    /// Closes the endpoint and the backend on the output thread, then joins it.
    ~OutputSupervisor();
    OutputSupervisor(const OutputSupervisor&) = delete;
    OutputSupervisor& operator=(const OutputSupervisor&) = delete;

    /// Playback endpoints of the backend.
    [[nodiscard]] Result<std::vector<OutputDeviceInfo>> list_devices();

    /// Switches to `device` and remembers it as the selection. A working current endpoint keeps
    /// playing when the new one fails to open, and is restarted when it fails to start.
    [[nodiscard]] Result<void> select(const OutputDeviceId& device);

    /// Re-opens the selected endpoint (or the system default when it is gone) whether or not the
    /// current one still works, and asks the decoder thread to bring the transport back exactly as
    /// it was, Playing included. On failure the endpoint counts as lost and automatic retries go on.
    [[nodiscard]] Result<void> restart();

    /// Reports the endpoint as lost: raises SharedPlaybackState::device_lost and wakes the decoder
    /// thread (which flushes and remembers the audible position) and the output thread (which
    /// schedules the re-open). Called by the endpoint's stop notification on miniaudio's thread.
    void on_device_lost() noexcept override;

private:
    OutputSupervisor(const OutputSupervisorOptions& options, PlaybackRenderer& renderer, SharedPlaybackState& shared,
                     WakeupSignal& decoder_wakeup);

    /// Runs `work` on the output thread and returns its result (exceptions included) to the caller.
    template <class Work>
    std::invoke_result_t<Work&> run_on_output_thread(Work work);

    /// Thread body: requests, then the retry schedule, then sleep until the next of either.
    void run(std::stop_token stop);
    /// Runs every queued request, in order.
    void run_requests();
    /// Re-opens a lost endpoint when its attempt is due; keeps the schedule otherwise.
    void retry_if_due();
    /// Time until the next retry attempt is due, or a bounded idle wait without one.
    [[nodiscard]] std::chrono::microseconds time_until_next_wake() const;

    /// Creates the backend context and opens the first endpoint.
    [[nodiscard]] Result<void> initialize();
    /// Opens and starts `device` in place of the current endpoint (see select()).
    [[nodiscard]] Result<void> switch_to(const OutputDeviceId& device);
    /// switch_to() the selection, then the system default when the selection fails.
    [[nodiscard]] Result<void> reopen_selected();
    /// Publishes the open endpoint's facts and tells the decoder thread to adopt them.
    void publish_device();
    /// Records that no endpoint is usable.
    void mark_device_lost() noexcept;

    const OutputSupervisorOptions options_;  ///< backend, first endpoint, settings and retry schedule
    PlaybackRenderer& renderer_;             ///< what every endpoint's callback runs
    SharedPlaybackState& shared_;            ///< device status for the decoder thread and snapshots
    WakeupSignal& decoder_wakeup_;           ///< wakes the decoder thread on device changes
    WakeupSignal wakeup_;                    ///< wakes this thread: requests, losses, shutdown

    std::mutex requests_mutex_;                        ///< guards requests_ only; never held while one runs
    std::deque<std::packaged_task<void()>> requests_;  ///< control-thread requests waiting to run

    // Output-thread state.
    std::unique_ptr<OutputContext> context_;  ///< the backend; null until initialize()
    std::unique_ptr<OutputDevice> device_;    ///< the open endpoint; null while none is open
    OutputDeviceId selected_;                 ///< endpoint the user chose (empty = system default)
    std::uint32_t failed_attempts_ = 0;       ///< failed re-opens since the loss
    std::optional<std::chrono::steady_clock::time_point> next_attempt_;  ///< due re-open, while lost

    std::jthread thread_;  ///< last member: starts after, and is joined before, everything above
};

}  // namespace mso::audio::detail
